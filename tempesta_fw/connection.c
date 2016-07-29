/**
 *		Tempesta FW
 *
 * Generic connection management.
 *
 * Copyright (C) 2014 NatSys Lab. (info@natsys-lab.com).
 * Copyright (C) 2015-2016 Tempesta Technologies, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITFWOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include "connection.h"
#include "gfsm.h"
#include "log.h"
#include "sync_socket.h"

#define TFW_CONN_MAX_PROTOS	TFW_GFSM_FSM_N

static TfwConnHooks *conn_hooks[TFW_CONN_MAX_PROTOS];

#define TFW_CONN_HOOK_CALL(conn, hook_name) \
	conn_hooks[TFW_CONN_TYPE2IDX(TFW_CONN_TYPE(conn))]->hook_name(conn)

/*
 * Initialize the connection structure.
 * It's not on any list yet, so it's safe to do so without locks.
 */
void
tfw_connection_init(TfwConnection *conn)
{
	memset(conn, 0, sizeof(*conn));

	INIT_LIST_HEAD(&conn->list);
	INIT_LIST_HEAD(&conn->msg_queue);
	spin_lock_init(&conn->msg_qlock);
}

void
tfw_connection_link_peer(TfwConnection *conn, TfwPeer *peer)
{
	BUG_ON(conn->peer || !list_empty(&conn->list));
	conn->peer = peer;
	tfw_peer_add_conn(peer, &conn->list);
}

/**
 * Publish the "connection is established" event via TfwConnHooks.
 */
int
tfw_connection_new(TfwConnection *conn)
{
	return TFW_CONN_HOOK_CALL(conn, conn_init);
}

/**
 * Publish the "connection is dropped" event via TfwConnHooks.
 */
void
tfw_connection_drop(TfwConnection *conn)
{
	/* Ask higher levels to free resources at connection close. */
	TFW_CONN_HOOK_CALL(conn, conn_drop);
	BUG_ON(conn->msg);
}

/*
 * Publish the "connection is released" event via TfwConnHooks.
 */
void
tfw_connection_release(TfwConnection *conn)
{
	/* Ask higher levels to free resources at connection release. */
	TFW_CONN_HOOK_CALL(conn, conn_release);
	BUG_ON(!list_empty(&conn->msg_queue));
}

/*
 * Code architecture decisions ensure that conn->sk remains valid
 * for the life of @conn instance. The socket itself may have been
 * closed, but not deleted. ss_send() makes sure that data is sent
 * only on an active socket.
 */
int
tfw_connection_send(TfwConnection *conn, TfwMsg *msg)
{
	if (conn->tls) {
		struct sk_buff *skb;

		while ((skb = ss_skb_dequeue(&msg->skb_list))) {
			int r, i;

			if (skb_headlen(skb)) {
				r = mbedtls_ssl_write(&conn->tls->ctx, skb->data, skb_headlen(skb));
			}

			for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
				const skb_frag_t *f = &skb_shinfo(skb)->frags[i];
				r = mbedtls_ssl_write(&conn->tls->ctx, skb_frag_address(f), f->size);
			}

			kfree_skb(skb);
		}

		return 0;
	}

	return ss_send(conn->sk, &msg->skb_list, msg->ss_flags);
}

int
tfw_connection_recv(void *cdata, struct sk_buff *skb, unsigned int off)
{
	TfwConnection *conn = cdata;

	if (!conn->msg) {
		conn->msg = TFW_CONN_HOOK_CALL(conn, conn_msg_alloc);
		if (!conn->msg) {
			__kfree_skb(skb);
			return -ENOMEM;
		}
		TFW_DBG("Link new msg %p with connection %p\n",
			conn->msg, conn);
	}

	return tfw_gfsm_dispatch(&conn->msg->state, conn, skb, off);
}

void
tfw_connection_hooks_register(TfwConnHooks *hooks, int type)
{
	unsigned hid = TFW_CONN_TYPE2IDX(type);

	BUG_ON(hid >= TFW_CONN_MAX_PROTOS || conn_hooks[hid]);

	conn_hooks[hid] = hooks;
}

void
tfw_connection_hooks_unregister(int type)
{
	conn_hooks[TFW_CONN_TYPE2IDX(type)] = NULL;
}
