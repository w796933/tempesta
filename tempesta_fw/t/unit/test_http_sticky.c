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

/* prevent exporting symbols */
#include <linux/module.h>
#undef EXPORT_SYMBOL
#define EXPORT_SYMBOL(...)

#ifdef __read_mostly
#undef __read_mostly
#define __read_mostly
#endif

#ifdef __init
#undef __init
#define __init
#endif

#include "http_msg.c"

#include "http_sticky.c"

#include "filter.c"
#include "sock.c"
#include "server.c"
#include "sock_srv.c"
#include "client.c"
#include "classifier.c"
#include "vhost.c"

/* rename original tfw_cli_conn_send(), a custom version will be used here */
#define tfw_cli_conn_send	divert_tfw_cli_conn_send
#include "sock_clnt.c"
#undef tfw_cli_conn_send

#include "hash.c"
#include "http.c"
#include "addr.c"
#include "ss_skb.c"
#include "sched.c"
#include "gfsm.c"
#include "cache.c"
#include "http_parser.c"
#include "str.c"
#include "work_queue.c"
#include "procfs.c"

/* rename original tfw_connection_send(), a custom version will be used here */
#define tfw_connection_send	divert_tfw_connection_send
#include "connection.c"
#undef tfw_connection_send

#include "test.h"
#include "helpers.h"
#include "tfw_str_helper.h"

#define COOKIE_NAME	"QWERTY_123"

static struct {
	int		tfw_connection_send_was_called;
	int		seen_set_cookie_header;
	int		seen_cookie;
	unsigned int	http_status;

	TfwHttpMsg	*hmreq;
	TfwHttpMsg	*hmresp;
	TfwConnection   connection;
	TfwClient	client;
	struct sock	sock;
} mock;

/*
 * Find a specific non-special header field in an HTTP message.
 *
 * This function assumes that the header field name is stored
 * in TfwStr{} after an HTTP message is parsed.
 */
static TfwStr *
tfw_http_field_raw(TfwHttpMsg *hm, const char *field_name, size_t len)
{
	int i;

	for (i = TFW_HTTP_HDR_RAW; i < hm->h_tbl->off; i++) {
		TfwStr *hdr_field = &hm->h_tbl->tbl[i];
		if (tfw_str_eq_cstr(hdr_field, field_name, len,
				    TFW_STR_EQ_PREFIX | TFW_STR_EQ_CASEI))
			return hdr_field;
	}

	return NULL;
}

static int
tfw_http_field_value(TfwHttpMsg *hm, const TfwStr *field_name, TfwStr *value)
{
	char *buf, *ptr;
	size_t len;
	TfwStr *hdr_field;

	hdr_field = tfw_http_field_raw(hm, field_name->data, field_name->len);
	if (hdr_field == NULL) {
		return 0;
	}
	/*
	 * XXX Linearize TfwStr{}. Should be eliminated
	 * when better TfwStr{} functions are implemented.
	 */
	len = hdr_field->len + 1;
	if ((buf = tfw_pool_alloc(hm->pool, len)) == NULL) {
		return -ENOMEM;
	}
	len = tfw_str_to_cstr(hdr_field, buf, len);
	ptr = strim(buf + field_name->len);
	value->data = ptr;
	value->len = len - (ptr - buf);

	return 1;
}

/* custom version for testing purposes */
int
tfw_connection_send(TfwConnection *conn, TfwMsg *msg)
{
	struct sk_buff *skb;
	unsigned int data_off = 0;
	const DEFINE_TFW_STR(s_set_cookie, "Set-Cookie:");
	DEFINE_TFW_STR(hdr_value, NULL);

	BUG_ON(!msg);
	BUG_ON(!conn);

	mock.tfw_connection_send_was_called += 1;

	skb = ss_skb_peek(&msg->skb_list);
	while (skb) {
		int ret;
		ret = ss_skb_process(skb, &data_off, tfw_http_parse_resp,
				     mock.hmresp);
		skb = ss_skb_next(skb);
	}

	mock.http_status = ((TfwHttpResp *)mock.hmresp)->status;

	mock.seen_set_cookie_header =
	    tfw_http_field_value(mock.hmresp, &s_set_cookie, &hdr_value) > 0;

	if (!mock.seen_set_cookie_header)
		return 0;

	/* XXX assuming string is linear */
	BUG_ON(!TFW_STR_PLAIN(&hdr_value));

	/* cookie name should be somewhere in Set-Cookie header value */
	mock.seen_cookie =
	    strnstr(hdr_value.data, COOKIE_NAME, hdr_value.len) != NULL;

	return 0;
}

/* custom version for testing purposes */
int tfw_cli_conn_send(TfwConnection *conn, TfwMsg *msg)
{
	return tfw_connection_send(conn, msg);
}

/* setup/teardown helpers */

static void
http_sticky_suite_setup(void)
{
	struct sk_buff *skb;

	BUG_ON(mock.hmreq);
	BUG_ON(mock.hmresp);

	memset(&mock, 0, sizeof(mock));

	mock.hmreq = tfw_http_msg_alloc(Conn_Clnt);
	mock.hmresp = tfw_http_msg_alloc(Conn_Srv);

	BUG_ON(!mock.hmreq);
	BUG_ON(!mock.hmresp);

	skb = alloc_skb(PAGE_SIZE, GFP_ATOMIC);
	BUG_ON(!skb);
	skb_reserve(skb, MAX_TCP_HEADER);
	ss_skb_queue_tail(&mock.hmreq->msg.skb_list, skb);

	skb = alloc_skb(PAGE_SIZE, GFP_ATOMIC);
	BUG_ON(!skb);
	skb_reserve(skb, MAX_TCP_HEADER);
	ss_skb_queue_tail(&mock.hmresp->msg.skb_list, skb);

	mock.hmreq->conn = &mock.connection;
	mock.hmresp->conn = &mock.connection;
	mock.connection.peer = (TfwPeer *)&mock.client;
	mock.connection.sk = &mock.sock;
	mock.sock.sk_family = AF_INET;
}

static void
http_sticky_suite_teardown(void)
{
	tfw_http_msg_free(mock.hmreq);
	tfw_http_msg_free(mock.hmresp);

	memset(&mock, 0, sizeof(mock));
}

TEST(http_sticky, sending_302_without_preparing)
{
	/* should fail -- cookie wasn't set */
	EXPECT_EQ(tfw_http_sticky_send_302(mock.hmreq), -1);

	EXPECT_FALSE(mock.tfw_connection_send_was_called);
}

TEST(http_sticky, sending_302)
{
	create_str_pool();

	{
		/* Need host header and
		 *it must be compound as special header
		 */
//		TFW_STR2(hdr1, "Host: ", "localhost");
		char *s_req = "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";

//		mock.hmreq->h_tbl->tbl[TFW_HTTP_HDR_HOST] = *hdr1;
		tfw_http_parse_req(mock.hmreq, s_req, strlen(s_req));

		EXPECT_EQ(tfw_http_sticky_set(mock.hmreq), 0);
		EXPECT_EQ(tfw_http_sticky_send_302(mock.hmreq), 0);

		EXPECT_TRUE(mock.tfw_connection_send_was_called);
		EXPECT_TRUE(mock.seen_set_cookie_header);
		EXPECT_TRUE(mock.seen_cookie);
		EXPECT_EQ(mock.http_status, 302);
	}

	free_all_str();
}

TEST(http_sticky, sending_502)
{
	EXPECT_EQ(tfw_http_sticky_set(mock.hmreq), 0);
	EXPECT_EQ(tfw_http_send_502(mock.hmreq), 0);

	/* HTTP 502 response have no Set-Cookie header */
	EXPECT_TRUE(mock.tfw_connection_send_was_called);
	EXPECT_FALSE(mock.seen_set_cookie_header);
	EXPECT_FALSE(mock.seen_cookie);
	EXPECT_EQ(mock.http_status, 502);
}

static void
append_string_to_msg(TfwHttpMsg *hm, const char *s)
{
	struct sk_buff  *skb;
	void		*ptr;
	size_t		len;

	BUG_ON(!s);
	len = strlen(s);

	skb = hm->msg.skb_list.first;
	BUG_ON(!skb);

	ptr = skb_put(skb, len);
	BUG_ON(!ptr);
	memcpy(ptr, s, len);
}

static int
http_parse_helper(TfwHttpMsg *hm, ss_skb_actor_t actor)
{
	struct sk_buff *skb;
	unsigned int off;

	skb = hm->msg.skb_list.first;
	BUG_ON(!skb);
	off = 0;
	while (1) {
		switch (ss_skb_process(skb, &off, actor, hm)) {
		case TFW_POSTPONE:
			if (skb == hm->msg.skb_list.last)
				return -1;
			skb = skb->next;
			continue;

		case TFW_PASS:
			/* sucessfully parsed */
			return 0;

		default:
			return -1;
		}
	}
}

static int
http_parse_req_helper(void)
{
	return http_parse_helper(mock.hmreq, tfw_http_parse_req);
}

static int
http_parse_resp_helper(void)
{
	/* XXX reset parser explicitly to be able to call it multiple times */
	memset(&mock.hmresp->parser, 0, sizeof(mock.hmresp->parser));
	mock.hmresp->h_tbl->off = TFW_HTTP_HDR_RAW;
	memset(mock.hmresp->h_tbl->tbl, 0, __HHTBL_SZ(1) * sizeof(TfwStr));

	return http_parse_helper(mock.hmresp, tfw_http_parse_resp);
}

TEST(http_sticky, sticky_get_absent)
{
	TfwStr value = {};
	const char *s_req = "GET / HTTP/1.0\r\nHost: localhost\r\n"
			    "Cookie: __utmz=12345; q=aa\r\n\r\n";

	append_string_to_msg(mock.hmreq, s_req);
	EXPECT_EQ(http_parse_req_helper(), 0);

	/* expecting no cookie */
	EXPECT_EQ(tfw_http_sticky_get(mock.hmreq, &value), 0);
}

static void
test_sticky_present_helper(const char *s_req)
{
	TfwStr	value = {};

	append_string_to_msg(mock.hmreq, s_req);
	EXPECT_EQ(http_parse_req_helper(), 0);

	EXPECT_EQ(tfw_http_sticky_get(mock.hmreq, &value), 1);

	EXPECT_TRUE(value.len == 5);
	EXPECT_TRUE(value.data && memcmp(value.data, "67890", 5) == 0);
}

TEST(http_sticky, sticky_get_present_begin)
{
	const char *s_req = "GET / HTTP/1.0\r\nContent-Length: 0\r\n"
			    "Cookie: " COOKIE_NAME "=67890; __utmz=12345; "
			    "q=aa\r\n\r\n";

	test_sticky_present_helper(s_req);
}

TEST(http_sticky, sticky_get_present_middle)
{
	const char *s_req = "GET / HTTP/1.0\r\nContent-Length: 0\r\n"
			    "Cookie: __utmz=12345; " COOKIE_NAME "=67890; "
			    "q=aa\r\n\r\n";

	test_sticky_present_helper(s_req);
}

TEST(http_sticky, sticky_get_present_end)
{
	const char *s_req = "GET / HTTP/1.0\r\nContent-Length: 0\r\n"
			    "Cookie: __utmz=12345; q=aa; "
			    COOKIE_NAME "=67890\r\n\r\n";

	test_sticky_present_helper(s_req);
}

/* request have no sticky cookie */
TEST(http_sticky, req_no_cookie)
{
	const char *s_req = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
	const char *s_resp = "HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n";

	append_string_to_msg(mock.hmreq, s_req);
	append_string_to_msg(mock.hmresp, s_resp);
	EXPECT_EQ(http_parse_req_helper(), 0);
	EXPECT_EQ(http_parse_resp_helper(), 0);

	EXPECT_EQ(tfw_http_sticky_req_process(mock.hmreq), 0);
	EXPECT_EQ(tfw_http_sticky_resp_process(mock.hmresp, mock.hmreq), 0);

	/* with no cookie enforcement, only backend response will be modified */
	EXPECT_FALSE(mock.tfw_connection_send_was_called);

	/* since response was modified, we need to parse it again */
	EXPECT_EQ(http_parse_resp_helper(), 0);
	tfw_connection_send(&mock.connection, &mock.hmresp->msg);

	EXPECT_TRUE(mock.tfw_connection_send_was_called);
	EXPECT_TRUE(mock.seen_set_cookie_header);
	EXPECT_TRUE(mock.seen_cookie);
}

/* request have sticky cookie */
TEST(http_sticky, req_have_cookie)
{
	const char *s_req = "GET / HTTP/1.0\r\nHost: localhost\r\n"
			    "Cookie: " COOKIE_NAME "=something\r\n\r\n";
	const char *s_resp = "HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n";

	append_string_to_msg(mock.hmreq, s_req);
	append_string_to_msg(mock.hmresp, s_resp);
	EXPECT_EQ(http_parse_req_helper(), 0);
	EXPECT_EQ(http_parse_resp_helper(), 0);

	EXPECT_EQ(tfw_http_sticky_req_process(mock.hmreq), 0);
	EXPECT_EQ(tfw_http_sticky_resp_process(mock.hmresp, mock.hmreq), 0);

	/* expecting no immediate responses */
	EXPECT_FALSE(mock.tfw_connection_send_was_called);

	/* since response could be modified, we need to parse it again */
	EXPECT_EQ(http_parse_resp_helper(), 0);
	tfw_connection_send(&mock.connection, &mock.hmresp->msg);

	/* no Set-Cookie headers are expected */
	EXPECT_FALSE(mock.seen_set_cookie_header);
	EXPECT_FALSE(mock.seen_cookie);
}

/* request have no sticky cookie; enforce mode activated */
TEST(http_sticky, req_no_cookie_enforce)
{
	const char *s_req = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";

	append_string_to_msg(mock.hmreq, s_req);
	EXPECT_EQ(http_parse_req_helper(), 0);
	EXPECT_EQ(tfw_http_sticky_req_process(mock.hmreq), 1);

	/* in enforce mode, 302 response is sent to a client by Tempesta
	 * before backend gets anything
	 */
	EXPECT_TRUE(mock.tfw_connection_send_was_called);
	EXPECT_TRUE(mock.seen_set_cookie_header);
	EXPECT_TRUE(mock.seen_cookie);
}

/* request have sticky cookie set; enforce mode activated */
TEST(http_sticky, req_have_cookie_enforce)
{
	const char *s_req = "GET / HTTP/1.0\r\nHost: localhost\r\n"
			    "Cookie: " COOKIE_NAME "=something\r\n\r\n";
	const char *s_resp = "HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n";

	append_string_to_msg(mock.hmreq, s_req);
	append_string_to_msg(mock.hmresp, s_resp);
	EXPECT_EQ(http_parse_req_helper(), 0);
	EXPECT_EQ(http_parse_resp_helper(), 0);

	EXPECT_EQ(tfw_http_sticky_req_process(mock.hmreq), 0);
	EXPECT_EQ(tfw_http_sticky_resp_process(mock.hmresp, mock.hmreq), 0);

	/* expecting no immediate responses */
	EXPECT_FALSE(mock.tfw_connection_send_was_called);

	/* since response could be modified, we need to parse it again */
	EXPECT_EQ(http_parse_resp_helper(), 0);
	tfw_connection_send(&mock.connection, &mock.hmresp->msg);

	/* no Set-Cookie headers are expected */
	EXPECT_FALSE(mock.seen_set_cookie_header);
	EXPECT_FALSE(mock.seen_cookie);
}

TEST_SUITE(http_sticky)
{
	TfwCfgEntry ce = {
		.name = "sticky",
		.val_n = 1,
		.vals = { "enforce" },
		.attr_n = 1,
		.attrs = { { .key = "name", .val = COOKIE_NAME } },
		.have_children = false
	};

	TEST_SETUP(http_sticky_suite_setup);
	TEST_TEARDOWN(http_sticky_suite_teardown);

	tfw_http_sticky_init();

	/* emulate configuration file */
	ce.val_n = 0; /* remove "enforce" parameter */
	tfw_http_sticky_cfg(&tfw_http_sticky_cfg_mod.specs[0], &ce);

	tfw_cfg_sticky_start();

	TEST_RUN(http_sticky, sending_302_without_preparing);
	TEST_RUN(http_sticky, sending_302);
	TEST_RUN(http_sticky, sending_502);
	TEST_RUN(http_sticky, sticky_get_absent);
	TEST_RUN(http_sticky, sticky_get_present_begin);
	TEST_RUN(http_sticky, sticky_get_present_middle);
	TEST_RUN(http_sticky, sticky_get_present_end);
	TEST_RUN(http_sticky, req_no_cookie);
	TEST_RUN(http_sticky, req_have_cookie);

	/* test "enforce" mode */
	ce.val_n = 1; /* return "enforce" parameter */
	tfw_http_sticky_cfg(&tfw_http_sticky_cfg_mod.specs[0], &ce);

	TEST_RUN(http_sticky, req_no_cookie_enforce);
	TEST_RUN(http_sticky, req_have_cookie_enforce);

	tfw_cfg_sticky_stop();

	tfw_http_sticky_exit();
}
