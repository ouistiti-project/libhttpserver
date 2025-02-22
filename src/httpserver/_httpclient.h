/*****************************************************************************
 * _httpclient.h: Simple HTTP server private data
 *****************************************************************************
 * Copyright (C) 2016-2017
 *
 * Authors: Marc Chalain <marc.chalain@gmail.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#ifndef ___HTTPCLIENT_H__
#define ___HTTPCLIENT_H__

#define CLIENT_STARTED 0x0100
#define CLIENT_RUNNING 0x0200
#define CLIENT_STOPPED 0x0400
#define CLIENT_LOCKED  0x0800
#define CLIENT_NONBLOCK 0x1000
#define CLIENT_ERROR 0x2000
#define CLIENT_RESPONSEREADY 0x4000
#define CLIENT_KEEPALIVE 0x8000
#define CLIENT_MACHINEMASK 0x000F
#define CLIENT_NEW 0x0000
#define CLIENT_READING 0x0001
#define CLIENT_WAITING 0x0002
#define CLIENT_SENDING 0x0003
#define CLIENT_EXIT 0x0009
#define CLIENT_DEAD 0x000A

#define WAIT_TIMER 2 //seconds

struct http_client_modctx_s
{
	void *ctx;
	const char *name;
	http_freectx_t freectx;
	http_client_modctx_t *next;
};
typedef struct http_client_modctx_s http_client_modctx_t;

struct http_client_s
{
	int sock;
	int state;
	int timeout;
	http_server_t *server; /* the server which create the client */
	vthread_t thread; /* The thread of socket management during the live of the connection */

	const httpclient_ops_t *ops;
	void *opsctx; /* ctx of ops functions */
	string_t scheme;

	http_send_t client_send;
	void *send_arg;
	http_recv_t client_recv;
	void *recv_arg;

	http_connector_list_t *callbacks;
	http_message_t *request;
	http_message_t *request_queue;

	http_client_modctx_t *modctx; /* list of pointers returned by getctx of each mod */

	buffer_t *sockdata;
#ifdef HTTPCLIENT_DUMPSOCKET
	int dumpfd;
#endif

	http_server_session_t *session;
	struct sockaddr_storage addr;
	unsigned int addr_size;
	struct http_client_s *next;
};
typedef struct http_client_s http_client_t;

int httpclient_socket(http_client_t *client);
int _httpclient_run(http_client_t *client);
int httpclient_state(http_client_t *client, int newstate);
#ifdef HTTPCLIENT_FEATURES
void httpclient_appendops(const httpclient_ops_t *ops);
const httpclient_ops_t *httpclient_ops();
int _httpclient_connect(http_client_t *client, const char *addr, int port);
#endif
int httpclient_addmodule(http_client_t *client, http_server_mod_t *mod);
void httpclient_freemodules(http_client_t *client);
void httpclient_freeconnectors(http_client_t *client);

#endif
