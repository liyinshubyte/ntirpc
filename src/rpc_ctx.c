/*
 * Copyright (c) 2012 Linux Box Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <config.h>

#include <pthread.h>
#include <reentrant.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <stdint.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <rpc/types.h>
#include <unistd.h>
#include <signal.h>
#include <misc/timespec.h>
#include <rpc/xdr.h>
#include <rpc/rpc.h>
#include <rpc/svc.h>
#include "rpc_com.h"
#include <misc/rbtree_x.h>
#include "clnt_internal.h"
#include "rpc_dplx_internal.h"
#include "rpc_ctx.h"

#define tv_to_ms(tv) (1000 * ((tv)->tv_sec) + (tv)->tv_usec/1000)

rpc_ctx_t *
alloc_rpc_call_ctx(CLIENT *clnt, rpcproc_t proc, xdrproc_t xdr_args,
                   void *args_ptr, xdrproc_t xdr_results, void *results_ptr,
                   struct timeval timeout)
{
    struct x_vc_data *xd = (struct x_vc_data *) clnt->cl_p1;
    struct rpc_dplx_rec *rec = xd->rec;
    rpc_ctx_t *ctx;

    ctx = mem_alloc(sizeof(rpc_ctx_t));
    if (! ctx)
        goto out;

    /* rec->calls and rbtree protected by (adaptive) mtx */
    mutex_lock(&rec->mtx);

    /* XXX we hold the client-fd lock */
    ctx->xid = ++(xd->cx.calls.xid);

    /* some of this looks like overkill;  it's here to support future,
     * fully async calls */
    ctx->ctx_u.clnt.clnt = clnt;
    ctx->ctx_u.clnt.timeout.tv_sec = 0;
    ctx->ctx_u.clnt.timeout.tv_nsec = 0;
    timespec_addms(&ctx->ctx_u.clnt.timeout, tv_to_ms(&timeout));
    ctx->ctx_u.clnt.proc = proc;
    ctx->ctx_u.clnt.xdr_args = xdr_args;
    ctx->ctx_u.clnt.args_ptr = args_ptr;
    ctx->ctx_u.clnt.results_ptr = results_ptr;
    ctx->msg = alloc_rpc_msg();
    ctx->flags = 0;

    /* stash it */
    if (opr_rbtree_insert(&xd->cx.calls.t, &ctx->node_k)) {
        __warnx(TIRPC_DEBUG_FLAG_RPC_CTX,
                "%s: call ctx insert failed (xid %d client %p)",
                __func__,
                ctx->xid, clnt);
        mutex_unlock(&rec->mtx);
        mem_free(ctx, sizeof(rpc_ctx_t));
        ctx = NULL;
        goto out;
    }

    mutex_unlock(&rec->mtx);

out:
    return (ctx);
}

void rpc_ctx_next_xid(rpc_ctx_t *ctx, uint32_t flags)
{
    struct x_vc_data *xd = (struct x_vc_data *) ctx->ctx_u.clnt.clnt->cl_p1;
    struct rpc_dplx_rec *rec = xd->rec;

    assert (flags & RPC_CTX_FLAG_LOCKED);

    mutex_lock(&rec->mtx);
    opr_rbtree_remove(&xd->cx.calls.t, &ctx->node_k);
    ctx->xid = ++(xd->cx.calls.xid);
    if (opr_rbtree_insert(&xd->cx.calls.t, &ctx->node_k)) {
        mutex_unlock(&rec->mtx);
        __warnx(TIRPC_DEBUG_FLAG_RPC_CTX,
                "%s: call ctx insert failed (xid %d client %p)",
                __func__,
                ctx->xid,
                ctx->ctx_u.clnt.clnt);
        goto out;
    }
    mutex_unlock(&rec->mtx);
out:
    return;
}

void
rpc_ctx_xfer_callmsg(rpc_ctx_t *ctx)
{
    struct x_vc_data *xd = (struct x_vc_data *) ctx->ctx_u.clnt.clnt->cl_p1;
    struct rpc_dplx_rec *rec  = xd->rec;

    if (rec->hdl.xprt) {
        /* queue msg */
        mutex_lock(&xd->rec->mtx);
        TAILQ_INSERT_TAIL(&xd->sx.msg_q, ctx->msg, msg_q);
        ++(xd->sx.qlen);
        mutex_unlock(&xd->rec->mtx);
        /* reset it */
        ctx->msg = alloc_rpc_msg();
    } else {
        /* unexpected msg (abuse?)--just reuse it */
    }    
}

int
rpc_ctx_wait_reply(rpc_ctx_t *ctx, uint32_t flags)
{
    struct x_vc_data *xd = (struct x_vc_data *) ctx->ctx_u.clnt.clnt->cl_p1;
    struct rpc_dplx_rec *rec  = xd->rec;
    struct timespec ts;
    int code = 0;

    assert (flags & RPC_CTX_FLAG_LOCKED);

    ctx->flags |= RPC_CTX_FLAG_WAITSYNC;
    while (! (ctx->flags & RPC_CTX_FLAG_SYNCDONE)) {
        (void) clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
        timespecadd(&ts, &ctx->ctx_u.clnt.timeout);
        code = cond_timedwait(&ctx->we.cv, &ctx->we.mtx, &ts);
    }

    /* switch on direction */
    switch (ctx->msg->rm_direction) {
    case REPLY:
        if (ctx->msg->rm_xid == ctx->xid)
            return (RPC_SUCCESS);
        break;
    case CALL:
        /* XXX cond transfer control to svc */
        /* TODO: finish */
        break;
    default:
        break;
    }

    return (code);
}

void
free_rpc_call_ctx(rpc_ctx_t *ctx, uint32_t flags)
{
    struct x_vc_data *xd = (struct x_vc_data *) ctx->ctx_u.clnt.clnt->cl_p1;
    struct rpc_dplx_rec *rec  = xd->rec;

    mutex_lock(&rec->mtx);
    opr_rbtree_remove(&xd->cx.calls.t, &ctx->node_k);
    mutex_unlock(&rec->mtx);
    if (ctx->msg)
        free_rpc_msg(ctx->msg);
    mem_free(ctx, sizeof(rpc_ctx_t));
}
