/*
 * Copyright 2024 The OpenSSL Project Authors. All Rights Reserved.
 *
 * Licensed under the Apache License 2.0 (the "License").  You may not use
 * this file except in compliance with the License.  You can obtain a copy
 * in the file LICENSE in the source distribution or at
 * https://www.openssl.org/source/license.html
 */

#include "quic_obj_local.h"
#include "quic_local.h"

static int obj_update_cache(QUIC_OBJ *obj);

int ossl_quic_obj_init(QUIC_OBJ *obj,
                       SSL_CTX *ctx,
                       int type,
                       SSL *parent_obj,
                       QUIC_ENGINE *engine,
                       QUIC_PORT *port)
{
    int is_event_leader = (engine != NULL);
    int is_port_leader  = (port != NULL);

    if (!ossl_assert(!obj->init_done && obj != NULL && SSL_TYPE_IS_QUIC(type)
                     && (parent_obj == NULL || IS_QUIC(parent_obj))))
        return 0;

    /* Event leader is always the root object. */
    if (!ossl_assert(!is_event_leader || parent_obj == NULL))
        return 0;

    if (!ossl_ssl_init(&obj->ssl, ctx, ctx->method, type))
        goto err;

    obj->parent_obj         = parent_obj;
    obj->is_event_leader    = is_event_leader;
    obj->is_port_leader     = is_port_leader;
    if (!obj_update_cache(obj))
        goto err;

    obj->engine             = engine;
    obj->port               = port;
    obj->init_done          = 1;
    return 1;

err:
    obj->is_event_leader = 0;
    obj->is_port_leader  = 0;
    return 0;
}

static ossl_inline QUIC_OBJ *
ssl_to_obj(SSL *ssl)
{
    if (ssl == NULL)
        return NULL;

    assert(IS_QUIC(ssl));
    return (QUIC_OBJ *)ssl;
}

static int obj_update_cache(QUIC_OBJ *obj)
{
    QUIC_OBJ *p;

    for (p = obj; p != NULL && !p->is_event_leader;
         p = ssl_to_obj(p->parent_obj))
        if (!ossl_assert(p == obj || p->init_done))
            return 0;

    if (!ossl_assert(p != NULL))
        return 0;

    /*
     * Offset of ->ssl is guaranteed to be 0 but the NULL check makes ubsan
     * happy.
     */
    obj->cached_event_leader    = (p != NULL) ? &p->ssl : NULL;
    obj->engine                 = p->engine;

    for (p = obj; p != NULL && !p->is_port_leader;
         p = ssl_to_obj(p->parent_obj));

    obj->cached_port_leader     = (p != NULL) ? &p->ssl : NULL;
    obj->port                   = (p != NULL) ? p->port : NULL;
    return 1;
}

SSL_CONNECTION *ossl_quic_obj_get0_handshake_layer(QUIC_OBJ *obj)
{
    assert(obj->init_done);

    if (obj == NULL || obj->ssl.type != SSL_TYPE_QUIC_CONNECTION)
        return NULL;

    return SSL_CONNECTION_FROM_SSL_ONLY(((QUIC_CONNECTION *)obj)->tls);
}