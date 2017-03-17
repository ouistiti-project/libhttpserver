/*****************************************************************************
 * _httpserver.h: Simple HTTP server private data
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

#ifndef ___HTTPSERVER_H__
#define ___HTTPSERVER_H__

#ifndef WIN32
# include <sys/un.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <netdb.h>
#else
# include <winsock2.h>
#endif

#include "vthread.h"
#include "dbentry.h"

typedef struct buffer_s buffer_t;
typedef struct http_connector_list_s http_connector_list_t;
typedef struct http_client_modctx_s http_client_modctx_t;

typedef void (*http_flush_t)(void *ctl);
typedef void (*http_close_t)(void *ctl);
#define CLIENT_STARTED 0x0100
#define CLIENT_RUNNING 0x0200
#define CLIENT_STOPPED 0x0400
#define CLIENT_NONBLOCK 0x1000
#define CLIENT_ERROR 0x2000
#define CLIENT_RESPONSEREADY 0x4000
#define CLIENT_KEEPALIVE 0x8000
#define CLIENT_MACHINEMASK 0x00FF
#define CLIENT_NEW 0x0000
#define CLIENT_REQUEST 0x0001
#define CLIENT_PUSHREQUEST 0x0002
#define CLIENT_PARSER1 0x0003
#define CLIENT_PARSER2 0x0004
#define CLIENT_RESPONSEHEADER 0x0005
#define CLIENT_RESPONSECONTENT 0x0006
#define CLIENT_PARSERERROR 0x0007
#define CLIENT_COMPLETE 0x0008
struct http_client_s
{
	int sock;
	int state;
	http_server_t *server; /* the server which create the client */
	vthread_t thread; /* The thread of socket management during the live of the connection */

	http_recv_t recvreq; /* callback to receive data on the socket */
	http_send_t sendresp; /* callback to send data on the socket */
	http_flush_t flush; /* callback to flush the socket */
	http_close_t close; /* callback to close the socket */
	void *ctx; /* ctx of recvreq and sendresp functions */

	http_connector_list_t *callbacks;
	http_connector_list_t *callback;
	http_message_t *request;
	http_message_t *request_queue;

	http_client_modctx_t *modctx; /* list of pointers returned by getctx of each mod */

	dbentry_t *session;
	buffer_t *session_storage;
#ifndef WIN32
	struct sockaddr_un addr;
#elif defined IPV6
	struct sockaddr_in6 addr;
#else
	struct sockaddr_in addr;
#endif
	unsigned int addr_size;
	struct http_client_s *next;
};
typedef struct http_client_s http_client_t;
http_client_t *httpclient_create(http_server_t *server);

typedef int (*_httpserver_start_t)(http_server_t *server);
typedef http_client_t *(*_httpserver_createclient_t)(http_server_t *server);
typedef void (*_httpserver_close_t)(http_server_t *server);
typedef struct httpserver_ops_s httpserver_ops_t;

struct httpserver_ops_s
{
		_httpserver_start_t start;
		_httpserver_createclient_t createclient;
		_httpserver_close_t close;
};

typedef struct http_server_mod_s http_server_mod_t;

struct http_server_s
{
	int sock;
	int run;
	vthread_t thread;
	http_client_t *clients;
	http_connector_list_t *callbacks;
	http_server_config_t *config;
	http_server_mod_t *mod;
	httpserver_ops_t *ops;
};

#endif

