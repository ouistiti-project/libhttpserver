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
typedef struct http_message_method_s http_message_method_t;
typedef struct http_server_session_s http_server_session_t;

typedef int (*http_connect_t)(void *ctl, char *addr, int port);
typedef int (*http_status_t)(void *ctl);
typedef void (*http_flush_t)(void *ctl);
typedef void (*http_disconnect_t)(void *ctl);
typedef void (*http_destroy_t)(void *ctl);
typedef struct httpclient_ops_s httpclient_ops_t;

struct httpclient_ops_s
{
	http_connect_t connect; /* callback to connect on an external server */
	http_recv_t recvreq; /* callback to receive data on the socket */
	http_send_t sendresp; /* callback to send data on the socket */
	http_status_t status; /* callback to get the socket status*/
	http_flush_t flush; /* callback to flush the socket */
	http_disconnect_t disconnect; /* callback to close the socket */
	http_destroy_t destroy; /* callback to close the socket */
};
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
struct http_client_s
{
	int sock;
	int state;
	http_server_t *server; /* the server which create the client */
	vthread_t thread; /* The thread of socket management during the live of the connection */

	httpclient_ops_t ops;
	void *ctx; /* ctx of recvreq and sendresp functions */

	http_connector_list_t *callbacks;
	http_message_t *request;
	http_message_t *request_queue;

	http_client_modctx_t *modctx; /* list of pointers returned by getctx of each mod */

	buffer_t *sockdata;

	http_server_session_t *session;
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
int httpclient_socket(http_client_t *client);

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
	http_message_method_t *methods;
	fd_set fds[3];
	int numfds;
};

typedef enum
{
	MESSAGE_TYPE_GET,
	MESSAGE_TYPE_POST,
	MESSAGE_TYPE_HEAD,
} _http_message_method_e;

static void _httpserver_addmethod(http_server_t *server, const char *key, _http_message_method_e id);

#endif

