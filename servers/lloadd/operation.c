/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 1998-2020 The OpenLDAP Foundation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */

#include "portable.h"

#include "lutil.h"
#include "slap.h"

ber_tag_t
slap_req2res( ber_tag_t tag )
{
    switch ( tag ) {
        case LDAP_REQ_ADD:
        case LDAP_REQ_BIND:
        case LDAP_REQ_COMPARE:
        case LDAP_REQ_EXTENDED:
        case LDAP_REQ_MODIFY:
        case LDAP_REQ_MODRDN:
            tag++;
            break;

        case LDAP_REQ_DELETE:
            tag = LDAP_RES_DELETE;
            break;

        case LDAP_REQ_ABANDON:
        case LDAP_REQ_UNBIND:
            tag = LBER_SEQUENCE;
            break;

        case LDAP_REQ_SEARCH:
            tag = LDAP_RES_SEARCH_RESULT;
            break;

        default:
            tag = LBER_SEQUENCE;
    }

    return tag;
}

const char *
slap_msgtype2str( ber_tag_t tag )
{
    switch ( tag ) {
        case LDAP_REQ_ABANDON: return "abandon request";
        case LDAP_REQ_ADD: return "add request";
        case LDAP_REQ_BIND: return "bind request";
        case LDAP_REQ_COMPARE: return "compare request";
        case LDAP_REQ_DELETE: return "delete request";
        case LDAP_REQ_EXTENDED: return "extended request";
        case LDAP_REQ_MODIFY: return "modify request";
        case LDAP_REQ_RENAME: return "rename request";
        case LDAP_REQ_SEARCH: return "search request";
        case LDAP_REQ_UNBIND: return "unbind request";

        case LDAP_RES_ADD: return "add result";
        case LDAP_RES_BIND: return "bind result";
        case LDAP_RES_COMPARE: return "compare result";
        case LDAP_RES_DELETE: return "delete result";
        case LDAP_RES_EXTENDED: return "extended result";
        case LDAP_RES_INTERMEDIATE: return "intermediate response";
        case LDAP_RES_MODIFY: return "modify result";
        case LDAP_RES_RENAME: return "rename result";
        case LDAP_RES_SEARCH_ENTRY: return "search-entry response";
        case LDAP_RES_SEARCH_REFERENCE: return "search-reference response";
        case LDAP_RES_SEARCH_RESULT: return "search result";
    }
    return "unknown message";
}

int
operation_client_cmp( const void *left, const void *right )
{
    const Operation *l = left, *r = right;

    assert( l->o_client == r->o_client );
    return ( l->o_client_msgid < r->o_client_msgid ) ?
            -1 :
            ( l->o_client_msgid > r->o_client_msgid );
}

int
operation_upstream_cmp( const void *left, const void *right )
{
    const Operation *l = left, *r = right;

    assert( l->o_upstream == r->o_upstream );
    return ( l->o_upstream_msgid < r->o_upstream_msgid ) ?
            -1 :
            ( l->o_upstream_msgid > r->o_upstream_msgid );
}

void
operation_destroy( Operation *op )
{
    Connection *c;

    /* TODO: this is a stopgap and there are many races here, just get
     * something in to test with until we implement the freelist */
    if ( op->o_client ) {
        c = op->o_client;
        ldap_pvt_thread_mutex_lock( &c->c_mutex );
        tavl_delete( &c->c_ops, op, operation_client_cmp );
        ldap_pvt_thread_mutex_unlock( &c->c_mutex );
    }

    if ( op->o_upstream ) {
        c = op->o_upstream;
        ldap_pvt_thread_mutex_lock( &c->c_mutex );
        tavl_delete( &c->c_ops, op, operation_upstream_cmp );
        ldap_pvt_thread_mutex_unlock( &c->c_mutex );
    }

    ch_free( op );
}

Operation *
operation_init( Connection *c, BerElement *ber )
{
    Operation *op;
    ber_tag_t tag;
    ber_len_t len;
    int rc;

    op = ch_calloc( 1, sizeof(Operation) );
    op->o_client = c;

    tag = ber_get_int( ber, &op->o_client_msgid );
    if ( tag != LDAP_TAG_MSGID ) {
        goto fail;
    }

    rc = tavl_insert( &c->c_ops, op, operation_client_cmp, avl_dup_error );
    if ( rc ) {
        Debug( LDAP_DEBUG_PACKETS, "operation_init: "
                "several operations with same msgid=%d in-flight "
                "from the client\n",
                op->o_client_msgid );
        goto fail;
    }

    tag = op->o_tag = ber_skip_element( ber, &op->o_request );
    switch ( tag ) {
        case LBER_ERROR:
            rc = -1;
            break;
    }
    if ( rc ) {
        tavl_delete( &c->c_ops, op, operation_client_cmp );
        goto fail;
    }

    tag = ber_peek_tag( ber, &len );
    if ( tag == LDAP_TAG_CONTROLS ) {
        ber_skip_element( ber, &op->o_ctrls );
    }

    Debug( LDAP_DEBUG_TRACE, "operation_init: "
            "set up a new operation, %s with msgid=%d for client %lu\n",
            slap_msgtype2str( op->o_tag ), op->o_client_msgid, c->c_connid );

    return op;

fail:
    ch_free( op );
    return NULL;
}

void *
operation_process( void *ctx, void *arg )
{
    Operation *op = arg;
    BerElement *output;
    Connection *c;
    ber_int_t msgid;
    int rc;

    c = backend_select( op );
    if ( !c ) {
        Debug( LDAP_DEBUG_STATS, "operation_process: "
                "no available connection found\n" );
        goto fail;
    }
    op->o_upstream = c;

    c->c_pendingber = output = ber_alloc();
    if ( !output ) {
        goto fail;
    }

    op->o_upstream_msgid = msgid = c->c_next_msgid++;
    rc = tavl_insert( &c->c_ops, op, operation_upstream_cmp, avl_dup_error );
    assert( rc == LDAP_SUCCESS );

    ber_start_seq( output, LDAP_TAG_MESSAGE );
    ber_put_int( output, msgid, LDAP_TAG_MSGID );
    ber_put_berval( output, &op->o_request, op->o_tag );
    if ( !BER_BVISNULL( &op->o_ctrls ) ) {
        ber_put_berval( output, &op->o_ctrls, LDAP_TAG_CONTROLS );
    }
    ber_put_seq( output );

    ldap_pvt_thread_mutex_unlock( &c->c_mutex );
    upstream_write_cb( -1, 0, c );

    return NULL;
fail:
    return NULL;
}