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

struct http_server_mod_s
{
	void *arg;
	const char *name;
	http_getctx_t func;
	http_freectx_t freectx;
	http_server_mod_t *next;
};

struct http_server_s
{
	int sock;
	int type;
	int run;
	vthread_t thread;
	http_client_t *clients;
	http_connector_list_t *callbacks;
	http_server_config_t *config;
	http_server_mod_t *mod;
	const httpserver_ops_t *ops;
	const httpclient_ops_t *protocol_ops;
	void *protocol;
	http_message_method_t *methods;
	buffer_t *methods_storage;
#ifdef USE_POLL
	struct pollfd *poll_set;
#else
	fd_set fds[3];
#endif
	int numfds;
	http_server_t *next;
};

struct http_server_session_s
{
	dbentry_t *dbfirst;
	buffer_t *storage;
};

http_server_session_t *_httpserver_createsession(const http_server_t *server, const http_client_t *client);

#endif

