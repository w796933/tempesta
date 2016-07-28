/**
 *		Tempesta FW
 *
 * Copyright (C) 2014 NatSys Lab. (info@natsys-lab.com).
 * Copyright (C) 2015-2016 Tempesta Technologies, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include <linux/string.h>

#include "cache.h"
#include "classifier.h"
#include "client.h"
#include "hash.h"
#include "http_msg.h"
#include "http_sticky.h"
#include "log.h"
#include "procfs.h"
#include "server.h"
#include "tls.h"

#include "sync_socket.h"

#define RESP_BUF_LEN			128
static DEFINE_PER_CPU(char[RESP_BUF_LEN], g_buf);
int ghprio; /* GFSM hook priority. */

#define S_CRLFCRLF		"\r\n\r\n"
#define S_HTTP			"http://"

#define S_200			"HTTP/1.1 200 OK"
#define S_302			"HTTP/1.1 302 Found"
#define S_403			"HTTP/1.1 403 Forbidden"
#define S_404			"HTTP/1.1 404 Not Found"
#define S_500			"HTTP/1.1 500 Internal Server Error"
#define S_502			"HTTP/1.1 502 Bad Gateway"
#define S_504			"HTTP/1.1 504 Gateway Timeout"

#define S_F_HOST		"Host: "
#define S_F_DATE		"Date: "
#define S_F_CONTENT_LENGTH	"Content-Length: "
#define S_F_LOCATION		"Location: "
#define S_F_CONNECTION		"Connection: "

#define S_V_DATE		"Sun, 06 Nov 1994 08:49:37 GMT"
#define S_V_CONTENT_LENGTH	"9999"
#define S_V_CONN_CLOSE		"close"
#define S_V_CONN_KA		"keep-alive"

#define S_H_CONN_KA		S_F_CONNECTION S_V_CONN_KA S_CRLFCRLF
#define S_H_CONN_CLOSE		S_F_CONNECTION S_V_CONN_CLOSE S_CRLFCRLF

/*
 * Prepare current date in the format required for HTTP "Date:"
 * header field. See RFC 2616 section 3.3.
 */
static void
tfw_http_prep_date_from(char *buf, time_t date)
{
	struct tm tm;
	char *ptr = buf;

	static char *wday[] __read_mostly =
		{ "Sun, ", "Mon, ", "Tue, ",
		  "Wed, ", "Thu, ", "Fri, ", "Sat, " };
	static char *month[] __read_mostly =
		{ " Jan ", " Feb ", " Mar ", " Apr ", " May ", " Jun ",
		  " Jul ", " Aug ", " Sep ", " Oct ", " Nov ", " Dec " };

#define PRINT_2DIGIT(p, n)			\
	*p++ = (n <= 9) ? '0' : '0' + n / 10;	\
	*p++ = '0' + n % 10;

	time_to_tm(date, 0, &tm);

	memcpy(ptr, wday[tm.tm_wday], 5);
	ptr += 5;
	PRINT_2DIGIT(ptr, tm.tm_mday);
	memcpy(ptr, month[tm.tm_mon], 5);
	ptr += 5;
	PRINT_2DIGIT(ptr, (tm.tm_year + 1900) / 100);
	PRINT_2DIGIT(ptr, (tm.tm_year + 1900) % 100);
	*ptr++ = ' ';
	PRINT_2DIGIT(ptr, tm.tm_hour);
	*ptr++ = ':';
	PRINT_2DIGIT(ptr, tm.tm_min);
	*ptr++ = ':';
	PRINT_2DIGIT(ptr, tm.tm_sec);
	memcpy(ptr, " GMT", 4);
#undef PRINT_2DIGIT
}

static inline void
tfw_http_prep_date(char *buf)
{
	tfw_http_prep_date_from(buf, tfw_current_timestamp());
}

unsigned long tfw_hash_str(const TfwStr *str);

/*
 * Convert a C string to a printable hex string.
 *
 * Each character makes two hex digits, thus the size of the
 * output buffer must be twice of the length of input string.
 */
void
tfw_http_prep_hexstring(char *buf, u_char *value, size_t len)
{
	char *ptr = buf;

	while (len--) {
		*ptr++ = hex_asc_hi(*value);
		*ptr++ = hex_asc_lo(*value++);
	}
}

#define S_302_PART_01	S_302 S_CRLF S_F_DATE
#define S_302_PART_02	S_CRLF S_F_CONTENT_LENGTH "0" S_CRLF S_F_LOCATION
#define S_302_PART_03	S_CRLF S_F_SET_COOKIE
#define S_302_FIXLEN	SLEN(S_302_PART_01 S_V_DATE S_302_PART_02 S_302_PART_03)
#define S_302_KEEP	S_CRLF S_H_CONN_KA
#define S_302_CLOSE	S_CRLF S_H_CONN_CLOSE
/*
 * HTTP 302 response.
 * The response redirects the client to the same URI as the original request,
 * but it includes 'Set-Cookie:' header field that sets Tempesta sticky cookie.
 */
int
tfw_http_prep_302(TfwHttpMsg *resp, TfwHttpMsg *hmreq, TfwStr *cookie)
{
	size_t data_len = S_302_FIXLEN;
	int conn_flag = hmreq->flags & __TFW_HTTP_CONN_MASK;
	TfwHttpReq *req = (TfwHttpReq *)hmreq;
	TfwMsgIter it;
	TfwStr rh = {
		.ptr = (TfwStr []){
			{ .ptr = S_302_PART_01, .len = SLEN(S_302_PART_01) },
			{ .ptr = *this_cpu_ptr(&g_buf), .len = SLEN(S_V_DATE) },
			{ .ptr = S_302_PART_02, .len = SLEN(S_302_PART_02) }
		},
		.len = SLEN(S_302_PART_01 S_V_DATE S_302_PART_02),
		.flags = 3 << TFW_STR_CN_SHIFT
	};
	static TfwStr part03 = {
		.ptr = S_302_PART_03, .len = SLEN(S_302_PART_03) };
	static TfwStr crlfcrlf = {
		.ptr = S_CRLFCRLF, .len = SLEN(S_CRLFCRLF) };
	static TfwStr crlf_keep = {
		.ptr = S_302_KEEP, .len = SLEN(S_302_KEEP) };
	static TfwStr crlf_close = {
		.ptr = S_302_CLOSE, .len = SLEN(S_302_CLOSE) };
	TfwStr host, *crlf = &crlfcrlf;

	if (!(req->flags & TFW_HTTP_STICKY_SET))
		return TFW_BLOCK;

	tfw_http_msg_clnthdr_val(&req->h_tbl->tbl[TFW_HTTP_HDR_HOST],
				 TFW_HTTP_HDR_HOST, &host);
	if (TFW_STR_EMPTY(&host))
		host = req->host;

	/* Set "Connection:" header field if needed. */
	if (conn_flag == TFW_HTTP_CONN_CLOSE)
		crlf = &crlf_close;
	else if (conn_flag == TFW_HTTP_CONN_KA)
		crlf = &crlf_keep;

	/* Add variable part of data length to get the total */
	data_len += host.len ? host.len + SLEN(S_HTTP) : 0;
	data_len += req->uri_path.len + cookie->len;
	data_len += crlf->len;

	if (!tfw_http_msg_create(resp, &it, Conn_Srv, data_len))
		return TFW_BLOCK;

	tfw_http_prep_date(__TFW_STR_CH(&rh, 1)->ptr);
	tfw_http_msg_write(&it, resp, &rh);
	/*
	 * HTTP/1.0 may have no host part, so we create relative URI.
	 * See RFC 1945 9.3 and RFC 7231 7.1.2.
	 */
	if (host.len) {
		static TfwStr proto = { .ptr = S_HTTP, .len = SLEN(S_HTTP) };
		tfw_http_msg_write(&it, resp, &proto);
		tfw_http_msg_write(&it, resp, &host);
	}
	tfw_http_msg_write(&it, resp, &req->uri_path);
	tfw_http_msg_write(&it, resp, &part03);
	tfw_http_msg_write(&it, resp, cookie);
	tfw_http_msg_write(&it, resp, crlf);

	return TFW_PASS;
}

static inline void
__init_req_ss_flags(TfwHttpReq *req)
{
	((TfwMsg *)req)->ss_flags |= SS_F_KEEP_SKB;
}

static inline void
__init_resp_ss_flags(TfwHttpResp *resp, const TfwHttpReq *req)
{
	if (req->flags & TFW_HTTP_CONN_CLOSE)
		((TfwMsg *)resp)->ss_flags |= SS_F_CONN_CLOSE;
}

/*
 * Perform operations common to sending an error response to a client.
 * Set current date in the header of an HTTP error response, and set
 * the "Connection:" header field if it was present in the request.
 *
 * NOTE: This function expects that the last chunk of @msg is CRLF.
 */
static int
tfw_http_send_resp(TfwHttpMsg *hmreq, TfwStr *msg, const TfwStr *date)
{
	int conn_flag = hmreq->flags & __TFW_HTTP_CONN_MASK;
	TfwStr *crlf = __TFW_STR_CH(msg, TFW_STR_CHUNKN(msg) - 1);
	TfwHttpMsg resp;
	TfwMsgIter it;

	if (conn_flag) {
		unsigned long crlf_len = crlf->len;
		if (conn_flag == TFW_HTTP_CONN_KA) {
			crlf->ptr = S_H_CONN_KA;
			crlf->len = SLEN(S_H_CONN_KA);
		} else {
			crlf->ptr = S_H_CONN_CLOSE;
			crlf->len = SLEN(S_H_CONN_CLOSE);
		}
		msg->len += crlf->len - crlf_len;
	}

	if (!tfw_http_msg_create(&resp, &it, Conn_Srv, msg->len))
		return -ENOMEM;

	tfw_http_prep_date(date->ptr);
	tfw_http_msg_write(&it, &resp, msg);

	__init_resp_ss_flags((TfwHttpResp *)&resp, (TfwHttpReq *)hmreq);

	return tfw_cli_conn_send(hmreq->conn, (TfwMsg *)&resp);
}

#define S_200_PART_01	S_200 S_CRLF S_F_DATE
#define S_200_PART_02	S_CRLF S_F_CONTENT_LENGTH "0" S_CRLF
/*
 * HTTP 200 response: Success.
 */
int
tfw_http_send_200(TfwHttpMsg *hmreq)
{
	TfwStr rh = {
		.ptr = (TfwStr []){
			{ .ptr = S_200_PART_01, .len = SLEN(S_200_PART_01) },
			{ .ptr = *this_cpu_ptr(&g_buf), .len = SLEN(S_V_DATE) },
			{ .ptr = S_200_PART_02, .len = SLEN(S_200_PART_02) },
			{ .ptr = S_CRLF, .len = SLEN(S_CRLF) },
		},
		.len = SLEN(S_200_PART_01 S_V_DATE S_200_PART_02 S_CRLF),
		.flags = 4 << TFW_STR_CN_SHIFT
	};

	TFW_DBG("Send HTTP 200 response to the client\n");

	return tfw_http_send_resp(hmreq, &rh, __TFW_STR_CH(&rh, 1));
}

#define S_403_PART_01	S_403 S_CRLF S_F_DATE
#define S_403_PART_02	S_CRLF S_F_CONTENT_LENGTH "0" S_CRLF
/*
 * HTTP 403 response: Access is forbidden.
 */
int
tfw_http_send_403(TfwHttpMsg *hmreq)
{
	TfwStr rh = {
		.ptr = (TfwStr []){
			{ .ptr = S_403_PART_01, .len = SLEN(S_403_PART_01) },
			{ .ptr = *this_cpu_ptr(&g_buf), .len = SLEN(S_V_DATE) },
			{ .ptr = S_403_PART_02, .len = SLEN(S_403_PART_02) },
			{ .ptr = S_CRLF, .len = SLEN(S_CRLF) },
		},
		.len = SLEN(S_403_PART_01 S_V_DATE S_403_PART_02 S_CRLF),
		.flags = 4 << TFW_STR_CN_SHIFT
	};

	TFW_DBG("Send HTTP 404 response to the client\n");

	return tfw_http_send_resp(hmreq, &rh, __TFW_STR_CH(&rh, 1));
}

#define S_404_PART_01	S_404 S_CRLF S_F_DATE
#define S_404_PART_02	S_CRLF S_F_CONTENT_LENGTH "0" S_CRLF
/*
 * HTTP 404 response: Tempesta is unable to find the requested data.
 */
int
tfw_http_send_404(TfwHttpMsg *hmreq)
{
	TfwStr rh = {
		.ptr = (TfwStr []){
			{ .ptr = S_404_PART_01, .len = SLEN(S_404_PART_01) },
			{ .ptr = *this_cpu_ptr(&g_buf), .len = SLEN(S_V_DATE) },
			{ .ptr = S_404_PART_02, .len = SLEN(S_404_PART_02) },
			{ .ptr = S_CRLF, .len = SLEN(S_CRLF) },
		},
		.len = SLEN(S_404_PART_01 S_V_DATE S_404_PART_02 S_CRLF),
		.flags = 4 << TFW_STR_CN_SHIFT
	};

	TFW_DBG("Send HTTP 404 response to the client\n");

	return tfw_http_send_resp(hmreq, &rh, __TFW_STR_CH(&rh, 1));
}

#define S_500_PART_01	S_500 S_CRLF S_F_DATE
#define S_500_PART_02	S_CRLF S_F_CONTENT_LENGTH "0" S_CRLF
/*
 * HTTP 500 response: there was an internal error while forwarding
 * the request to a server.
 */
static int
tfw_http_send_500(TfwHttpMsg *hmreq)
{
	TfwStr rh = {
		.ptr = (TfwStr []){
			{ .ptr = S_500_PART_01, .len = SLEN(S_500_PART_01) },
			{ .ptr = *this_cpu_ptr(&g_buf), .len = SLEN(S_V_DATE) },
			{ .ptr = S_500_PART_02, .len = SLEN(S_500_PART_02) },
			{ .ptr = S_CRLF, .len = SLEN(S_CRLF) },
		},
		.len = SLEN(S_500_PART_01 S_V_DATE S_500_PART_02 S_CRLF),
		.flags = 4 << TFW_STR_CN_SHIFT
	};

	TFW_DBG("Send HTTP 500 response to the client\n");

	return tfw_http_send_resp(hmreq, &rh, __TFW_STR_CH(&rh, 1));
}

#define S_502_PART_01	S_502 S_CRLF S_F_DATE
#define S_502_PART_02	S_CRLF S_F_CONTENT_LENGTH "0" S_CRLF
/*
 * HTTP 502 response: Tempesta is unable to forward the request to
 * the designated server.
 */
int
tfw_http_send_502(TfwHttpMsg *hmreq)
{
	TfwStr rh = {
		.ptr = (TfwStr []){
			{ .ptr = S_502_PART_01, .len = SLEN(S_502_PART_01) },
			{ .ptr = *this_cpu_ptr(&g_buf), .len = SLEN(S_V_DATE) },
			{ .ptr = S_502_PART_02, .len = SLEN(S_502_PART_02) },
			{ .ptr = S_CRLF, .len = SLEN(S_CRLF) },
		},
		.len = SLEN(S_502_PART_01 S_V_DATE S_502_PART_02 S_CRLF),
		.flags = 4 << TFW_STR_CN_SHIFT
	};

	TFW_DBG("Send HTTP 502 response to the client\n");

	return tfw_http_send_resp(hmreq, &rh, __TFW_STR_CH(&rh, 1));
}

#define S_504_PART_01	S_504 S_CRLF S_F_DATE
#define S_504_PART_02	S_CRLF S_F_CONTENT_LENGTH "0" S_CRLF
/*
 * HTTP 504 response: did not receive a timely response from
 * the designated server.
 */
int
tfw_http_send_504(TfwHttpMsg *hmreq)
{
	TfwStr rh = {
		.ptr = (TfwStr []){
			{ .ptr = S_504_PART_01, .len = SLEN(S_504_PART_01) },
			{ .ptr = *this_cpu_ptr(&g_buf), .len = SLEN(S_V_DATE) },
			{ .ptr = S_504_PART_02, .len = SLEN(S_504_PART_02) },
			{ .ptr = S_CRLF, .len = SLEN(S_CRLF) },
		},
		.len = SLEN(S_504_PART_01 S_V_DATE S_504_PART_02 S_CRLF),
		.flags = 4 << TFW_STR_CN_SHIFT
	};

	TFW_DBG("Send HTTP 504 response to the client\n");

	return tfw_http_send_resp(hmreq, &rh, __TFW_STR_CH(&rh, 1));
}

/*
 * Allocate a new HTTP message structure, and link it with
 * the connection structure. Increment the number of users
 * of the connection structure. Initialize GFSM for the message.
 */
static TfwMsg *
tfw_http_conn_msg_alloc(TfwConnection *conn)
{
	TfwHttpMsg *hm = tfw_http_msg_alloc(TFW_CONN_TYPE(conn));
	if (unlikely(!hm))
		return NULL;

	hm->conn = conn;
	tfw_connection_get(conn);
	tfw_gfsm_state_init(&hm->msg.state, conn, TFW_HTTP_FSM_INIT);

        if (TFW_CONN_TYPE(conn) & Conn_Clnt) {
                TFW_INC_STAT_BH(clnt.rx_messages);
        } else {
		TfwHttpReq *req;

		spin_lock(&conn->msg_qlock);
		req = (TfwHttpReq *)list_first_entry_or_null(&conn->msg_queue,
							     TfwMsg, msg_list);
		spin_unlock(&conn->msg_qlock);
		if (req && (req->method == TFW_HTTP_METH_HEAD))
			hm->flags |= TFW_HTTP_VOID_BODY;
                TFW_INC_STAT_BH(serv.rx_messages);
	}

	return (TfwMsg *)hm;
}

/*
 * Free an HTTP message.
 * Also, free the connection structure if there's no more references.
 *
 * This function should be used anytime when there's a chance that
 * a connection structure may belong to multiple messages, which is
 * almost always. If a connection is suddenly closed then it still
 * can be safely dereferenced and used in the code.
 * In rare cases we're sure that a connection structure in a message
 * doesn't have multiple users. For instance, when an error response
 * is prepared and sent by Tempesta, that HTTP message does not need
 * a connection structure. The message is then immediately destroyed,
 * and a simpler tfw_http_msg_free() can be used for that.
 */
static void
tfw_http_conn_msg_free(TfwHttpMsg *hm)
{
	if (unlikely(hm == NULL))
		return;
	if (tfw_connection_put(hm->conn)) {
		/* The connection and underlying socket seems closed. */
		TFW_CONN_TYPE(hm->conn) & Conn_Clnt
			? tfw_cli_conn_release(hm->conn)
			: tfw_srv_conn_release(hm->conn);
		hm->conn = NULL;
	}
	tfw_http_msg_free(hm);
}

/**
 * TODO Initialize allocated Client structure by HTTP specific callbacks
 * and FSM.
 */
static int
tfw_http_conn_init(TfwConnection *conn)
{
	return 0;
}

/*
 * Connection with a peer is released.
 *
 * For server connections requests that were sent to that server are kept
 * in the queue until a paired response comes. That will never happen now.
 * For each request that has been unanswered send an error response, then
 * delete the request and drop the connection with the client if required.
 *
 * Called when a connection is released. There are no users at that time,
 * so locks are not needed.
 */
static void
tfw_http_conn_release(TfwConnection *conn)
{
	TfwMsg *msg, *tmp;

	list_for_each_entry_safe(msg, tmp, &conn->msg_queue, msg_list) {
		BUG_ON(((TfwHttpMsg *)msg)->conn
			&& (((TfwHttpMsg *)msg)->conn == conn));
		list_del(&msg->msg_list);
		tfw_http_send_404((TfwHttpMsg *)msg);
		tfw_http_conn_msg_free((TfwHttpMsg *)msg);
		TFW_INC_STAT_BH(clnt.msgs_otherr);
	}
	INIT_LIST_HEAD(&conn->msg_queue);
}

/*
 * Connection with a peer is dropped.
 *
 * Release resources that are not needed anymore, and keep other
 * resources that are needed while there are users of the connection.
 */
static void tfw_http_resp_terminate(TfwHttpMsg *hm);

static void
tfw_http_conn_drop(TfwConnection *conn)
{
	if (conn->msg && (TFW_CONN_TYPE(conn) & Conn_Srv)) {
		if (tfw_http_parse_terminate((TfwHttpMsg *)conn->msg)) {
			tfw_http_resp_terminate((TfwHttpMsg *)conn->msg);
		}
	}
	tfw_http_conn_msg_free((TfwHttpMsg *)conn->msg);
}

/**
 * Create a sibling for @msg message.
 * Siblings in HTTP are pipelined requests that share the same SKB.
 */
static TfwHttpMsg *
tfw_http_msg_create_sibling(TfwHttpMsg *hm, struct sk_buff **skb,
			    unsigned int split_offset, int type)
{
	TfwHttpMsg *shm;
	struct sk_buff *nskb;

	TFW_DBG2("Create sibling message: conn %p, skb %p\n", hm->conn, skb);

	/* The sibling message belongs to the same connection. */
	shm = (TfwHttpMsg *)tfw_http_conn_msg_alloc(hm->conn);
	if (unlikely(!shm))
		return NULL;

	/*
	 * The sibling message is set up with a new SKB as
	 * the starting SKB. The new SKB is split off from
	 * the original SKB and contains the first part of
	 * new message. The original SKB is shrunk to have
	 * just data from the original message.
	 */
	nskb = ss_skb_split(*skb, split_offset);
	if (!nskb) {
		tfw_http_conn_msg_free(shm);
		return NULL;
	}
	ss_skb_queue_tail(&shm->msg.skb_list, nskb);
	*skb = nskb;

	return shm;
}

static int
tfw_http_set_hdr_date(TfwHttpMsg *hm)
{
	int r;
	char *s_date = *this_cpu_ptr(&g_buf);

	tfw_http_prep_date_from(s_date, ((TfwHttpResp *)hm)->date);
	r = tfw_http_msg_hdr_xfrm(hm, "Date", sizeof("Date") - 1,
				  s_date, SLEN(S_V_DATE),
				  TFW_HTTP_HDR_RAW, 0);
	if (r)
		TFW_ERR("Unable to add Date: header to msg [%p]\n", hm);
	else
		TFW_DBG2("Added Date: header to msg [%p]\n", hm);
	return r;
}

/**
 * Remove Connection header from HTTP message @msg if @conn_flg is zero,
 * and replace or set a new header value otherwise.
 *
 * skb's can be shared between number of HTTP messages. We don't copy skb if
 * it's shared - we modify skb's safely and shared skb is still owned by one
 * CPU.
 */
static int
tfw_http_set_hdr_connection(TfwHttpMsg *hm, int conn_flg)
{
	if (((hm->flags & __TFW_HTTP_CONN_MASK) == conn_flg)
	    && (!TFW_STR_EMPTY(&hm->h_tbl->tbl[TFW_HTTP_HDR_CONNECTION])))
		return 0;

	switch (conn_flg) {
	case TFW_HTTP_CONN_CLOSE:
		return TFW_HTTP_MSG_HDR_XFRM(hm, "Connection", "close",
					     TFW_HTTP_HDR_CONNECTION, 0);
	case TFW_HTTP_CONN_KA:
		return TFW_HTTP_MSG_HDR_XFRM(hm, "Connection", "keep-alive",
					     TFW_HTTP_HDR_CONNECTION, 0);
	default:
		return TFW_HTTP_MSG_HDR_DEL(hm, "Connection",
					    TFW_HTTP_HDR_CONNECTION);
	}
}

/*
 * Add/Replace/Remove Keep-Alive header field to/from HTTP message.
 */
static int
tfw_http_set_hdr_keep_alive(TfwHttpMsg *hm, int conn_flg)
{
	int r;

	if ((hm->flags & __TFW_HTTP_CONN_MASK) == conn_flg)
		return 0;

	switch (conn_flg) {
	case TFW_HTTP_CONN_CLOSE:
		r = TFW_HTTP_MSG_HDR_DEL(hm, "Keep-Alive", TFW_HTTP_HDR_RAW);
		if (unlikely(r && r != -ENOENT)) {
			TFW_WARN("Cannot delete Keep-Alive header (%d)\n", r);
			return r;
		}
		return 0;
	case TFW_HTTP_CONN_KA:
		/*
		 * If present, "Keep-Alive" header informs the other side
		 * of the timeout policy for a connection. Otherwise, it's
		 * presumed that default policy is in action.
		 *
		 * TODO: Add/Replace "Keep-Alive" header when Tempesta
		 * implements connection timeout policies and the policy
		 * for the connection differs from default policy.
		 */
		return 0;
	default:
		/*
		 * "Keep-Alive" header mandates that "Connection: keep-alive"
		 * header in present in HTTP message. HTTP/1.1 connections
		 * are keep-alive by default. If we want to add "Keep-Alive"
		 * header then "Connection: keep-alive" header must be added
		 * as well. TFW_HTTP_CONN_KA flag will force the addition of
		 * "Connection: keep-alive" header to HTTP message.
		 */
		return 0;
	}
}

static int
tfw_http_add_hdr_via(TfwHttpMsg *hm)
{
	int r;
	static const char const * __read_mostly s_http_version[] = {
		[0 ... _TFW_HTTP_VER_COUNT] = NULL,
		[TFW_HTTP_VER_09] = "0.9 ",
		[TFW_HTTP_VER_10] = "1.0 ",
		[TFW_HTTP_VER_11] = "1.1 ",
		[TFW_HTTP_VER_20] = "2.0 ",
	};
	TfwVhost *vhost = tfw_vhost_get_default();
	TfwStr rh = {
#define S_VIA	"Via: "
		.ptr = (TfwStr []) {
			{ .ptr = S_VIA, .len = SLEN(S_VIA) },
			{ .ptr = (void *)s_http_version[hm->version],
			  .len = 4 },
			{ .ptr = *this_cpu_ptr(&g_buf),
			  .len = vhost->hdr_via_len },
		},
		.len = SLEN(S_VIA) + 4 + vhost->hdr_via_len,
		.eolen = 2,
		.flags = 3 << TFW_STR_CN_SHIFT
#undef S_VIA
	};

	memcpy(__TFW_STR_CH(&rh, 2)->ptr, vhost->hdr_via, vhost->hdr_via_len);

	r = tfw_http_msg_hdr_add(hm, &rh);
	if (r)
		TFW_ERR("Unable to add Via: header to msg [%p]\n", hm);
	else
		TFW_DBG2("Added Via: header to msg [%p]\n", hm);
	return r;
}

static int
tfw_http_add_x_forwarded_for(TfwHttpMsg *hm)
{
	int r;
	char *p, *buf = *this_cpu_ptr(&g_buf);

	p = ss_skb_fmt_src_addr(hm->msg.skb_list.first, buf);

	r = tfw_http_msg_hdr_xfrm(hm, "X-Forwarded-For",
				  sizeof("X-Forwarded-For") - 1, buf, p - buf,
				  TFW_HTTP_HDR_X_FORWARDED_FOR, true);
	if (r)
		TFW_ERR("can't add X-Forwarded-For header for %.*s to msg %p",
			(int)(p - buf), buf, hm);
	else
		TFW_DBG2("added X-Forwarded-For header for %*s\n",
			 (int)(p - buf), buf);
	return r;
}

/**
 * Adjust the request before proxying it to real server.
 */
static int
tfw_http_adjust_req(TfwHttpReq *req)
{
	int r;
	TfwHttpMsg *hm = (TfwHttpMsg *)req;

	__init_req_ss_flags(req);

	r = tfw_http_add_x_forwarded_for(hm);
	if (r)
		return r;

	r = tfw_http_add_hdr_via(hm);
	if (r)
		return r;

	return tfw_http_set_hdr_connection(hm, TFW_HTTP_CONN_KA);
}

/**
 * Adjust the response before proxying it to real client.
 */
static int
tfw_http_adjust_resp(TfwHttpResp *resp, TfwHttpReq *req)
{
	int r, conn_flg = req->flags & __TFW_HTTP_CONN_MASK;
	TfwHttpMsg *hm = (TfwHttpMsg *)resp;

	__init_resp_ss_flags(resp, req);

	r = tfw_http_sticky_resp_process(hm, (TfwHttpMsg *)req);
	if (r < 0)
		return r;

	r = tfw_http_set_hdr_keep_alive(hm, conn_flg);
	if (r < 0)
		return r;

	r = tfw_http_set_hdr_connection(hm, conn_flg);
	if (r < 0)
		return r;

	r = tfw_http_add_hdr_via(hm);
	if (resp->flags & TFW_HTTP_RESP_STALE) {
#define S_WARN_110_N "Warning:"
#define S_WARN_110_V "110 - Response is stale"

		r = tfw_http_msg_hdr_xfrm(hm, S_WARN_110_N, SLEN(
					  S_WARN_110_N), S_WARN_110_V, 
				          SLEN(S_WARN_110_V), 
					  TFW_HTTP_HDR_RAW, 0);
		if (r)
			return r;
#undef S_WARN_110_N
#undef S_WARN_110_V
	}
	if (!(resp->flags & TFW_HTTP_HAS_HDR_DATE)) {
		r =  tfw_http_set_hdr_date(hm);
		if (r < 0)
			return r;
	}

	return TFW_HTTP_MSG_HDR_XFRM(hm, "Server", TFW_NAME "/" TFW_VERSION,
				     TFW_HTTP_HDR_SERVER, 0);
}

/*
 * Depending on results of processing of a request, either send the request
 * to an appropriate server, or return the cached response. If none of that
 * can be done for any reason, return HTTP 404 or 500 error to the client.
 */
static void
tfw_http_req_cache_cb(TfwHttpReq *req, TfwHttpResp *resp)
{
	int r;
	TfwConnection *srv_conn;

	if (resp) {
		/*
		 * The request is served from cache.
		 * Send the response as is and unrefer its data.
		 */
		if (tfw_http_adjust_resp(resp, req))
			goto resp_err;
		if (tfw_cli_conn_send(req->conn, (TfwMsg *)resp))
			goto resp_err;
		TFW_INC_STAT_BH(clnt.msgs_fromcache);
resp_out:
		tfw_http_conn_msg_free((TfwHttpMsg *)resp);
		tfw_http_conn_msg_free((TfwHttpMsg *)req);
		return;
resp_err:
		tfw_http_send_500((TfwHttpMsg *)req);
		TFW_INC_STAT_BH(clnt.msgs_otherr);
		goto resp_out;
	}

	/*
	 * Dispatch request to an appropriate server. Schedulers
	 * should make a decision based on an unmodified request,
	 * so this must be done before any request mangling.
	 *
	 * The code below is typically called on remote NUMA node.
	 * That's not good, but we must run TDB lookup on the node
	 * before this is executed, to avoid unnecessary work in
	 * SoftIRQ and to speed up the cache operation.
	 * At the same time, cache hits are expected to prevail
	 * over cache misses, so this is not a frequent path.
	 */
	srv_conn = tfw_sched_get_srv_conn((TfwMsg *)req);
	if (srv_conn == NULL) {
		TFW_WARN("Unable to find a backend server\n");
		goto send_404;
	}

	/*
	 * Sticky cookie module may send a response to the client
	 * when sticky cookie presence is enforced and the cookie
	 * is missing from the request.
	 */
	r = tfw_http_sticky_req_process((TfwHttpMsg *)req);
	if (r < 0) {
		goto send_500;
	}
	else if (r > 0) {
		/* Response sent, nothing to do */
		tfw_http_conn_msg_free((TfwHttpMsg *)req);
		goto conn_put;
	}
	if (tfw_http_adjust_req(req))
		goto send_500;

	/* Add request to the server connection. */
	spin_lock(&srv_conn->msg_qlock);
	list_add_tail(&req->msg.msg_list, &srv_conn->msg_queue);
	spin_unlock(&srv_conn->msg_qlock);

	/* Send request to the server. */
	if (tfw_connection_send(srv_conn, (TfwMsg *)req)) {
		spin_lock(&srv_conn->msg_qlock);
		list_del(&req->msg.msg_list);
		spin_unlock(&srv_conn->msg_qlock);
		goto send_500;
	}
	TFW_INC_STAT_BH(clnt.msgs_forwarded);
	goto conn_put;

send_404:
	tfw_http_send_404((TfwHttpMsg *)req);
	tfw_http_conn_msg_free((TfwHttpMsg *)req);
	TFW_INC_STAT_BH(clnt.msgs_otherr);
	return;
send_500:
	tfw_http_send_500((TfwHttpMsg *)req);
	tfw_http_conn_msg_free((TfwHttpMsg *)req);
	TFW_INC_STAT_BH(clnt.msgs_otherr);
conn_put:
	if (tfw_connection_put(srv_conn))
		tfw_srv_conn_release(srv_conn);
}

static int
tfw_http_req_set_context(TfwHttpReq *req)
{
	req->vhost = tfw_vhost_match(&req->uri_path);
	req->location = tfw_location_match(req->vhost, &req->uri_path);
	return (!req->vhost);
}

/**
 * @return zero on success and negative value otherwise.
 * TODO enter the function depending on current GFSM state.
 */
static int
tfw_http_req_process(TfwConnection *conn, struct sk_buff *skb, unsigned int off)
{
	int r = TFW_BLOCK;
	unsigned int data_off = off;
	unsigned int skb_len = skb->len;

	BUG_ON(!conn->msg);
	BUG_ON(data_off >= skb_len);

	TFW_DBG2("Received %u client data bytes on conn=%p msg=%p\n",
		 skb_len - off, conn, conn->msg);

	/*
	 * Process pipelined requests in a loop
	 * until all data in the SKB is processed.
	 */
	while (data_off < skb_len) {
		int req_conn_close;
		TfwHttpMsg *hmsib = NULL;
		TfwHttpMsg *hmreq = (TfwHttpMsg *)conn->msg;
		TfwHttpParser *parser = &hmreq->parser;

		/*
		 * Process/parse data in the SKB.
		 * @off points at the start of data for processing.
		 * @data_off is the current offset of data to process in
		 * the SKB. After processing @data_off points at the end
		 * of latest data chunk. However processing may have
		 * stopped in the middle of the chunk. Adjust it to point 
		 * to the right location within the chunk.
		 */
		off = data_off;
		r = ss_skb_process(skb, &data_off, tfw_http_parse_req, hmreq);
		data_off -= parser->to_go;
		hmreq->msg.len += data_off - off;
		TFW_ADD_STAT_BH(data_off - off, clnt.rx_bytes);

		TFW_DBG2("Request parsed: len=%u parsed=%d msg_len=%lu"
			 " ver=%d res=%d\n",
			 skb_len - off, data_off - off, hmreq->msg.len,
			 hmreq->version, r);

		switch (r) {
		default:
			TFW_ERR("Unrecognized HTTP request "
				"parser return code, %d\n", r);
			BUG();
		case TFW_BLOCK:
			TFW_DBG2("Block invalid HTTP request\n");
			TFW_INC_STAT_BH(clnt.msgs_parserr);
			return TFW_BLOCK;
		case TFW_POSTPONE:
			r = tfw_gfsm_move(&hmreq->msg.state,
					  TFW_HTTP_FSM_REQ_CHUNK, skb, off);
			TFW_DBG3("TFW_HTTP_FSM_REQ_CHUNK return code %d\n", r);
			if (r == TFW_BLOCK) {
				TFW_INC_STAT_BH(clnt.msgs_filtout);
				return TFW_BLOCK;
			}
			/*
			 * TFW_POSTPONE status means that parsing succeeded
			 * but more data is needed to complete it. Lower layers
			 * just supply data for parsing. They only want to know
			 * if processing of a message should continue or not.
			 */
			return TFW_PASS;
		case TFW_PASS:
			/*
			 * The request is fully parsed,
			 * fall through and process it.
			 */
			;
		}

		r = tfw_gfsm_move(&hmreq->msg.state,
				  TFW_HTTP_FSM_REQ_MSG, skb, off);
		TFW_DBG3("TFW_HTTP_FSM_REQ_MSG return code %d\n", r);
		/* Don't accept any following requests from the peer. */
		if (r == TFW_BLOCK) {
			TFW_INC_STAT_BH(clnt.msgs_filtout);
			return TFW_BLOCK;
		}

		/*
		 * The time the request was received is used in cache
		 * for age calculations, and for APM and Load Balancing.
		 */
		hmreq->cache_ctl.timestamp = tfw_current_timestamp();

		/* Assign the right Vhost for this request. */
		if (tfw_http_req_set_context((TfwHttpReq *)hmreq))
			return TFW_BLOCK;

		/*
		 * In HTTP 0.9 the server always closes the connection
		 * after sending the response.
		 *
		 * In HTTP 1.0 the server always closes the connection
		 * after sending the response unless the client sent a
		 * a "Connection: keep-alive" request header, and the
		 * server sent a "Connection: keep-alive" response header.
		 *
		 * This behavior was added to existing HTTP 1.0 protocol.
		 * RFC 1945 section 1.3 says:
		 * "Except for experimental applications, current practice
		 * requires that the connection be established by the client
		 * prior to each request and closed by the server after
		 * sending the response."
		 *
		 * Make it work this way in Tempesta by setting the flag.
		 */
		if ((hmreq->version == TFW_HTTP_VER_09)
		    || ((hmreq->version == TFW_HTTP_VER_10)
			&& !(hmreq->flags & __TFW_HTTP_CONN_MASK)))
		{
			hmreq->flags |= TFW_HTTP_CONN_CLOSE;
		}

		/*
		 * The request has been successfully parsed and processed.
		 * If the connection will be closed after the response to
		 * the request is sent to the client, then there's no need
		 * to process pipelined requests. Also, the request may be
		 * released when handled in tfw_cache_req_process() below.
		 * So, save the needed request flag for later use as it
		 * may not be accessible later through @req->flags.
		 */
		req_conn_close = (hmreq->flags & TFW_HTTP_CONN_CLOSE);

		if (!req_conn_close && (data_off < skb_len)) {
			/*
			 * Pipelined requests: create a new sibling message.
			 * @skb is replaced with pointer to a new SKB.
			 */
			hmsib = tfw_http_msg_create_sibling(hmreq, &skb,
							    data_off,
							    Conn_Clnt);
			if (hmsib == NULL) {
				/*
				 * Not enough memory. Unfortunately, there's
				 * no recourse. The caller expects that data
				 * is processed in full, and can't deal with
				 * partially processed data.
				 */
				TFW_WARN("Not enough memory to create"
					 " a request sibling\n");
				TFW_INC_STAT_BH(clnt.msgs_otherr);
				return TFW_BLOCK;
			}
		}

		/*
		 * Complete HTTP message has been collected and processed
		 * with success. Mark the message as complete in @conn as
		 * further handling of @conn depends on that. Future SKBs
		 * will be put in a new message.
		 * On an error the function returns from anywhere inside
		 * the loop. @conn->msg holds the reference to the message,
		 * which can be used to release it.
		 */
		tfw_connection_unlink_msg(conn);

		/*
		 * The request should either be stored or released.
		 * Otherwise we lose the reference to it and get a leak.
		 */
		if (tfw_cache_process((TfwHttpReq *)hmreq, NULL,
				      tfw_http_req_cache_cb))
		{
			tfw_http_send_500(hmreq);
			tfw_http_conn_msg_free(hmreq);
			TFW_INC_STAT_BH(clnt.msgs_otherr);
			return TFW_PASS;
		}

		/*
		 * According to RFC 7230 6.3.2, connection with a client
		 * must be dropped after a response is sent to that client,
		 * if the client sends "Connection: close" header field in
		 * the request. Subsequent requests from the client coming
		 * over the same connection are ignored.
		 *
		 * Note: This connection's @conn must not be dereferenced
		 * from this point on.
		 */
		if (req_conn_close)
			return TFW_STOP;

		if (hmsib) {
			/*
			 * Switch connection to the new sibling message.
			 * Data processing will continue with the new SKB.
			 */
			data_off = 0;
			skb_len = skb->len;
			conn->msg = (TfwMsg *)hmsib;
		}
	}

	return r;
}

/**
 * This is the second half of tfw_http_resp_process().
 * tfw_http_resp_process() runs in SoftIRQ whereas tfw_http_resp_cache_cb()
 * runs in cache thread that is scheduled at an appropriate TDB node.
 *
 * HTTP requests are usually much smaller than HTTP responses, so it's
 * better to transfer requests to a TDB node to make any adjustments.
 * The other benefit of the scheme is that less work is done in SoftIRQ.
 */
static void
tfw_http_resp_cache_cb(TfwHttpReq *req, TfwHttpResp *resp)
{
	/*
	 * Typically we're at a node far from the node where @resp was
	 * received, so we do an inter-node transfer. However, this is
	 * the final place where the response will be stored. Upcoming
	 * requests will get responded to by the current node without
	 * inter-node data transfers. (see tfw_http_req_cache_cb())
	 */
	if (tfw_http_adjust_resp(resp, req))
		goto err;

	if (tfw_cli_conn_send(req->conn, (TfwMsg *)resp))
		goto err;

	TFW_INC_STAT_BH(serv.msgs_forwarded);
out:
	/* Now we don't need the request and the response anymore. */
	tfw_http_conn_msg_free((TfwHttpMsg *)resp);
	tfw_http_conn_msg_free((TfwHttpMsg *)req);
	return;
err:
	tfw_http_send_500((TfwHttpMsg *)req);
	TFW_INC_STAT_BH(serv.msgs_otherr);
	goto out;
}

/*
 * Request messages that were forwarded to a backend server are added
 * to and kept in @msg_queue of the connection @conn for that server.
 * If a paired request is not found, then the response is deleted.
 */
static TfwHttpReq *
tfw_http_popreq(TfwHttpMsg *hmresp)
{
	TfwMsg *msg;
	TfwConnection *conn = hmresp->conn;

	spin_lock(&conn->msg_qlock);
	if (unlikely(list_empty(&conn->msg_queue))) {
		spin_unlock(&conn->msg_qlock);
		/* @conn->msg will get NULLed in the process. */
		TFW_WARN("Paired request missing\n");
		TFW_WARN("Possible HTTP Response Splitting attack.\n");
		tfw_http_conn_msg_free(hmresp);
		TFW_INC_STAT_BH(serv.msgs_otherr);
		return NULL;
	}
	msg = list_first_entry(&conn->msg_queue, TfwMsg, msg_list);
	list_del(&msg->msg_list);
	spin_unlock(&conn->msg_qlock);

	return (TfwHttpReq *)msg;
}

/*
 * Post-process the response. Pass it to modules registered with GFSM
 * for further processing. Finish the request/response exchange properly
 * in case of an error.
 */
static int
tfw_http_resp_gfsm(TfwHttpMsg *hmresp, struct sk_buff *skb, unsigned int off)
{
	int r;
	TfwHttpMsg *hmreq;

	r = tfw_gfsm_move(&hmresp->msg.state, TFW_HTTP_FSM_RESP_MSG, skb, off);
	TFW_DBG3("TFW_HTTP_FSM_RESP_MSG return code %d\n", r);
	if (r == TFW_BLOCK)
		goto error;
	/* Proceed with the next GSFM processing */

	r = tfw_gfsm_move(&hmresp->msg.state,
			  TFW_HTTP_FSM_LOCAL_RESP_FILTER, skb, off);
	TFW_DBG3("TFW_HTTP_FSM_LOCAL_RESP_FILTER return code %d\n", r);
	if (r == TFW_PASS)
		return TFW_PASS;
	/* Proceed with the error processing */
error:
	/*
	 * Send an error response to the client, otherwise the pairing
	 * of requests and responses will be broken. If a paired request
	 * is not found, then something is terribly wrong.
	 */
	hmreq = (TfwHttpMsg *)tfw_http_popreq(hmresp);
	if (unlikely(hmreq == NULL)) {
		TFW_INC_STAT_BH(serv.msgs_filtout);
		return TFW_STOP;
	}

	tfw_http_send_502(hmreq);
	tfw_http_conn_msg_free(hmresp);
	tfw_http_conn_msg_free(hmreq);
	TFW_INC_STAT_BH(serv.msgs_filtout);
	return TFW_BLOCK;
}

static int
tfw_http_resp_cache(TfwHttpMsg *hmresp)
{
	TfwHttpMsg *hmreq;
	time_t timestamp = tfw_current_timestamp();

	/*
	 * The time the response was received is used in cache
	 * for age calculations, and for APM and Load Balancing.
	 */
	hmresp->cache_ctl.timestamp = timestamp;
	/*
	 * If 'Date:' header is missing in the response, then
	 * set the date to the time the response was received.
	 */
	if (!(hmresp->flags & TFW_HTTP_HAS_HDR_DATE))
		((TfwHttpResp *)hmresp)->date = timestamp;
	/*
	 * Cache adjusted and filtered responses only. Responses
	 * are received in the same order as requests, so we can
	 * just pop the first request. If a paired request is not
	 * found, then something is terribly wrong, and pairing
	 * of requests and responses is broken. The response is
	 * deleted, and an error is returned.
	 */
	hmreq = (TfwHttpMsg *)tfw_http_popreq(hmresp);
	if (unlikely(hmreq == NULL))
		return -ENOENT;
	/*
	 * Complete HTTP message has been collected and processed
	 * with success. Mark the message as complete in @conn as
	 * further handling of @conn depends on that. Future SKBs
	 * will be put in a new message.
	 */
	tfw_connection_unlink_msg(hmresp->conn);
	if (tfw_cache_process((TfwHttpReq *)hmreq, (TfwHttpResp *)hmresp,
			      tfw_http_resp_cache_cb))
	{
		tfw_http_send_500(hmreq);
		tfw_http_conn_msg_free(hmresp);
		tfw_http_conn_msg_free(hmreq);
		TFW_INC_STAT_BH(serv.msgs_otherr);
		/* Proceed with processing of the next response. */
	}

	return 0;
}

/*
 * Finish a response that is terminated by closing the connection.
 */
static void
tfw_http_resp_terminate(TfwHttpMsg *hm)
{
	struct sk_buff *skb = ss_skb_peek_tail(&hm->msg.skb_list);

	BUG_ON(!skb);

	/*
	 * Note that in this case we don't have data to process.
	 * All data has been processed already. The response needs
	 * to go through Tempesta's post-processing, and then be
	 * sent to the client. The full skb->len is used as the
	 * offset to mark this case in the post-processing phase.
	 */
	if (tfw_http_resp_gfsm(hm, skb, skb->len) != TFW_PASS)
		return;
	tfw_http_resp_cache(hm);
}

/**
 * @return zero on success and negative value otherwise.
 * TODO enter the function depending on current GFSM state.
 */
static int
tfw_http_resp_process(TfwConnection *conn, struct sk_buff *skb,
		      unsigned int off)
{
	int r = TFW_BLOCK;
	unsigned int data_off = off;
	unsigned int skb_len = skb->len;

	BUG_ON(!conn->msg);
	BUG_ON(data_off >= skb_len);

	TFW_DBG2("received %u server data bytes on conn=%p msg=%p\n",
		skb->len - off, conn, conn->msg);
	/*
	 * Process pipelined requests in a loop
	 * until all data in the SKB is processed.
	 */
	while (data_off < skb_len) {
		TfwHttpMsg *hmsib = NULL;
		TfwHttpMsg *hmresp = (TfwHttpMsg *)conn->msg;
		TfwHttpParser *parser = &hmresp->parser;

		/*
		 * Process/parse data in the SKB.
		 * @off points at the start of data for processing.
		 * @data_off is the current offset of data to process in
		 * the SKB. After processing @data_off points at the end
		 * of latest data chunk. However processing may have
		 * stopped in the middle of the chunk. Adjust it to point 
		 * at correct location within the chunk.
		 */
		off = data_off;
		r = ss_skb_process(skb, &data_off, tfw_http_parse_resp, hmresp);
		data_off -= parser->to_go;
		hmresp->msg.len += data_off - off;
		TFW_ADD_STAT_BH(data_off - off, serv.rx_bytes);

		TFW_DBG2("Response parsed: len=%u parsed=%d msg_len=%lu"
			 " ver=%d res=%d\n",
			 skb_len - off, data_off - off, hmresp->msg.len,
			 hmresp->version, r);

		switch (r) {
		default:
			TFW_ERR("Unrecognized HTTP response "
				"parser return code, %d\n", r);
			BUG();
		case TFW_BLOCK:
			/*
			 * The response has not been fully parsed. There's no
			 * choice but report a critical error. The lower layer
			 * will close the connection and release the response
			 * message, and well as all request messages that went
			 * out on this connection and are waiting for paired
			 * response messages.
			 */
			TFW_DBG2("Block invalid HTTP response\n");
			TFW_INC_STAT_BH(serv.msgs_parserr);
			return TFW_BLOCK;
		case TFW_POSTPONE:
			r = tfw_gfsm_move(&hmresp->msg.state,
					  TFW_HTTP_FSM_RESP_CHUNK, skb, off);
			TFW_DBG3("TFW_HTTP_FSM_RESP_CHUNK return code %d\n", r);
			if (r == TFW_BLOCK) {
				TFW_INC_STAT_BH(serv.msgs_filtout);
				return TFW_BLOCK;
			}
			/*
			 * TFW_POSTPONE status means that parsing succeeded
			 * but more data is needed to complete it. Lower layers
			 * just supply data for parsing. They only want to know
			 * if processing of a message should continue or not.
			 */
			return TFW_PASS;
		case TFW_PASS:
			/*
			 * The response is fully parsed,
			 * fall through and process it.
			 */
			;
		}

		/* Pass the response to GFSM for further processing. */
		r = tfw_http_resp_gfsm(hmresp, skb, off);
		if (unlikely(r == TFW_STOP))
			return TFW_BLOCK;
		/*
		 * If @skb's data has not been processed in full, then
		 * we have pipelined responses. Create a sibling message.
		 * @skb is replaced with a pointer to a new SKB.
		 */
		if (data_off < skb_len) {
			hmsib = tfw_http_msg_create_sibling(hmresp, &skb,
							    data_off,
							    Conn_Srv);
			/*
			 * In case of an error there's no recourse. The
			 * caller expects that data is processed in full,
			 * and can't deal with partially processed data.
			 */
			if (hmsib == NULL) {
				TFW_WARN("Insufficient memory "
					 "to create a response sibling\n");
				TFW_INC_STAT_BH(serv.msgs_otherr);
				return TFW_BLOCK;
			}
		}
		/*
		 * If an error occured in further GFSM processing, then
		 * the response and the paired request had been handled.
		 * Keep the server connection open for data exchange.
		 */
		if (unlikely(r != TFW_PASS)) {
			r = TFW_PASS;
			goto next_resp;
		}
		/*
		 * Pass the response to cache for further processing.
		 * In the end, the response is sent on to the client.
		 */
		if (tfw_http_resp_cache(hmresp))
			return TFW_BLOCK;
next_resp:
		if (hmsib) {
			/*
			 * Switch the connection to the sibling message.
			 * Data processing will continue with the new SKB.
			 */
			data_off = 0;
			skb_len = skb->len;
			conn->msg = (TfwMsg *)hmsib;
		}
	}

	return r;
}

/**
 * @return status (application logic decision) of the message processing.
 */
int
tfw_http_msg_process(void *conn, struct sk_buff *skb, unsigned int off)
{
	TfwConnection *c = (TfwConnection *)conn;

	return (TFW_CONN_TYPE(c) & Conn_Clnt)
		? tfw_http_req_process(c, skb, off)
		: tfw_http_resp_process(c, skb, off);
}

/**
 * Calculate the key of an HTTP request by hashing URI and Host header values.
 */
unsigned long
tfw_http_req_key_calc(TfwHttpReq *req)
{
	TfwStr host;

	if (req->hash)
		return req->hash;

	req->hash = tfw_hash_str(&req->uri_path) ^ req->method;

	tfw_http_msg_clnthdr_val(&req->h_tbl->tbl[TFW_HTTP_HDR_HOST],
				 TFW_HTTP_HDR_HOST, &host);
	if (!TFW_STR_EMPTY(&host))
		req->hash ^= tfw_hash_str(&host);

	return req->hash;
}
EXPORT_SYMBOL(tfw_http_req_key_calc);

static TfwConnHooks http_conn_hooks = {
	.conn_init	= tfw_http_conn_init,
	.conn_drop	= tfw_http_conn_drop,
	.conn_release	= tfw_http_conn_release,
	.conn_msg_alloc	= tfw_http_conn_msg_alloc,
};

int __init
tfw_http_init(void)
{
	int r = tfw_gfsm_register_fsm(TFW_FSM_HTTP, tfw_http_msg_process);
	if (r)
		return r;

	tfw_connection_hooks_register(&http_conn_hooks, TFW_FSM_HTTP);

	/* Must be last call - we can't unregister the hook. */
	ghprio = tfw_gfsm_register_hook(TFW_FSM_HTTPS,
					TFW_GFSM_HOOK_PRIORITY_ANY,
					TFW_HTTPS_FSM_TODO_ISSUE_81,
					TFW_FSM_HTTP, TFW_HTTP_FSM_INIT);
	if (ghprio < 0)
		return ghprio;

	return 0;
}

void
tfw_http_exit(void)
{
	tfw_gfsm_unregister_hook(TFW_FSM_HTTPS, ghprio,
				 TFW_HTTPS_FSM_TODO_ISSUE_81);
	tfw_connection_hooks_unregister(TFW_FSM_HTTP);
	tfw_gfsm_unregister_fsm(TFW_FSM_HTTP);
}
