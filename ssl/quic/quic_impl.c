/*
 * Copyright 2022 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include <openssl/macros.h>
#include <openssl/objects.h>
#include <openssl/sslerr.h>
#include <crypto/rand.h>
#include "quic_local.h"
#include "internal/quic_tls.h"
#include "internal/quic_rx_depack.h"
#include "internal/quic_error.h"
#include "internal/time.h"

static void aon_write_finish(QUIC_XSO *xso);
static int create_channel(QUIC_CONNECTION *qc);

/*
 * QUIC Front-End I/O API: Common Utilities
 * ========================================
 */

/*
 * Block until a predicate is met.
 *
 * Precondition: Must have a channel.
 * Precondition: Must hold channel lock (unchecked).
 */
QUIC_NEEDS_LOCK
static int block_until_pred(QUIC_CONNECTION *qc,
                            int (*pred)(void *arg), void *pred_arg,
                            uint32_t flags)
{
    QUIC_REACTOR *rtor;

    assert(qc->ch != NULL);

    rtor = ossl_quic_channel_get_reactor(qc->ch);
    return ossl_quic_reactor_block_until_pred(rtor, pred, pred_arg, flags,
                                              qc->mutex);
}

/*
 * Raise a 'normal' error, meaning one that can be reported via SSL_get_error()
 * rather than via ERR.
 */
static int quic_raise_normal_error(QUIC_CONNECTION *qc,
                                   int err)
{
    qc->last_error = err;
    return 0;
}

/*
 * Raise a 'non-normal' error, meaning any error that is not reported via
 * SSL_get_error() and must be reported via ERR.
 *
 * qc should be provided if available. In exceptional circumstances when qc is
 * not known NULL may be passed. This should generally only happen when an
 * expect_...() function defined below fails, which generally indicates a
 * dispatch error or caller error.
 */
static int quic_raise_non_normal_error(QUIC_CONNECTION *qc,
                                       const char *file,
                                       int line,
                                       const char *func,
                                       int reason,
                                       const char *fmt,
                                       ...)
{
    va_list args;

    ERR_new();
    ERR_set_debug(file, line, func);

    va_start(args, fmt);
    ERR_vset_error(ERR_LIB_SSL, reason, fmt, args);
    va_end(args);

    if (qc != NULL)
        qc->last_error = SSL_ERROR_SSL;

    return 0;
}

#define QUIC_RAISE_NORMAL_ERROR(qc, err)                        \
    quic_raise_normal_error((qc), (err))

#define QUIC_RAISE_NON_NORMAL_ERROR(qc, reason, msg)            \
    quic_raise_non_normal_error((qc),                           \
                                OPENSSL_FILE, OPENSSL_LINE,     \
                                OPENSSL_FUNC,                   \
                                (reason),                       \
                                (msg))

/*
 * QCTX is a utility structure which provides information we commonly wish to
 * unwrap upon an API call being dispatched to us, namely:
 *
 *   - a pointer to the QUIC_CONNECTION (regardless of whether a QCSO or QSSO
 *     was passed);
 *   - a pointer to any applicable QUIC_XSO (e.g. if a QSSO was passed, or if
 *     a QCSO with a default stream was passed);
 *   - whether a QSSO was passed (xso == NULL must not be used to determine this
 *     because it may be non-NULL when a QCSO is passed if that QCSO has a
 *     default stream).
 */
typedef struct qctx_st {
    QUIC_CONNECTION *qc;
    QUIC_XSO        *xso;
    int             is_stream;
} QCTX;

/*
 * Given a QCSO or QSSO, initialises a QCTX, determining the contextually
 * applicable QUIC_CONNECTION pointer and, if applicable, QUIC_XSO pointer.
 *
 * After this returns 1, all fields of the passed QCTX are initialised.
 * Returns 0 on failure. This function is intended to be used to provide API
 * semantics and as such, it invokes QUIC_RAISE_NON_NORMAL_ERROR() on failure.
 */
static int expect_quic(const SSL *s, QCTX *ctx)
{
    QUIC_CONNECTION *qc;
    QUIC_XSO *xso;

    ctx->qc         = NULL;
    ctx->xso        = NULL;
    ctx->is_stream  = 0;

    if (s == NULL)
        return QUIC_RAISE_NON_NORMAL_ERROR(NULL, ERR_R_INTERNAL_ERROR, NULL);

    switch (s->type) {
    case SSL_TYPE_QUIC_CONNECTION:
        qc              = (QUIC_CONNECTION *)s;
        ctx->qc         = qc;
        ctx->xso        = qc->default_xso;
        ctx->is_stream  = 0;
        return 1;

    case SSL_TYPE_QUIC_XSO:
        xso             = (QUIC_XSO *)s;
        ctx->qc         = xso->conn;
        ctx->xso        = xso;
        ctx->is_stream  = 1;
        return 1;

    default:
        return QUIC_RAISE_NON_NORMAL_ERROR(NULL, ERR_R_INTERNAL_ERROR, NULL);
    }
}

/*
 * Like expect_quic(), but requires a QUIC_XSO be contextually available. In
 * other words, requires that the passed QSO be a QSSO or a QCSO with a default
 * stream.
 */
static int ossl_unused expect_quic_with_stream(const SSL *s, QCTX *ctx)
{
    if (!expect_quic(s, ctx))
        return 0;

    if (ctx->xso == NULL)
        return QUIC_RAISE_NON_NORMAL_ERROR(ctx->qc, ERR_R_INTERNAL_ERROR, NULL);

    return 1;
}

/*
 * Like expect_quic(), but fails if called on a QUIC_XSO. ctx->xso may still
 * be non-NULL if the QCSO has a default stream.
 */
static int ossl_unused expect_quic_conn_only(const SSL *s, QCTX *ctx)
{
    if (!expect_quic(s, ctx))
        return 0;

    if (ctx->is_stream)
        return QUIC_RAISE_NON_NORMAL_ERROR(ctx->qc, ERR_R_INTERNAL_ERROR, NULL);

    return 1;
}

/*
 * Ensures that the channel mutex is held for a method which touches channel
 * state.
 *
 * Precondition: Channel mutex is not held (unchecked)
 */
static void quic_lock(QUIC_CONNECTION *qc)
{
    ossl_crypto_mutex_lock(qc->mutex);
}

/* Precondition: Channel mutex is held (unchecked) */
QUIC_NEEDS_LOCK
static void quic_unlock(QUIC_CONNECTION *qc)
{
    ossl_crypto_mutex_unlock(qc->mutex);
}


/*
 * QUIC Front-End I/O API: Initialization
 * ======================================
 *
 *         SSL_new                  => ossl_quic_new
 *                                     ossl_quic_init
 *         SSL_reset                => ossl_quic_reset
 *         SSL_clear                => ossl_quic_clear
 *                                     ossl_quic_deinit
 *         SSL_free                 => ossl_quic_free
 *
 */

/* SSL_new */
SSL *ossl_quic_new(SSL_CTX *ctx)
{
    QUIC_CONNECTION *qc = NULL;
    SSL *ssl_base = NULL;
    SSL_CONNECTION *sc = NULL;

    qc = OPENSSL_zalloc(sizeof(*qc));
    if (qc == NULL)
        goto err;

    /* Initialise the QUIC_CONNECTION's stub header. */
    ssl_base = &qc->ssl;
    if (!ossl_ssl_init(ssl_base, ctx, ctx->method, SSL_TYPE_QUIC_CONNECTION)) {
        ssl_base = NULL;
        goto err;
    }

    qc->tls = ossl_ssl_connection_new_int(ctx, TLS_method());
    if (qc->tls == NULL || (sc = SSL_CONNECTION_FROM_SSL(qc->tls)) == NULL)
         goto err;

    if ((qc->mutex = ossl_crypto_mutex_new()) == NULL)
        goto err;

    qc->is_thread_assisted
        = (ssl_base->method == OSSL_QUIC_client_thread_method());

    qc->as_server       = 0; /* TODO(QUIC): server support */
    qc->as_server_state = qc->as_server;

    qc->default_ssl_mode    = qc->ssl.ctx->mode;
    qc->default_blocking    = 1;
    qc->last_error          = SSL_ERROR_NONE;

    if (!create_channel(qc))
        goto err;

    if ((qc->default_xso = (QUIC_XSO *)ossl_quic_conn_stream_new(&qc->ssl, 0)) == NULL)
        goto err;

    return ssl_base;

err:
    if (qc != NULL) {
        ossl_quic_channel_free(qc->ch);
        SSL_free(qc->tls);
    }
    OPENSSL_free(qc);
    return NULL;
}

/* SSL_free */
QUIC_TAKES_LOCK
void ossl_quic_free(SSL *s)
{
    QCTX ctx;

    /* We should never be called on anything but a QSO. */
    if (!expect_quic(s, &ctx))
        return;

    if (ctx.is_stream) {
        /*
         * When a QSSO is freed, the XSO is freed immediately, because the XSO
         * itself only contains API personality layer data. However the
         * underlying QUIC_STREAM is not freed immediately but is instead marked
         * as deleted for later collection.
         */

        quic_lock(ctx.qc);

        assert(ctx.qc->num_xso > 0);
        --ctx.qc->num_xso;

        ctx.xso->stream->deleted = 1;

        /* Auto-conclude stream. */
        /* TODO(QUIC): Do RESET_STREAM here instead of auto-conclude */
        if (ctx.xso->stream->sstream != NULL)
            ossl_quic_sstream_fin(ctx.xso->stream->sstream);

        /* Update stream state. */
        ossl_quic_stream_map_update_state(ossl_quic_channel_get_qsm(ctx.xso->conn->ch),
                                          ctx.xso->stream);

        quic_unlock(ctx.qc);

        /* Note: SSL_free calls OPENSSL_free(xso) for us */
        return;
    }

    quic_lock(ctx.qc);

    /*
     * Free the default XSO, if any. The QUIC_STREAM is not deleted at this
     * stage, but is freed during the channel free when the whole QSM is freed.
     */
    if (ctx.qc->default_xso != NULL)
        SSL_free(&ctx.qc->default_xso->ssl);

    /* Ensure we have no remaining XSOs. */
    assert(ctx.qc->num_xso == 0);

    if (ctx.qc->is_thread_assisted && ctx.qc->started) {
        ossl_quic_thread_assist_wait_stopped(&ctx.qc->thread_assist);
        ossl_quic_thread_assist_cleanup(&ctx.qc->thread_assist);
    }

    ossl_quic_channel_free(ctx.qc->ch);

    BIO_free(ctx.qc->net_rbio);
    BIO_free(ctx.qc->net_wbio);

    /* Note: SSL_free calls OPENSSL_free(qc) for us */

    SSL_free(ctx.qc->tls);
    ossl_crypto_mutex_free(&ctx.qc->mutex); /* freed while still locked */
}

/* SSL method init */
int ossl_quic_init(SSL *s)
{
    /* Same op as SSL_clear, forward the call. */
    return ossl_quic_clear(s);
}

/* SSL method deinit */
void ossl_quic_deinit(SSL *s)
{
    /* No-op. */
}

/* SSL_reset */
int ossl_quic_reset(SSL *s)
{
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return 0;

    /* TODO(QUIC); Currently a no-op. */
    return 1;
}

/* SSL_clear */
int ossl_quic_clear(SSL *s)
{
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return 0;

    /* TODO(QUIC): Currently a no-op. */
    return 1;
}

void ossl_quic_conn_set_override_now_cb(SSL *s,
                                        OSSL_TIME (*now_cb)(void *arg),
                                        void *now_cb_arg)
{
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return;

    ctx.qc->override_now_cb     = now_cb;
    ctx.qc->override_now_cb_arg = now_cb_arg;
}

void ossl_quic_conn_force_assist_thread_wake(SSL *s)
{
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return;

    if (ctx.qc->is_thread_assisted && ctx.qc->started)
        ossl_quic_thread_assist_notify_deadline_changed(&ctx.qc->thread_assist);
}

/*
 * QUIC Front-End I/O API: Network BIO Configuration
 * =================================================
 *
 * Handling the different BIOs is difficult:
 *
 *   - It is more or less a requirement that we use non-blocking network I/O;
 *     we need to be able to have timeouts on recv() calls, and make best effort
 *     (non blocking) send() and recv() calls.
 *
 *     The only sensible way to do this is to configure the socket into
 *     non-blocking mode. We could try to do select() before calling send() or
 *     recv() to get a guarantee that the call will not block, but this will
 *     probably run into issues with buggy OSes which generate spurious socket
 *     readiness events. In any case, relying on this to work reliably does not
 *     seem sane.
 *
 *     Timeouts could be handled via setsockopt() socket timeout options, but
 *     this depends on OS support and adds another syscall to every network I/O
 *     operation. It also has obvious thread safety concerns if we want to move
 *     to concurrent use of a single socket at some later date.
 *
 *     Some OSes support a MSG_DONTWAIT flag which allows a single I/O option to
 *     be made non-blocking. However some OSes (e.g. Windows) do not support
 *     this, so we cannot rely on this.
 *
 *     As such, we need to configure any FD in non-blocking mode. This may
 *     confound users who pass a blocking socket to libssl. However, in practice
 *     it would be extremely strange for a user of QUIC to pass an FD to us,
 *     then also try and send receive traffic on the same socket(!). Thus the
 *     impact of this should be limited, and can be documented.
 *
 *   - We support both blocking and non-blocking operation in terms of the API
 *     presented to the user. One prospect is to set the blocking mode based on
 *     whether the socket passed to us was already in blocking mode. However,
 *     Windows has no API for determining if a socket is in blocking mode (!),
 *     therefore this cannot be done portably. Currently therefore we expose an
 *     explicit API call to set this, and default to blocking mode.
 *
 *   - We need to determine our initial destination UDP address. The "natural"
 *     way for a user to do this is to set the peer variable on a BIO_dgram.
 *     However, this has problems because BIO_dgram's peer variable is used for
 *     both transmission and reception. This means it can be constantly being
 *     changed to a malicious value (e.g. if some random unrelated entity on the
 *     network starts sending traffic to us) on every read call. This is not a
 *     direct issue because we use the 'stateless' BIO_sendmmsg and BIO_recvmmsg
 *     calls only, which do not use this variable. However, we do need to let
 *     the user specify the peer in a 'normal' manner. The compromise here is
 *     that we grab the current peer value set at the time the write BIO is set
 *     and do not read the value again.
 *
 *   - We also need to support memory BIOs (e.g. BIO_dgram_pair) or custom BIOs.
 *     Currently we do this by only supporting non-blocking mode.
 *
 */

/*
 * Determines what initial destination UDP address we should use, if possible.
 * If this fails the client must set the destination address manually, or use a
 * BIO which does not need a destination address.
 */
static int csm_analyse_init_peer_addr(BIO *net_wbio, BIO_ADDR *peer)
{
    if (BIO_dgram_get_peer(net_wbio, peer) <= 0)
        return 0;

    return 1;
}

void ossl_quic_conn_set0_net_rbio(SSL *s, BIO *net_rbio)
{
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return;

    if (ctx.qc->net_rbio == net_rbio)
        return;

    if (!ossl_quic_channel_set_net_rbio(ctx.qc->ch, net_rbio))
        return;

    BIO_free(ctx.qc->net_rbio);
    ctx.qc->net_rbio = net_rbio;

    /*
     * If what we have is not pollable (e.g. a BIO_dgram_pair) disable blocking
     * mode as we do not support it for non-pollable BIOs.
     */
    if (net_rbio != NULL) {
        BIO_POLL_DESCRIPTOR d = {0};

        if (!BIO_get_rpoll_descriptor(net_rbio, &d)
            || d.type != BIO_POLL_DESCRIPTOR_TYPE_SOCK_FD) {
            ctx.qc->blocking          = 0;
            ctx.qc->default_blocking  = 0;
            ctx.qc->can_poll_net_rbio = 0;
        } else {
            ctx.qc->can_poll_net_rbio = 1;
        }
    }
}

void ossl_quic_conn_set0_net_wbio(SSL *s, BIO *net_wbio)
{
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return;

    if (ctx.qc->net_wbio == net_wbio)
        return;

    if (!ossl_quic_channel_set_net_wbio(ctx.qc->ch, net_wbio))
        return;

    BIO_free(ctx.qc->net_wbio);
    ctx.qc->net_wbio = net_wbio;

    if (net_wbio != NULL) {
        BIO_POLL_DESCRIPTOR d = {0};

        if (!BIO_get_wpoll_descriptor(net_wbio, &d)
            || d.type != BIO_POLL_DESCRIPTOR_TYPE_SOCK_FD) {
            ctx.qc->blocking          = 0;
            ctx.qc->default_blocking  = 0;
            ctx.qc->can_poll_net_wbio = 0;
        } else {
            ctx.qc->can_poll_net_wbio = 1;
        }

        /*
         * If we do not have a peer address yet, and we have not started trying
         * to connect yet, try to autodetect one.
         */
        if (BIO_ADDR_family(&ctx.qc->init_peer_addr) == AF_UNSPEC
            && !ctx.qc->started) {
            if (!csm_analyse_init_peer_addr(net_wbio, &ctx.qc->init_peer_addr))
                /* best effort */
                BIO_ADDR_clear(&ctx.qc->init_peer_addr);

            ossl_quic_channel_set_peer_addr(ctx.qc->ch,
                                            &ctx.qc->init_peer_addr);
        }
    }
}

BIO *ossl_quic_conn_get_net_rbio(const SSL *s)
{
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return NULL;

    return ctx.qc->net_rbio;
}

BIO *ossl_quic_conn_get_net_wbio(const SSL *s)
{
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return NULL;

    return ctx.qc->net_wbio;
}

int ossl_quic_conn_get_blocking_mode(const SSL *s)
{
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return 0;

    if (ctx.is_stream)
        return ctx.xso->blocking;

    return ctx.qc->blocking;
}

int ossl_quic_conn_set_blocking_mode(SSL *s, int blocking)
{
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return 0;

    /* Cannot enable blocking mode if we do not have pollable FDs. */
    if (blocking != 0 &&
        (!ctx.qc->can_poll_net_rbio || !ctx.qc->can_poll_net_wbio))
        return QUIC_RAISE_NON_NORMAL_ERROR(ctx.qc, ERR_R_UNSUPPORTED, NULL);

    if (!ctx.is_stream) {
        /*
         * If called on a QCSO, update default and connection-level blocking
         * modes.
         */
        ctx.qc->blocking         = (blocking != 0);
        ctx.qc->default_blocking = ctx.qc->blocking;
    }

    if (ctx.xso != NULL)
        /*
         * If called on  a QSSO or QCSO with a default XSO, update blocking
         * mode.
         */
        ctx.xso->blocking = (blocking != 0);

    return 1;
}

int ossl_quic_conn_set_initial_peer_addr(SSL *s,
                                         const BIO_ADDR *peer_addr)
{
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return 0;

    if (ctx.qc->started)
        return QUIC_RAISE_NON_NORMAL_ERROR(ctx.qc, ERR_R_SHOULD_NOT_HAVE_BEEN_CALLED,
                                           NULL);

    if (peer_addr == NULL) {
        BIO_ADDR_clear(&ctx.qc->init_peer_addr);
        return 1;
    }

    ctx.qc->init_peer_addr = *peer_addr;
    return 1;
}

/*
 * QUIC Front-End I/O API: Asynchronous I/O Management
 * ===================================================
 *
 *   (BIO/)SSL_tick                 => ossl_quic_tick
 *   (BIO/)SSL_get_tick_timeout     => ossl_quic_get_tick_timeout
 *   (BIO/)SSL_get_poll_fd          => ossl_quic_get_poll_fd
 *
 */

/* Returns 1 if the connection is being used in blocking mode. */
static int qc_blocking_mode(const QUIC_CONNECTION *qc)
{
    return qc->blocking;
}

static int xso_blocking_mode(const QUIC_XSO *xso)
{
    return xso->blocking
        && xso->conn->can_poll_net_rbio
        && xso->conn->can_poll_net_wbio;
}

/* SSL_tick; ticks the reactor. */
QUIC_TAKES_LOCK
int ossl_quic_tick(SSL *s)
{
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return 0;

    quic_lock(ctx.qc);
    ossl_quic_reactor_tick(ossl_quic_channel_get_reactor(ctx.qc->ch), 0);
    quic_unlock(ctx.qc);
    return 1;
}

/*
 * SSL_get_tick_timeout. Get the time in milliseconds until the SSL object
 * should be ticked by the application by calling SSL_tick(). tv is set to 0 if
 * the object should be ticked immediately and tv->tv_sec is set to -1 if no
 * timeout is currently active.
 */
QUIC_TAKES_LOCK
int ossl_quic_get_tick_timeout(SSL *s, struct timeval *tv)
{
    QCTX ctx;
    OSSL_TIME deadline = ossl_time_infinite();

    if (!expect_quic(s, &ctx))
        return 0;

    quic_lock(ctx.qc);

    deadline
        = ossl_quic_reactor_get_tick_deadline(ossl_quic_channel_get_reactor(ctx.qc->ch));

    if (ossl_time_is_infinite(deadline)) {
        tv->tv_sec  = -1;
        tv->tv_usec = 0;
        quic_unlock(ctx.qc);
        return 1;
    }

    *tv = ossl_time_to_timeval(ossl_time_subtract(deadline, ossl_time_now()));
    quic_unlock(ctx.qc);
    return 1;
}

/* SSL_get_rpoll_descriptor */
int ossl_quic_get_rpoll_descriptor(SSL *s, BIO_POLL_DESCRIPTOR *desc)
{
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return 0;

    if (desc == NULL || ctx.qc->net_rbio == NULL)
        return 0;

    return BIO_get_rpoll_descriptor(ctx.qc->net_rbio, desc);
}

/* SSL_get_wpoll_descriptor */
int ossl_quic_get_wpoll_descriptor(SSL *s, BIO_POLL_DESCRIPTOR *desc)
{
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return 0;

    if (desc == NULL || ctx.qc->net_wbio == NULL)
        return 0;

    return BIO_get_wpoll_descriptor(ctx.qc->net_wbio, desc);
}

/* SSL_net_read_desired */
QUIC_TAKES_LOCK
int ossl_quic_get_net_read_desired(SSL *s)
{
    QCTX ctx;
    int ret;

    if (!expect_quic(s, &ctx))
        return 0;

    quic_lock(ctx.qc);
    ret = ossl_quic_reactor_net_read_desired(ossl_quic_channel_get_reactor(ctx.qc->ch));
    quic_unlock(ctx.qc);
    return ret;
}

/* SSL_net_write_desired */
QUIC_TAKES_LOCK
int ossl_quic_get_net_write_desired(SSL *s)
{
    int ret;
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return 0;

    quic_lock(ctx.qc);
    ret = ossl_quic_reactor_net_write_desired(ossl_quic_channel_get_reactor(ctx.qc->ch));
    quic_unlock(ctx.qc);
    return ret;
}

/*
 * QUIC Front-End I/O API: Connection Lifecycle Operations
 * =======================================================
 *
 *         SSL_do_handshake         => ossl_quic_do_handshake
 *         SSL_set_connect_state    => ossl_quic_set_connect_state
 *         SSL_set_accept_state     => ossl_quic_set_accept_state
 *         SSL_shutdown             => ossl_quic_shutdown
 *         SSL_ctrl                 => ossl_quic_ctrl
 *   (BIO/)SSL_connect              => ossl_quic_connect
 *   (BIO/)SSL_accept               => ossl_quic_accept
 *
 */

/* SSL_shutdown */
static int quic_shutdown_wait(void *arg)
{
    QUIC_CONNECTION *qc = arg;

    return ossl_quic_channel_is_terminated(qc->ch);
}

QUIC_TAKES_LOCK
int ossl_quic_conn_shutdown(SSL *s, uint64_t flags,
                            const SSL_SHUTDOWN_EX_ARGS *args,
                            size_t args_len)
{
    int ret;
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return 0;

    if (ctx.is_stream)
        /* TODO(QUIC): Semantics currently undefined for QSSOs */
        return -1;

    quic_lock(ctx.qc);

    ossl_quic_channel_local_close(ctx.qc->ch,
                                  args != NULL ? args->quic_error_code : 0);

    /* TODO(QUIC): !SSL_SHUTDOWN_FLAG_NO_STREAM_FLUSH */

    if (ossl_quic_channel_is_terminated(ctx.qc->ch)) {
        quic_unlock(ctx.qc);
        return 1;
    }

    if (qc_blocking_mode(ctx.qc) && (flags & SSL_SHUTDOWN_FLAG_RAPID) == 0)
        block_until_pred(ctx.qc, quic_shutdown_wait, ctx.qc, 0);
    else
        ossl_quic_reactor_tick(ossl_quic_channel_get_reactor(ctx.qc->ch), 0);

    ret = ossl_quic_channel_is_terminated(ctx.qc->ch);
    quic_unlock(ctx.qc);
    return ret;
}

/* SSL_ctrl */
long ossl_quic_ctrl(SSL *s, int cmd, long larg, void *parg)
{
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return 0;

    switch (cmd) {
    case SSL_CTRL_MODE:
        /* If called on a QCSO, update the default mode. */
        if (!ctx.is_stream)
            ctx.qc->default_ssl_mode |= (uint32_t)larg;

        /*
         * If we were called on a QSSO or have a default stream, we also update
         * that.
         */
        if (ctx.xso != NULL) {
            /* Cannot enable EPW while AON write in progress. */
            if (ctx.xso->aon_write_in_progress)
                larg &= ~SSL_MODE_ENABLE_PARTIAL_WRITE;

            ctx.xso->ssl_mode |= (uint32_t)larg;
            return ctx.xso->ssl_mode;
        }

        return ctx.qc->default_ssl_mode;
    case SSL_CTRL_CLEAR_MODE:
        if (!ctx.is_stream)
            ctx.qc->default_ssl_mode &= ~(uint32_t)larg;

        if (ctx.xso != NULL) {
            ctx.xso->ssl_mode &= ~(uint32_t)larg;
            return ctx.xso->ssl_mode;
        }

        return ctx.qc->default_ssl_mode;
    default:
        /* Probably a TLS related ctrl. Defer to our internal SSL object */
        return SSL_ctrl(ctx.qc->tls, cmd, larg, parg);
    }
}

/* SSL_set_connect_state */
void ossl_quic_set_connect_state(SSL *s)
{
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return;

    /* Cannot be changed after handshake started */
    if (ctx.qc->started || ctx.is_stream)
        return;

    ctx.qc->as_server_state = 0;
}

/* SSL_set_accept_state */
void ossl_quic_set_accept_state(SSL *s)
{
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return;

    /* Cannot be changed after handshake started */
    if (ctx.qc->started || ctx.is_stream)
        return;

    ctx.qc->as_server_state = 1;
}

/* SSL_do_handshake */
struct quic_handshake_wait_args {
    QUIC_CONNECTION     *qc;
};

static int quic_handshake_wait(void *arg)
{
    struct quic_handshake_wait_args *args = arg;

    if (!ossl_quic_channel_is_active(args->qc->ch))
        return -1;

    if (ossl_quic_channel_is_handshake_complete(args->qc->ch))
        return 1;

    return 0;
}

static int configure_channel(QUIC_CONNECTION *qc)
{
    assert(qc->ch != NULL);

    if (!ossl_quic_channel_set_net_rbio(qc->ch, qc->net_rbio)
        || !ossl_quic_channel_set_net_wbio(qc->ch, qc->net_wbio)
        || !ossl_quic_channel_set_peer_addr(qc->ch, &qc->init_peer_addr))
        return 0;

    return 1;
}

QUIC_NEEDS_LOCK
static int create_channel(QUIC_CONNECTION *qc)
{
    QUIC_CHANNEL_ARGS args = {0};

    args.libctx     = qc->ssl.ctx->libctx;
    args.propq      = qc->ssl.ctx->propq;
    args.is_server  = qc->as_server;
    args.tls        = qc->tls;
    args.mutex      = qc->mutex;
    args.now_cb     = qc->override_now_cb;
    args.now_cb_arg = qc->override_now_cb_arg;

    qc->ch = ossl_quic_channel_new(&args);
    if (qc->ch == NULL)
        return 0;

    return 1;
}

/*
 * Creates a channel and configures it with the information we have accumulated
 * via calls made to us from the application prior to starting a handshake
 * attempt.
 */
QUIC_NEEDS_LOCK
static int ensure_channel_started(QUIC_CONNECTION *qc)
{
    if (!qc->started) {
        if (!configure_channel(qc)
            || !ossl_quic_channel_start(qc->ch))
            goto err;

        if (qc->is_thread_assisted)
            if (!ossl_quic_thread_assist_init_start(&qc->thread_assist, qc->ch))
                goto err;
    }

    qc->started = 1;
    return 1;

err:
    ossl_quic_channel_free(qc->ch);
    qc->ch = NULL;
    return 0;
}

QUIC_NEEDS_LOCK
static int quic_do_handshake(QUIC_CONNECTION *qc)
{
    int ret;

    if (ossl_quic_channel_is_handshake_complete(qc->ch))
        /* Handshake already completed. */
        return 1;

    if (ossl_quic_channel_is_term_any(qc->ch))
        return QUIC_RAISE_NON_NORMAL_ERROR(qc, SSL_R_PROTOCOL_IS_SHUTDOWN, NULL);

    if (BIO_ADDR_family(&qc->init_peer_addr) == AF_UNSPEC) {
        /* Peer address must have been set. */
        QUIC_RAISE_NON_NORMAL_ERROR(qc, SSL_R_REMOTE_PEER_ADDRESS_NOT_SET, NULL);
        return -1; /* Non-protocol error */
    }

    if (qc->as_server != qc->as_server_state) {
        /* TODO(QUIC): Must match the method used to create the QCSO */
        QUIC_RAISE_NON_NORMAL_ERROR(qc, ERR_R_PASSED_INVALID_ARGUMENT, NULL);
        return -1; /* Non-protocol error */
    }

    if (qc->net_rbio == NULL || qc->net_wbio == NULL) {
        /* Need read and write BIOs. */
        QUIC_RAISE_NON_NORMAL_ERROR(qc, SSL_R_BIO_NOT_SET, NULL);
        return -1; /* Non-protocol error */
    }

    /*
     * Start connection process. Note we may come here multiple times in
     * non-blocking mode, which is fine.
     */
    if (!ensure_channel_started(qc)) {
        QUIC_RAISE_NON_NORMAL_ERROR(qc, ERR_R_INTERNAL_ERROR, NULL);
        return -1; /* Non-protocol error */
    }

    if (ossl_quic_channel_is_handshake_complete(qc->ch))
        /* The handshake is now done. */
        return 1;

    if (qc_blocking_mode(qc)) {
        /* In blocking mode, wait for the handshake to complete. */
        struct quic_handshake_wait_args args;

        args.qc     = qc;

        ret = block_until_pred(qc, quic_handshake_wait, &args, 0);
        if (!ossl_quic_channel_is_active(qc->ch)) {
            QUIC_RAISE_NON_NORMAL_ERROR(qc, SSL_R_PROTOCOL_IS_SHUTDOWN, NULL);
            return 0; /* Shutdown before completion */
        } else if (ret <= 0) {
            QUIC_RAISE_NON_NORMAL_ERROR(qc, ERR_R_INTERNAL_ERROR, NULL);
            return -1; /* Non-protocol error */
        }

        assert(ossl_quic_channel_is_handshake_complete(qc->ch));
        return 1;
    } else {
        /* Try to advance the reactor. */
        ossl_quic_reactor_tick(ossl_quic_channel_get_reactor(qc->ch), 0);

        if (ossl_quic_channel_is_handshake_complete(qc->ch))
            /* The handshake is now done. */
            return 1;

        /* Otherwise, indicate that the handshake isn't done yet. */
        QUIC_RAISE_NORMAL_ERROR(qc, SSL_ERROR_WANT_READ);
        return -1; /* Non-protocol error */
    }
}

QUIC_TAKES_LOCK
int ossl_quic_do_handshake(SSL *s)
{
    int ret;
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return 0;

    quic_lock(ctx.qc);

    ret = quic_do_handshake(ctx.qc);
    quic_unlock(ctx.qc);
    return ret;
}

/* SSL_connect */
int ossl_quic_connect(SSL *s)
{
    /* Ensure we are in connect state (no-op if non-idle). */
    ossl_quic_set_connect_state(s);

    /* Begin or continue the handshake */
    return ossl_quic_do_handshake(s);
}

/* SSL_accept */
int ossl_quic_accept(SSL *s)
{
    /* Ensure we are in accept state (no-op if non-idle). */
    ossl_quic_set_accept_state(s);

    /* Begin or continue the handshake */
    return ossl_quic_do_handshake(s);
}

/*
 * QUIC Front-End I/O API: Stream Lifecycle Operations
 * ===================================================
 *
 *         SSL_stream_new       => ossl_quic_conn_stream_new
 *
 */
SSL *ossl_quic_conn_stream_new(SSL *s, uint64_t flags)
{
    QCTX ctx;
    QUIC_XSO *xso = NULL;
    int is_uni = ((flags & SSL_STREAM_FLAG_UNI) != 0);

    if (!expect_quic_conn_only(s, &ctx))
        return NULL;

    quic_lock(ctx.qc);

    if (ossl_quic_channel_is_term_any(ctx.qc->ch)) {
        QUIC_RAISE_NON_NORMAL_ERROR(ctx.qc, SSL_R_PROTOCOL_IS_SHUTDOWN, NULL);
        goto err;
    }

    if ((xso = OPENSSL_zalloc(sizeof(*xso))) == NULL)
        goto err;

    if (!ossl_ssl_init(&xso->ssl, s->ctx, s->method, SSL_TYPE_QUIC_XSO))
        goto err;

    xso->conn       = ctx.qc;
    xso->blocking   = ctx.qc->default_blocking;
    xso->ssl_mode   = ctx.qc->default_ssl_mode;

    xso->stream     = ossl_quic_channel_new_stream_local(ctx.qc->ch, is_uni);
    if (xso->stream == NULL)
        goto err;

    ++ctx.qc->num_xso;

    quic_unlock(ctx.qc);
    return &xso->ssl;

err:
    OPENSSL_free(xso);
    quic_unlock(ctx.qc);
    return NULL;
}

/*
 * QUIC Front-End I/O API: Steady-State Operations
 * ===============================================
 *
 * Here we dispatch calls to the steady-state front-end I/O API functions; that
 * is, the functions used during the established phase of a QUIC connection
 * (e.g. SSL_read, SSL_write).
 *
 * Each function must handle both blocking and non-blocking modes. As discussed
 * above, all QUIC I/O is implemented using non-blocking mode internally.
 *
 *         SSL_get_error        => partially implemented by ossl_quic_get_error
 *   (BIO/)SSL_read             => ossl_quic_read
 *   (BIO/)SSL_write            => ossl_quic_write
 *         SSL_pending          => ossl_quic_pending
 *         SSL_stream_conclude  => ossl_quic_conn_stream_conclude
 */

/* SSL_get_error */
int ossl_quic_get_error(const SSL *s, int i)
{
    QCTX ctx;

    if (!expect_quic(s, &ctx))
        return 0;

    return ctx.qc->last_error;
}

/*
 * SSL_write
 * ---------
 *
 * The set of functions below provide the implementation of the public SSL_write
 * function. We must handle:
 *
 *   - both blocking and non-blocking operation at the application level,
 *     depending on how we are configured;
 *
 *   - SSL_MODE_ENABLE_PARTIAL_WRITE being on or off;
 *
 *   - SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER.
 *
 */
QUIC_NEEDS_LOCK
static void quic_post_write(QUIC_XSO *xso, int did_append, int do_tick)
{
    /*
     * We have appended at least one byte to the stream.
     * Potentially mark stream as active, depending on FC.
     */
    if (did_append)
        ossl_quic_stream_map_update_state(ossl_quic_channel_get_qsm(xso->conn->ch),
                                          xso->stream);

    /*
     * Try and send.
     *
     * TODO(QUIC): It is probably inefficient to try and do this immediately,
     * plus we should eventually consider Nagle's algorithm.
     */
    if (do_tick)
        ossl_quic_reactor_tick(ossl_quic_channel_get_reactor(xso->conn->ch), 0);
}

struct quic_write_again_args {
    QUIC_XSO            *xso;
    const unsigned char *buf;
    size_t              len;
    size_t              total_written;
};

QUIC_NEEDS_LOCK
static int quic_write_again(void *arg)
{
    struct quic_write_again_args *args = arg;
    size_t actual_written = 0;

    if (!ossl_quic_channel_is_active(args->xso->conn->ch))
        /* If connection is torn down due to an error while blocking, stop. */
        return -2;

    if (!ossl_quic_sstream_append(args->xso->stream->sstream,
                                  args->buf, args->len, &actual_written))
        return -2;

    quic_post_write(args->xso, actual_written > 0, 0);

    args->buf           += actual_written;
    args->len           -= actual_written;
    args->total_written += actual_written;

    if (args->len == 0)
        /* Written everything, done. */
        return 1;

    /* Not written everything yet, keep trying. */
    return 0;
}

QUIC_NEEDS_LOCK
static int quic_write_blocking(QUIC_XSO *xso, const void *buf, size_t len,
                               size_t *written)
{
    int res;
    struct quic_write_again_args args;
    size_t actual_written = 0;

    /* First make a best effort to append as much of the data as possible. */
    if (!ossl_quic_sstream_append(xso->stream->sstream, buf, len,
                                  &actual_written)) {
        /* Stream already finished or allocation error. */
        *written = 0;
        return QUIC_RAISE_NON_NORMAL_ERROR(xso->conn, ERR_R_INTERNAL_ERROR, NULL);
    }

    quic_post_write(xso, actual_written > 0, 1);

    if (actual_written == len) {
        /* Managed to append everything on the first try. */
        *written = actual_written;
        return 1;
    }

    /*
     * We did not manage to append all of the data immediately, so the stream
     * buffer has probably filled up. This means we need to block until some of
     * it is freed up.
     */
    args.xso            = xso;
    args.buf            = (const unsigned char *)buf + actual_written;
    args.len            = len - actual_written;
    args.total_written  = 0;

    res = block_until_pred(xso->conn, quic_write_again, &args, 0);
    if (res <= 0) {
        if (!ossl_quic_channel_is_active(xso->conn->ch))
            return QUIC_RAISE_NON_NORMAL_ERROR(xso->conn, SSL_R_PROTOCOL_IS_SHUTDOWN, NULL);
        else
            return QUIC_RAISE_NON_NORMAL_ERROR(xso->conn, ERR_R_INTERNAL_ERROR, NULL);
    }

    *written = args.total_written;
    return 1;
}

/*
 * Functions to manage All-or-Nothing (AON) (that is, non-ENABLE_PARTIAL_WRITE)
 * write semantics.
 */
static void aon_write_begin(QUIC_XSO *xso, const unsigned char *buf,
                            size_t buf_len, size_t already_sent)
{
    assert(!xso->aon_write_in_progress);

    xso->aon_write_in_progress = 1;
    xso->aon_buf_base          = buf;
    xso->aon_buf_pos           = already_sent;
    xso->aon_buf_len           = buf_len;
}

static void aon_write_finish(QUIC_XSO *xso)
{
    xso->aon_write_in_progress   = 0;
    xso->aon_buf_base            = NULL;
    xso->aon_buf_pos             = 0;
    xso->aon_buf_len             = 0;
}

QUIC_NEEDS_LOCK
static int quic_write_nonblocking_aon(QUIC_XSO *xso, const void *buf,
                                      size_t len, size_t *written)
{
    const void *actual_buf;
    size_t actual_len, actual_written = 0;
    int accept_moving_buffer
        = ((xso->ssl_mode & SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER) != 0);

    if (xso->aon_write_in_progress) {
        /*
         * We are in the middle of an AON write (i.e., a previous write did not
         * manage to append all data to the SSTREAM and we have Enable Partial
         * Write (EPW) mode disabled.)
         */
        if ((!accept_moving_buffer && xso->aon_buf_base != buf)
            || len != xso->aon_buf_len)
            /*
             * Pointer must not have changed if we are not in accept moving
             * buffer mode. Length must never change.
             */
            return QUIC_RAISE_NON_NORMAL_ERROR(xso->conn, SSL_R_BAD_WRITE_RETRY, NULL);

        actual_buf = (unsigned char *)buf + xso->aon_buf_pos;
        actual_len = len - xso->aon_buf_pos;
        assert(actual_len > 0);
    } else {
        actual_buf = buf;
        actual_len = len;
    }

    /* First make a best effort to append as much of the data as possible. */
    if (!ossl_quic_sstream_append(xso->stream->sstream, actual_buf, actual_len,
                                  &actual_written)) {
        /* Stream already finished or allocation error. */
        *written = 0;
        return QUIC_RAISE_NON_NORMAL_ERROR(xso->conn, ERR_R_INTERNAL_ERROR, NULL);
    }

    quic_post_write(xso, actual_written > 0, 1);

    if (actual_written == actual_len) {
        /* We have sent everything. */
        if (xso->aon_write_in_progress) {
            /*
             * We have sent everything, and we were in the middle of an AON
             * write. The output write length is the total length of the AON
             * buffer, not however many bytes we managed to write to the stream
             * in this call.
             */
            *written = xso->aon_buf_len;
            aon_write_finish(xso);
        } else {
            *written = actual_written;
        }

        return 1;
    }

    if (xso->aon_write_in_progress) {
        /*
         * AON write is in progress but we have not written everything yet. We
         * may have managed to send zero bytes, or some number of bytes less
         * than the total remaining which need to be appended during this
         * AON operation.
         */
        xso->aon_buf_pos += actual_written;
        assert(xso->aon_buf_pos < xso->aon_buf_len);
        return QUIC_RAISE_NORMAL_ERROR(xso->conn, SSL_ERROR_WANT_WRITE);
    }

    /*
     * Not in an existing AON operation but partial write is not enabled, so we
     * need to begin a new AON operation. However we needn't bother if we didn't
     * actually append anything.
     */
    if (actual_written > 0)
        aon_write_begin(xso, buf, len, actual_written);

    /*
     * AON - We do not publicly admit to having appended anything until AON
     * completes.
     */
    *written = 0;
    return QUIC_RAISE_NORMAL_ERROR(xso->conn, SSL_ERROR_WANT_WRITE);
}

QUIC_NEEDS_LOCK
static int quic_write_nonblocking_epw(QUIC_XSO *xso, const void *buf, size_t len,
                                      size_t *written)
{
    /* Simple best effort operation. */
    if (!ossl_quic_sstream_append(xso->stream->sstream, buf, len, written)) {
        /* Stream already finished or allocation error. */
        *written = 0;
        return QUIC_RAISE_NON_NORMAL_ERROR(xso->conn, ERR_R_INTERNAL_ERROR, NULL);
    }

    quic_post_write(xso, *written > 0, 1);
    return 1;
}

QUIC_TAKES_LOCK
int ossl_quic_write(SSL *s, const void *buf, size_t len, size_t *written)
{
    int ret;
    QCTX ctx;
    int partial_write;

    *written = 0;

    if (!expect_quic_with_stream(s, &ctx))
        return 0;

    quic_lock(ctx.qc);

    partial_write = ((ctx.xso->ssl_mode & SSL_MODE_ENABLE_PARTIAL_WRITE) != 0);

    if (ossl_quic_channel_is_term_any(ctx.qc->ch)) {
        ret = QUIC_RAISE_NON_NORMAL_ERROR(ctx.qc, SSL_R_PROTOCOL_IS_SHUTDOWN, NULL);
        goto out;
    }

    /*
     * If we haven't finished the handshake, try to advance it.
     * We don't accept writes until the handshake is completed.
     */
    if (quic_do_handshake(ctx.qc) < 1) {
        ret = 0;
        goto out;
    }

    if (ctx.xso->stream == NULL || ctx.xso->stream->sstream == NULL) {
        ret = QUIC_RAISE_NON_NORMAL_ERROR(ctx.qc, ERR_R_INTERNAL_ERROR, NULL);
        goto out;
    }

    if (xso_blocking_mode(ctx.xso))
        ret = quic_write_blocking(ctx.xso, buf, len, written);
    else if (partial_write)
        ret = quic_write_nonblocking_epw(ctx.xso, buf, len, written);
    else
        ret = quic_write_nonblocking_aon(ctx.xso, buf, len, written);

out:
    quic_unlock(ctx.qc);
    return ret;
}

/*
 * SSL_read
 * --------
 */
struct quic_read_again_args {
    QUIC_CONNECTION *qc;
    QUIC_STREAM     *stream;
    void            *buf;
    size_t          len;
    size_t          *bytes_read;
    int             peek;
};

QUIC_NEEDS_LOCK
static int quic_read_actual(QUIC_CONNECTION *qc,
                            QUIC_STREAM *stream,
                            void *buf, size_t buf_len,
                            size_t *bytes_read,
                            int peek)
{
    int is_fin = 0;

    /* If the receive part of the stream is over, issue EOF. */
    if (stream->recv_fin_retired)
        return QUIC_RAISE_NORMAL_ERROR(qc, SSL_ERROR_ZERO_RETURN);

    if (stream->rstream == NULL)
        return QUIC_RAISE_NON_NORMAL_ERROR(qc, ERR_R_INTERNAL_ERROR, NULL);

    if (peek) {
        if (!ossl_quic_rstream_peek(stream->rstream, buf, buf_len,
                                    bytes_read, &is_fin))
            return QUIC_RAISE_NON_NORMAL_ERROR(qc, ERR_R_INTERNAL_ERROR, NULL);

    } else {
        if (!ossl_quic_rstream_read(stream->rstream, buf, buf_len,
                                    bytes_read, &is_fin))
            return QUIC_RAISE_NON_NORMAL_ERROR(qc, ERR_R_INTERNAL_ERROR, NULL);
    }

    if (!peek) {
        if (*bytes_read > 0) {
            /*
             * We have read at least one byte from the stream. Inform stream-level
             * RXFC of the retirement of controlled bytes. Update the active stream
             * status (the RXFC may now want to emit a frame granting more credit to
             * the peer).
             */
            OSSL_RTT_INFO rtt_info;

            ossl_statm_get_rtt_info(ossl_quic_channel_get_statm(qc->ch), &rtt_info);

            if (!ossl_quic_rxfc_on_retire(&stream->rxfc, *bytes_read,
                                          rtt_info.smoothed_rtt))
                return QUIC_RAISE_NON_NORMAL_ERROR(qc, ERR_R_INTERNAL_ERROR, NULL);
        }

        if (is_fin)
            stream->recv_fin_retired = 1;

        if (*bytes_read > 0)
            ossl_quic_stream_map_update_state(ossl_quic_channel_get_qsm(qc->ch),
                                              stream);
    }

    return 1;
}

QUIC_NEEDS_LOCK
static int quic_read_again(void *arg)
{
    struct quic_read_again_args *args = arg;

    if (!ossl_quic_channel_is_active(args->qc->ch)) {
        /* If connection is torn down due to an error while blocking, stop. */
        QUIC_RAISE_NON_NORMAL_ERROR(args->qc, SSL_R_PROTOCOL_IS_SHUTDOWN, NULL);
        return -1;
    }

    if (!quic_read_actual(args->qc, args->stream,
                          args->buf, args->len, args->bytes_read,
                          args->peek))
        return -1;

    if (*args->bytes_read > 0)
        /* got at least one byte, the SSL_read op can finish now */
        return 1;

    return 0; /* did not read anything, keep trying */
}

QUIC_TAKES_LOCK
static int quic_read(SSL *s, void *buf, size_t len, size_t *bytes_read, int peek)
{
    int ret, res;
    QCTX ctx;
    struct quic_read_again_args args;

    *bytes_read = 0;

    if (!expect_quic_with_stream(s, &ctx))
        return 0;

    quic_lock(ctx.qc);

    if (ossl_quic_channel_is_term_any(ctx.qc->ch)) {
        ret = QUIC_RAISE_NON_NORMAL_ERROR(ctx.qc, SSL_R_PROTOCOL_IS_SHUTDOWN, NULL);
        goto out;
    }

    /* If we haven't finished the handshake, try to advance it. */
    if (quic_do_handshake(ctx.qc) < 1) {
        ret = 0; /* ossl_quic_do_handshake raised error here */
        goto out;
    }

    if (ctx.xso->stream == NULL) {
        ret = QUIC_RAISE_NON_NORMAL_ERROR(ctx.qc, ERR_R_INTERNAL_ERROR, NULL);
        goto out;
    }

    if (!quic_read_actual(ctx.qc, ctx.xso->stream, buf, len, bytes_read, peek)) {
        ret = 0; /* quic_read_actual raised error here */
        goto out;
    }

    if (*bytes_read > 0) {
        /*
         * Even though we succeeded, tick the reactor here to ensure we are
         * handling other aspects of the QUIC connection.
         */
        ossl_quic_reactor_tick(ossl_quic_channel_get_reactor(ctx.qc->ch), 0);
        ret = 1;
    } else if (xso_blocking_mode(ctx.xso)) {
        /*
         * We were not able to read anything immediately, so our stream
         * buffer is empty. This means we need to block until we get
         * at least one byte.
         */
        args.qc         = ctx.qc;
        args.stream     = ctx.xso->stream;
        args.buf        = buf;
        args.len        = len;
        args.bytes_read = bytes_read;
        args.peek       = peek;

        res = block_until_pred(ctx.qc, quic_read_again, &args, 0);
        if (res == 0) {
            ret = QUIC_RAISE_NON_NORMAL_ERROR(ctx.qc, ERR_R_INTERNAL_ERROR, NULL);
            goto out;
        } else if (res < 0) {
            ret = 0; /* quic_read_again raised error here */
            goto out;
        }

        ret = 1;
    } else {
        /* We did not get any bytes and are not in blocking mode. */
        ret = QUIC_RAISE_NORMAL_ERROR(ctx.qc, SSL_ERROR_WANT_READ);
    }

out:
    quic_unlock(ctx.qc);
    return ret;
}

int ossl_quic_read(SSL *s, void *buf, size_t len, size_t *bytes_read)
{
    return quic_read(s, buf, len, bytes_read, 0);
}

int ossl_quic_peek(SSL *s, void *buf, size_t len, size_t *bytes_read)
{
    return quic_read(s, buf, len, bytes_read, 1);
}

/*
 * SSL_pending
 * -----------
 */
QUIC_TAKES_LOCK
static size_t ossl_quic_pending_int(const SSL *s)
{
    QCTX ctx;
    size_t avail = 0;
    int fin = 0;

    if (!expect_quic_with_stream(s, &ctx))
        return 0;

    quic_lock(ctx.qc);

    if (ctx.xso->stream == NULL || ctx.xso->stream->rstream == NULL)
        /* Cannot raise errors here because we are const, just fail. */
        goto out;

    if (!ossl_quic_rstream_available(ctx.xso->stream->rstream, &avail, &fin))
        avail = 0;

out:
    quic_unlock(ctx.qc);
    return avail;
}

size_t ossl_quic_pending(const SSL *s)
{
    return ossl_quic_pending_int(s);
}

int ossl_quic_has_pending(const SSL *s)
{
    return ossl_quic_pending_int(s) > 0;
}

/*
 * SSL_stream_conclude
 * -------------------
 */
QUIC_TAKES_LOCK
int ossl_quic_conn_stream_conclude(SSL *s)
{
    QCTX ctx;
    QUIC_STREAM *qs;

    if (!expect_quic_with_stream(s, &ctx))
        return 0;

    quic_lock(ctx.qc);

    qs = ctx.xso->stream;

    if (qs == NULL || qs->sstream == NULL) {
        quic_unlock(ctx.qc);
        return 0;
    }

    if (!ossl_quic_channel_is_active(ctx.qc->ch)
        || ossl_quic_sstream_get_final_size(qs->sstream, NULL)) {
        quic_unlock(ctx.qc);
        return 1;
    }

    ossl_quic_sstream_fin(qs->sstream);
    quic_post_write(ctx.xso, 1, 1);
    quic_unlock(ctx.qc);
    return 1;
}

/*
 * SSL_inject_net_dgram
 * --------------------
 */
QUIC_TAKES_LOCK
int SSL_inject_net_dgram(SSL *s, const unsigned char *buf,
                         size_t buf_len,
                         const BIO_ADDR *peer,
                         const BIO_ADDR *local)
{
    int ret;
    QCTX ctx;
    QUIC_DEMUX *demux;

    if (!expect_quic(s, &ctx))
        return 0;

    quic_lock(ctx.qc);

    demux = ossl_quic_channel_get0_demux(ctx.qc->ch);
    ret = ossl_quic_demux_inject(demux, buf, buf_len, peer, local);

    quic_unlock(ctx.qc);
    return ret;
}

/*
 * QUIC Front-End I/O API: SSL_CTX Management
 * ==========================================
 */

long ossl_quic_ctx_ctrl(SSL_CTX *ctx, int cmd, long larg, void *parg)
{
    switch (cmd) {
    default:
        return ssl3_ctx_ctrl(ctx, cmd, larg, parg);
    }
}

long ossl_quic_callback_ctrl(SSL *s, int cmd, void (*fp) (void))
{
    return ssl3_callback_ctrl(s, cmd, fp);
}

long ossl_quic_ctx_callback_ctrl(SSL_CTX *ctx, int cmd, void (*fp) (void))
{
    return ssl3_ctx_callback_ctrl(ctx, cmd, fp);
}

int ossl_quic_renegotiate_check(SSL *ssl, int initok)
{
    /* We never do renegotiation. */
    return 0;
}

/*
 * These functions define the TLSv1.2 (and below) ciphers that are supported by
 * the SSL_METHOD. Since QUIC only supports TLSv1.3 we don't support any.
 */

int ossl_quic_num_ciphers(void)
{
    return 0;
}

const SSL_CIPHER *ossl_quic_get_cipher(unsigned int u)
{
    return NULL;
}