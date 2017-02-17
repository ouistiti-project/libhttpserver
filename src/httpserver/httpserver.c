/*****************************************************************************
 * httpserver.c: Simple HTTP server
 *****************************************************************************
 * Copyright (C) 2016-2017
 *
 * Authors: Marc Chalain <marc.chalain@gmail.com>
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

#if defined(__GNUC__) && !defined(_GNU_SOURCE)
# define _GNU_SOURCE
#else
# define strcasestr strstr
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#ifndef WIN32
# include <sys/socket.h>
# include <sys/ioctl.h>
# include <sys/un.h>
# include <net/if.h>
# include <netinet/in.h>
# include <netinet/tcp.h>
# include <arpa/inet.h>
# include <netdb.h>

#else

# include <winsock2.h>
# include <ws2tcpip.h>

# define SHUT_RDWR SD_BOTH
# pragma comment (lib, "Ws2_32.lib")

#ifdef __cplusplus
extern "C" {
#endif
   void WSAAPI freeaddrinfo( struct addrinfo* );

   int WSAAPI getaddrinfo( const char*, const char*, const struct addrinfo*,
                 struct addrinfo** );

   int WSAAPI getnameinfo( const struct sockaddr*, socklen_t, char*, DWORD,
                char*, DWORD, int );
#ifdef __cplusplus
}
#endif

#endif

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
# define dbg(...)
#endif

#include "valloc.h"
#include "vthread.h"
#include "dbentry.h"
#include "httpserver.h"

typedef struct buffer_s
{
	char *data;
	char *offset;
	int size;
	int length;
} buffer_t;

struct http_connector_list_s
{
	char *vhost;
	http_connector_t func;
	void *arg;
	struct http_connector_list_s *next;
};
typedef struct http_connector_list_s http_connector_list_t;

typedef struct http_server_mod_s http_server_mod_t;
struct http_server_mod_s
{
	void *arg;
	http_getctx_t func;
	http_freectx_t freectx;
	http_server_mod_t *next;
};

struct http_message_queue_s
{
	http_message_t *message;
	struct http_message_queue_s *next;
};
typedef struct http_message_queue_s http_message_queue_t;

typedef struct http_client_modctx_s http_client_modctx_t;
struct http_client_modctx_s
{
	void *ctx;
	http_client_modctx_t *next;
};

#define CLIENT_STARTED 0x0100
#define CLIENT_RUNNING 0x0200
#define CLIENT_STOPPED 0x0400
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
	void *ctx; /* ctx of recvreq and sendresp functions */

	http_connector_list_t *callbacks;
	http_connector_list_t *callback;
	http_message_t *request;
	http_message_queue_t *request_queue;

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

struct http_message_s
{
	http_message_result_e result;
	int keepalive;
	http_client_t *client;
	http_message_t *response;
	http_connector_list_t *connector;
	enum
	{
		MESSAGE_TYPE_GET,
		MESSAGE_TYPE_POST,
		MESSAGE_TYPE_HEAD,
		MESSAGE_TYPE_PUT,
		MESSAGE_TYPE_DELETE,
	} type;
	enum
	{
		PARSE_INIT,
		PARSE_URI,
		PARSE_VERSION,
		PARSE_HEADER,
		PARSE_HEADERNEXT,
		PARSE_CONTENT,
		PARSE_END,
		PARSE_MASK = 0x00FF,
		PARSE_CONTINUE = 0x0100,
	} state;
	buffer_t *content;
	int content_length;
	buffer_t *uri;
	char *query;
	http_message_version_e version;
	buffer_t *headers_storage;
	dbentry_t *headers;
	void *private;
};

struct http_server_s
{
	int sock;
	int run;
	vthread_t thread;
	http_client_t *clients;
	http_connector_list_t *callbacks;
	http_server_config_t *config;
	http_server_mod_t *mod;
};

static int _httpserver_start(http_server_t *server);
static void _httpmessage_fillheaderdb(http_message_t *message);
static void _httpmessage_addheader(http_message_t *message, char *key, char *value);
static int _httpclient_run(http_client_t *client);

/********************************************************************/
static http_server_config_t defaultconfig = {
	.addr = NULL,
	.port = 80,
	.maxclients = 10,
	.chunksize = 64,
	.keepalive = 1,
	.version = HTTP10,
};

static const char *_http_message_result[] =
{
	" 200 OK",
	" 400 Bad Request",
	" 404 File Not Found",
	" 405 Method Not Allowed",
#ifndef HTTP_STATUS_PARTIAL
	" 301 Moved Permanently",
	" 302 Found",
	" 304 Not Modified",
	" 401 Unauthorized",
	" 414 Request URI too long",
	" 505 HTTP Version Not Supported",
	" 511 Network Authentication Required",
#endif
};

static char *_http_message_version[] =
{
	"HTTP/0.9",
	"HTTP/1.0",
	"HTTP/1.1",
	"HTTP/2",
};
char httpserver_software[] = "libhttpserver";
static const char str_connection[] = "Connection";
static const char str_contenttype[] = "Content-Type";
static const char str_contentlength[] = "Content-Length";
/********************************************************************/
#define CHUNKSIZE 64
#define BUFFERMAX 2048
static buffer_t * _buffer_create()
{
	buffer_t *buffer = vcalloc(1, sizeof(*buffer));
	buffer->data = vcalloc(1, CHUNKSIZE);
	buffer->size = CHUNKSIZE;
	buffer->offset = buffer->data;
	return buffer;
}

static char *_buffer_append(buffer_t *buffer, char *data, int length)
{
	if (buffer->data + buffer->size <= buffer->offset + length + 1)
	{
		char *data = buffer->data;
		int chunksize = CHUNKSIZE * (length/CHUNKSIZE +1);

		data = vrealloc(buffer->data, buffer->size + chunksize);
		if ((data == NULL && errno == ENOMEM) || (buffer->size + chunksize) > BUFFERMAX)
		{
			warn("buffer max: %d / %d", buffer->size + chunksize, BUFFERMAX);
			return NULL;
		}
		buffer->size += chunksize;
		if (data != buffer->data)
		{
			char *offset = buffer->offset;
			buffer->offset = data + (offset - buffer->data);
			buffer->data = data;
		}
	}
	char *offset = buffer->offset;
	memcpy(buffer->offset, data, length);
	buffer->length += length;
	buffer->offset += length;
	*buffer->offset = '\0';
	return offset;
}

static void _buffer_reset(buffer_t *buffer)
{
	buffer->offset = buffer->data;
	buffer->length = 0;
}

static void _buffer_destroy(buffer_t *buffer)
{
	vfree(buffer->data);
	vfree(buffer);
}

static http_message_t * _httpmessage_create(http_client_t *client, http_message_t *parent)
{
	http_message_t *message;

	message = vcalloc(1, sizeof(*message));
	if (message)
	{
		message->client = client;
		if (parent)
		{
			parent->response = message;

			message->type = parent->type;
			message->client = parent->client;
			message->version = parent->version;
		}
	}
	return message;
}

static void _httpmessage_reset(http_message_t *message)
{
	if (message->uri)
		_buffer_reset(message->uri);
	if (message->content)
		_buffer_reset(message->content);
	if (message->headers_storage)
		_buffer_reset(message->headers_storage);
}

static void _httpmessage_destroy(http_message_t *message)
{
	if (message->response)
		_httpmessage_destroy(message->response);
	if (message->uri)
		_buffer_destroy(message->uri);
	if (message->content)
		_buffer_destroy(message->content);
	if (message->headers_storage)
		_buffer_destroy(message->headers_storage);
	vfree(message);
}

static int _httpmessage_parserequest(http_message_t *message, buffer_t *data)
{
	int ret = ECONTINUE;

	do
	{
		int next = message->state  & PARSE_MASK;
		switch (next)
		{
			case PARSE_INIT:
			{
				if (!strncasecmp(data->offset,"GET ",4))
				{
					message->type = MESSAGE_TYPE_GET;
					data->offset += 4;
					next = PARSE_URI;
				}
				else if (!strncasecmp(data->offset,"POST ",5))
				{
					message->type = MESSAGE_TYPE_POST;
					data->offset += 5;
					next = PARSE_URI;
				}
				else if (!strncasecmp(data->offset,"HEAD ",5))
				{
					message->type = MESSAGE_TYPE_HEAD;
					data->offset += 5;
					next = PARSE_URI;
				}
#ifndef HTTP_METHOD_PARTIAL
				else if (!strncasecmp(data->offset,"PUT ",4))
				{
					message->type = MESSAGE_TYPE_PUT;
					data->offset += 4;
					next = PARSE_URI;
				}
				else if (!strncasecmp(data->offset,"DELETE ",7))
				{
					message->type = MESSAGE_TYPE_DELETE;
					data->offset += 7;
					next = PARSE_URI;
				}
#endif
				else
				{
					data->offset++;
					message->result = RESULT_405;
					ret = EREJECT;
				}
			}
			break;
			case PARSE_URI:
			{
				char *uri = data->offset;
				int length = 0;
				if (message->uri == NULL)
					message->uri = _buffer_create();
				while (data->offset < (data->data + data->size) && next == PARSE_URI)
				{
					switch (*data->offset)
					{
						case ' ':
						{
							*data->offset = '\0';
							uri = _buffer_append(message->uri, uri, length + 1);
							message->query = uri;
							next = PARSE_VERSION;
						}
						break;
						case '\r':
						{
							next = PARSE_HEADER;
							*data->offset = '\0';
							if (*(data->offset + 1) == '\n')
								data->offset++;
							uri = _buffer_append(message->uri, uri, length + 1);
							message->query = uri;
						}
						break;
						case '\n':
						{
							next = PARSE_HEADER;
							*data->offset = '\0';
						}
						break;
						default:
						{
							length++;
						}
					}
					data->offset++;
				}
				
				if (next == PARSE_URI)
				{
					uri = _buffer_append(message->uri, uri, length);
				}
				if (uri == NULL)
				{
					_buffer_destroy(message->uri);
					message->uri = _buffer_create();
					message->result = RESULT_414;
					ret = EREJECT;
				}
				if (next != PARSE_URI)
				{
					int i;
					for (i = 0; i < message->uri->length; i++)
					{
						if (message->uri->data[i] == '?')
						{
							message->query = message->uri->data + i + 1;
							break;
						}
					}
					dbg("new request for %s", message->uri->data);
				}
			}
			break;
			case PARSE_VERSION:
			{
				int i;
				for (i = HTTP09; i < HTTPVERSIONS; i++)
				{
					int length = strlen(_http_message_version[i]);
					if (!strncasecmp(data->offset,_http_message_version[i],length))
					{
						message->version = i;
						data->offset += length;
						break;
					}
				}
				while (data->offset < (data->data + data->size) && next == PARSE_VERSION)
				{
					switch (*data->offset)
					{
						case '\r':
						{
							next = PARSE_HEADER;
							if (data->offset[1] == '\n')
							{
								data->offset++;
							}
						}
						break;
						case '\n':
						{
							next = PARSE_HEADER;
							if (data->offset[1] == '\r')
							{
								data->offset++;
							}
						}
						break;
						default:
						{
						}
					}
					data->offset++;
				}
			}
			break;
			case PARSE_HEADER:
			{
				char *header = data->offset;
				int length = 0;
				if (message->headers_storage == NULL)
					message->headers_storage = _buffer_create();
				/* store header line as "<key>:<value>\0" */
				while (data->offset < (data->data + data->size) && next == PARSE_HEADER)
				{
					if (*data->offset == '\n')
					{
						*data->offset = '\0';
						if (length == 0 && !(message->state & PARSE_CONTINUE))
						{
							next = PARSE_HEADERNEXT;
						}
						else
						{
							header[length] = 0;
							_buffer_append(message->headers_storage, header, length + 1);
							header = data->offset + 1;
							length = 0;
							message->state &= ~PARSE_CONTINUE;
						}
					}
					else  if (*data->offset == '\n')
					{
						header = data->offset + 1;
						length = 0;
					}
					else if (*data->offset != '\r')
						length++;
					data->offset++;
				}

				/* not enougth data to complete the line */
				if (next == PARSE_HEADER && length > 0)
				{
					_buffer_append(message->headers_storage, header, length);
					message->state |= PARSE_CONTINUE;
				}
			}
			break;
			case PARSE_HEADERNEXT:
			{
				/* create key/value entry for each "<key>:<value>\0" string */
				_httpmessage_fillheaderdb(message);
				/* reset the buffer to begin the content at the begining of the buffer */
				data->length -= (data->offset - data->data);
				while ((*(data->offset + 1) == 0) && data->length > 0)
				{
					data->offset++;
					data->length--;
				}
				memcpy(data->data, data->offset + 1, data->length);
				data->offset = data->data;
				next = PARSE_CONTENT;
			}
			break;
			case PARSE_CONTENT:
			{
				if (message->content_length == 0)
				{
					next = PARSE_END;
				}
				else
				{
					int length = data->length -(data->offset - data->data);
					//httpmessage_addcontent(message, NULL, data->offset, length);

					if (message->content_length > 0)
					{
						message->content_length -= length;
						if (message->content_length <= 0)
							next = PARSE_END;
					}
				}
			}
			break;
			case PARSE_END:
			{
				ret = ESUCCESS;
			}
			break;
		}
		if (next == (message->state & PARSE_MASK) && (ret == ECONTINUE))
			ret = EINCOMPLETE;
		message->state = (message->state & ~PARSE_MASK) | next;
	} while (ret == ECONTINUE);
	return ret;
}

void httpclient_addconnector(http_client_t *client, char *vhost, http_connector_t func, void *funcarg)
{
	http_connector_list_t *callback;

	callback = vcalloc(1, sizeof(*callback));
	if (vhost)
	{
		int length = strlen(vhost);
		callback->vhost = malloc(length + 1);
		strcpy(callback->vhost, vhost);
	}
	callback->func = func;
	callback->arg = funcarg;
	callback->next = client->callbacks;
	client->callbacks = callback;
}

void *httpclient_context(http_client_t *client)
{
	return client->ctx;
}

http_recv_t httpclient_addreceiver(http_client_t *client, http_recv_t func, void *arg)
{
	http_recv_t previous = client->recvreq;
	client->recvreq = func;
	client->ctx = arg;
	return previous;
}

http_send_t httpclient_addsender(http_client_t *client, http_send_t func, void *arg)
{
	http_send_t previous = client->sendresp;
	client->sendresp = func;
	client->ctx = arg;
	return previous;
}

int httpclient_recv(void *ctl, char *data, int length)
{
	http_client_t *client = (http_client_t *)ctl;
	int ret = recv(client->sock, data, length, MSG_NOSIGNAL);
	return ret;
}

int httpclient_send(void *ctl, char *data, int length)
{
	http_client_t *client = (http_client_t *)ctl;
	int ret = send(client->sock, data, length, MSG_NOSIGNAL);
	return ret;
}

static int _httpmessage_buildheader(http_client_t *client, http_message_t *response, buffer_t *header)
{
	if (response->headers == NULL)
		_httpmessage_fillheaderdb(response);
	dbentry_t *headers = response->headers;
	http_message_version_e version = response->version;
	if (response->version > (client->server->config->version & HTTPVERSION_MASK))
		version = (client->server->config->version & HTTPVERSION_MASK);
	_buffer_append(header, _http_message_version[version], strlen(_http_message_version[version]));
	_buffer_append(header, (char *)_http_message_result[response->result], strlen(_http_message_result[response->result]));
	_buffer_append(header, "\r\n", 2);
	while (headers != NULL)
	{
		_buffer_append(header, headers->key, strlen(headers->key));
		_buffer_append(header, ": ", 2);
		_buffer_append(header, headers->value, strlen(headers->value));
		_buffer_append(header, "\r\n", 2);
		headers = headers->next;
	}
	if (response->content_length > 0)
	{
		if (response->keepalive > 0)
		{
			char keepalive[32];
			snprintf(keepalive, 31, "%s: %s\r\n", str_connection, "Keep-Alive");
			_buffer_append(header, keepalive, strlen(keepalive));
		}
		char content_length[32];
		snprintf(content_length, 31, "%s: %d\r\n", str_contentlength, response->content_length);
		_buffer_append(header, content_length, strlen(content_length));
	}
	return ESUCCESS;
}

static http_client_t *_httpclient_create(http_server_t *server)
{
	http_client_t *client = vcalloc(1, sizeof(*client));
	client->server = server;

	client->recvreq = httpclient_recv;
	client->sendresp = httpclient_send;
	client->ctx = client;

	http_connector_list_t *callback = server->callbacks;
	while (callback != NULL)
	{
		httpclient_addconnector(client, callback->vhost, callback->func, callback->arg);
		callback = callback->next;
	}
	client->callback = client->callbacks;

	return client;
}

static void _httpclient_destroy(http_client_t *client)
{
	if (client->request)
		_httpmessage_destroy(client->request);
	if (client->session_storage)
		vfree(client->session_storage);
	vfree(client);
}

static int _httpclient_connect(http_client_t *client)
{

	client->state &= ~CLIENT_STARTED;
	client->state |= CLIENT_RUNNING;
	do
	{
		_httpclient_run(client);
	} while(!(client->state & CLIENT_STOPPED));
	dbg("client %p close", client);
	return 0;
}

static int _httpclient_checkconnector(http_client_t *client, http_message_t *request, http_message_t *response)
{
	int ret = ESUCCESS;
	char *vhost = NULL;
	http_connector_list_t *iterator;

	iterator = client->callbacks;
	while (iterator != NULL)
	{
		vhost = iterator->vhost;
		if (vhost != NULL)
		{
			char *host = httpmessage_REQUEST(request, "host");
			if (!strcasecmp(vhost, host))
				vhost = NULL;
		}

		if (vhost == NULL && iterator->func)
		{
			ret = iterator->func(iterator->arg, request, response);
			if (ret != EREJECT)
			{
				if (ret == ESUCCESS)
				{
					client->state |= CLIENT_RESPONSEREADY;
					if (response->result != RESULT_200)
					{
						ret = EREJECT;
					}
				}
				client->callback = iterator;
				break;
			}
		}
		iterator = iterator->next;
	}
	return ret;
}

void httpclient_finish(http_client_t *client, int close)
{
	client->state &= ~CLIENT_KEEPALIVE;
	if (close)
		client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
}

static int _httpclient_request(http_client_t *client)
{
	int ret = EINCOMPLETE;
	int size = CHUNKSIZE - 1;
	buffer_t *tempo = _buffer_create();
	if (client->request == NULL)
		client->request = _httpmessage_create(client, NULL);

	/**
	 * here, it is the call to the recvreq callback from the
	 * server configuration.
	 * see http_server_config_t and httpserver_create
	 */
	size = client->recvreq(client->ctx, tempo->data, tempo->size);
	if (size < 0)
	{
		if (errno != EAGAIN)
		{
			_buffer_destroy(tempo);
			return EREJECT;
		}
	}
	else if (size > 0)
	{
		tempo->length = size;
		ret = _httpmessage_parserequest(client->request, tempo);

		if (ret == ESUCCESS && client->request->state >= PARSE_CONTENT)
		{
			ret = _httpclient_checkconnector(client, client->request, client->request->response);
		}
	}
	else /* socket shutdown */
	{
		_buffer_destroy(tempo);
		return EREJECT;
	}
	_buffer_destroy(tempo);
	switch (ret)
	{
		case ESUCCESS:
		{
			if (!(client->state & CLIENT_RESPONSEREADY) &&
				(client->request->type == MESSAGE_TYPE_PUT ||
				client->request->type == MESSAGE_TYPE_DELETE))
			{
				client->request->result = RESULT_405;
			}
		}
		break;
		case EREJECT:
		{
			if (client->request->response->result == RESULT_200)
			{
				if (client->request->result == RESULT_200)
					client->request->result = RESULT_400;
				client->request->response->result = client->request->result;
			}
			ret = ESUCCESS;
		}
		break;
		case ECONTINUE:
		case EINCOMPLETE:
		{
			if (client->request->state == PARSE_END)
				ret = ESUCCESS;
		}
		break;
	}
	return ret;
}

static int _httpclient_pushrequest(http_client_t *client, http_message_t *request)
{
	http_message_queue_t *new = calloc(1, sizeof(*new));
	new->message = request;
	new->message->connector = client->callback;

	http_message_queue_t *iterator = client->request_queue;
	if (iterator == NULL)
	{
		client->request_queue = new;
	}
	else
	{
		while (iterator->next != NULL) iterator = iterator->next;
		iterator->next = new;
	}
	return (new->message->result != RESULT_200)? EREJECT:ESUCCESS;
}

static int _httpclient_run(http_client_t *client)
{
	http_message_t *request = NULL;
	if (client->request_queue)
		request = client->request_queue->message;

	int request_ret = ECONTINUE;
	if ((client->server->config->version >= (HTTP11 | HTTP_PIPELINE)) || 
		((client->state & CLIENT_MACHINEMASK) == CLIENT_REQUEST))
	{
		request_ret = _httpclient_request(client);
		if (request_ret == ESUCCESS)
		{
			_httpclient_pushrequest(client, client->request);
		}
	}

	switch (client->state & CLIENT_MACHINEMASK)
	{
		case CLIENT_NEW:
		{
			client->state = CLIENT_REQUEST | (client->state & ~CLIENT_MACHINEMASK);
		}
		break;
		case CLIENT_REQUEST:
		{
			if (request_ret == ESUCCESS)
			{
				client->state = CLIENT_PUSHREQUEST | (client->state & ~CLIENT_MACHINEMASK);
			}
			else if (request_ret == EREJECT)
			{
				client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
				client->state &= ~CLIENT_KEEPALIVE;
			}
#ifdef VTHREAD
			else
			{
				int sret;
				struct timeval *ptimeout = NULL;
				struct timeval timeout;
				fd_set rfds;
				if (client->server->config->keepalive)
				{
					timeout.tv_sec = client->server->config->keepalive;
					timeout.tv_usec = 0;
					ptimeout = &timeout;
				}
				FD_ZERO(&rfds);
				FD_SET(client->sock, &rfds);
				/** allow read and write, because the mod may need write data during the connection (HTTPS) **/
				sret = select(client->sock + 1, &rfds, &rfds, NULL, ptimeout);
				if (sret == 0)
				{
					/* timeout */
					client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
					client->state &= ~CLIENT_KEEPALIVE;
				}
			}
#endif
		}
		break;
		case CLIENT_PUSHREQUEST:
		{
			if (client->request->response->result != RESULT_200)
				client->state = CLIENT_PARSERERROR | (client->state & ~CLIENT_MACHINEMASK);
			else if (client->request->response->content == NULL)
				client->state = CLIENT_PARSER1 | (client->state & ~CLIENT_MACHINEMASK);
			else if (client->request->version == HTTP09)
				client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
			else
				client->state = CLIENT_RESPONSEHEADER | (client->state & ~CLIENT_MACHINEMASK);
			if (client->request->keepalive)
			{
				client->state |= CLIENT_KEEPALIVE;
			}
			if (client->request->response->keepalive)
			{
				client->state |= CLIENT_KEEPALIVE;
			}
			client->request = NULL;
		}
		break;
		case CLIENT_PARSER1:
		{
			int ret = 0;
			ret = request->connector->func(request->connector->arg, request, request->response);
			if (ret == EREJECT)
			{
				client->state = CLIENT_PARSERERROR | (client->state & ~CLIENT_MACHINEMASK);
				/** delete func to stop request after the error response **/
				request->connector->func = NULL;
			}
			else if (ret != EINCOMPLETE)
			{
				if (request->response->version == HTTP09)
					client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
				else
					client->state = CLIENT_RESPONSEHEADER | (client->state & ~CLIENT_MACHINEMASK);
			}
		}
		break;
		case CLIENT_PARSER2:
		{
			int ret = EREJECT;
			if (request->connector->func)
				ret = request->connector->func(request->connector->arg, request, request->response);
			if (ret == EREJECT)
			{
				/** delete func to stop request after the error response **/
				request->connector->func = NULL;
				client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
			}
			else if (ret != ESUCCESS)
			{

				client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
			}
			else
				client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
		}
		break;
		case CLIENT_RESPONSEHEADER:
		{
			int size = 0;
			buffer_t *header = _buffer_create();
			_httpmessage_buildheader(client, request->response, header);

			/**
			 * here, it is the call to the sendresp callback from the
			 * server configuration.
			 * see http_server_config_t and httpserver_create
			 */
			size = client->sendresp(client->ctx, header->data, header->length);
			if (size < 0)
			{
				client->state &= ~CLIENT_KEEPALIVE;
				client->state |= CLIENT_ERROR;
				client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
			}
			else if (client->state & CLIENT_RESPONSEREADY)
				client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
			else
				client->state = CLIENT_PARSER2 | (client->state & ~CLIENT_MACHINEMASK);
			_buffer_destroy(header);
		}
		break;
		case CLIENT_RESPONSECONTENT:
		{
			if (request->response->content)
			if (request->type != MESSAGE_TYPE_HEAD &&
				request->response->content &&
				request->response->content->length > 0)
			{
				int size = CHUNKSIZE - 1;
				size = client->sendresp(client->ctx, request->response->content->data, request->response->content->length);
				if (size < 0)
				{
					client->state &= ~CLIENT_KEEPALIVE;
					client->state |= CLIENT_ERROR;
					client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
				}
				else if (size == 0 && (request->response->content->length > 0))
				{
					client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
				}
				else if (size == request->response->content->length)
				{
					_buffer_reset(request->response->content);
					client->state = CLIENT_PARSER2 | (client->state & ~CLIENT_MACHINEMASK);
				}
				else
				{
					request->response->content->length -= size;
					request->response->content->offset += size;
				}
			}
			else
				client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
		}
		break;
		case CLIENT_PARSERERROR:
		{
			if (request->response->result == RESULT_200)
				request->response->result = RESULT_404;
			httpmessage_addheader(request->response, "Allow", "GET, POST, HEAD");
			const char *value = _http_message_result[request->response->result];
			httpmessage_addcontent(request->response, "text/plain", (char *)value, strlen(value));
			if (request->response->version == HTTP09)
				client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
			else
				client->state = CLIENT_RESPONSEHEADER | (client->state & ~CLIENT_MACHINEMASK);
			client->state |= CLIENT_RESPONSEREADY;
		}
		break;
		case CLIENT_COMPLETE:
		{
			setsockopt(client->sock, IPPROTO_TCP, TCP_NODELAY, (char *) &(int) {1}, sizeof(int));
			if (client->server->config->keepalive &&
				(client->state & CLIENT_KEEPALIVE) &&
				request->response->version > HTTP09 &&
				((client->state & ~CLIENT_ERROR) == client->state))
			{
				client->state = CLIENT_NEW | (client->state & ~CLIENT_MACHINEMASK);
				dbg("keepalive");
			}
			else
			{
				client->state |= CLIENT_STOPPED;
				http_server_mod_t *mod = client->server->mod;
				http_client_modctx_t *modctx = client->modctx;
				while (mod)
				{
					if (mod->freectx)
					{
						mod->freectx(modctx->ctx);
					}
					mod = mod->next;
					modctx = modctx->next;
				}
				shutdown(client->sock, SHUT_RDWR);
		#ifndef WIN32
				close(client->sock);
		#else
				closesocket(client->sock);
		#endif
				client->sock = -1;
			}
			if (request)
			{
				_httpmessage_destroy(request);
				client->request_queue = client->request_queue->next;
			}
		}
		break;
	}
	return 0;
}

static int _httpserver_connect(http_server_t *server)
{
	int ret = 0;
	int maxfd = 0;
	fd_set rfds, wfds;

	server->run = 1;
	while(server->run)
	{
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_SET(server->sock, &rfds);
		maxfd = server->sock;
		http_client_t *client = server->clients;

		while (client != NULL)
		{
			if (client->state & CLIENT_STOPPED)
			{
#ifdef VTHREAD
				vthread_join(client->thread, NULL);
#endif

				http_client_t *client2 = server->clients;
				if (client == server->clients)
				{
					server->clients = client->next;
					client2 = server->clients;
				}
				else
				{
					while (client2->next != client) client2 = client2->next;
					client2->next = client->next;
					client2 = client2->next;
				}
				_httpclient_destroy(client);
				client = client2;
			}
			else if (client->sock > 0)
			{
#ifndef VTHREAD
				if ((client->state & CLIENT_MACHINEMASK) != CLIENT_REQUEST)
				{
					FD_SET(client->sock, &wfds);
				}
				else
					FD_SET(client->sock, &rfds);
#else
				FD_SET(client->sock, &rfds);
#endif
				maxfd = (maxfd > client->sock)? maxfd:client->sock;
				client = client->next;
			}
		}
		ret = select(maxfd +1, &rfds, &wfds, NULL, NULL);
		if (ret > 0)
		{
			if (FD_ISSET(server->sock, &rfds))
			{
				http_client_t *client = _httpclient_create(server);
				// Client connection request recieved
				// Create new client socket to communicate
				client->addr_size = sizeof(client->addr);
				client->sock = accept(server->sock, (struct sockaddr *)&client->addr, &client->addr_size);
				dbg("new connection %p", client);
				http_server_mod_t *mod = server->mod;
				http_client_modctx_t *currentctx = NULL;
				while (mod)
				{
					http_client_modctx_t *modctx = vcalloc(1, sizeof(*modctx));

					if (mod->func)
					{
						modctx->ctx = mod->func(mod->arg, client, (struct sockaddr *)&client->addr, client->addr_size);
					}
					mod = mod->next;
					if (client->modctx == NULL)
						client->modctx = modctx;
					else
					{
						currentctx->next = modctx;
					}
					currentctx = modctx;
				}
				int flags;
				flags = fcntl(client->sock,F_GETFL, 0);
				fcntl(client->sock,F_SETFL, flags | O_NONBLOCK);

				client->next = server->clients;
				server->clients = client;

				ret = 1;
			}
			else
			{
				client = server->clients;
				while (client != NULL)
				{
					http_client_t *next = client->next;
					if (FD_ISSET(client->sock, &rfds) || FD_ISSET(client->sock, &wfds))
					{
#ifndef VTHREAD
						client->state |= CLIENT_RUNNING;
						_httpclient_run(client);
#else
						if (!(client->state & (CLIENT_STARTED | CLIENT_RUNNING)))
						{
							vthread_attr_t attr;
							client->state &= ~CLIENT_STOPPED;
							client->state |= CLIENT_STARTED;
							vthread_create(&client->thread, &attr, (vthread_routine)_httpclient_connect, (void *)client, sizeof(*client));
						}
						else
						{
							vthread_yield(client->thread);
						}
#endif
					}
					client = next;
				}
			}
		}
	}
	return ret;
}

http_server_t *httpserver_create(http_server_config_t *config)
{
	http_server_t *server;

	server = vcalloc(1, sizeof(*server));
	if (config)
		server->config = config;
	else
		server->config = &defaultconfig;
#ifdef WIN32
	WSADATA wsaData = {0};
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
	if (_httpserver_start(server))
	{
		free(server);
		return NULL;
	}
	return server;
}

static int _httpserver_start(http_server_t *server)
{
	int status;

	if (server->config->addr == NULL)
	{
#ifdef IPV6
		server->sock = socket(AF_INET6, SOCK_STREAM, IPPROTO_IP);
#else
		server->sock = socket(PF_INET, SOCK_STREAM, IPPROTO_IP);
#endif
		if ( server->sock < 0 )
		{
			warn("Error creating socket");
			return -1;
		}

		if (setsockopt(server->sock, SOL_SOCKET, SO_REUSEADDR, (void *)&(int){ 1 }, sizeof(int)) < 0)
				warn("setsockopt(SO_REUSEADDR) failed");
#ifdef SO_REUSEPORT
		if (setsockopt(server->sock, SOL_SOCKET, SO_REUSEPORT, (void *)&(int){ 1 }, sizeof(int)) < 0)
				warn("setsockopt(SO_REUSEPORT) failed");
#endif

		int socklen = sizeof(struct sockaddr_in);
#ifdef IPV6
		struct sockaddr_in6saddr;
		saddr.sin6_family = AF_INET6;
		saddr.sin6_port = htons(server->config->port);
		saddr.sin6_addr.s_addr = htonl(INADDR_ANY); // bind socket to any interface
#else
		struct sockaddr_in saddr;

		saddr.sin_family = AF_INET;
		saddr.sin_port = htons(server->config->port);
		saddr.sin_addr.s_addr = htonl(INADDR_ANY); // bind socket to any interface
#endif
		status = bind(server->sock, (struct sockaddr *)&saddr, socklen);
	}
	else
	{
		struct addrinfo hints;
		struct addrinfo *result, *rp;

		memset(&hints, 0, sizeof(struct addrinfo));
#ifdef IPV6
		hints.ai_family = AF_INET6;    /* Allow IPv4 or IPv6 */
#eelse
		hints.ai_family = AF_INET;    /* Allow IPv4 or IPv6 */
#endif
		hints.ai_socktype = SOCK_STREAM; /* Stream socket */
		hints.ai_flags = AI_PASSIVE;    /* For wildcard IP address */
		hints.ai_protocol = 0;          /* Any protocol */
		hints.ai_canonname = NULL;
		hints.ai_addr = NULL;
		hints.ai_next = NULL;

		status = getaddrinfo(server->config->addr, NULL, &hints, &result);
		if (status != 0) {
			fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
			return -1;
		}

		for (rp = result; rp != NULL; rp = rp->ai_next)
		{
			server->sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
			if (server->sock == -1)
				continue;

			if (setsockopt(server->sock, SOL_SOCKET, SO_REUSEADDR, (void *)&(int){ 1 }, sizeof(int)) < 0)
					warn("setsockopt(SO_REUSEADDR) failed");
#ifdef SO_REUSEPORT
			if (setsockopt(server->sock, SOL_SOCKET, SO_REUSEPORT, (void *)&(int){ 1 }, sizeof(int)) < 0)
					warn("setsockopt(SO_REUSEPORT) failed");
#endif

			((struct sockaddr_in *)rp->ai_addr)->sin_port = htons(server->config->port);
			if (bind(server->sock, rp->ai_addr, rp->ai_addrlen) == 0)
				break;                  /* Success */

#ifndef WIN32
			close(server->sock);
#else
			closesocket(server->sock);
#endif
		}
		freeaddrinfo(result); 
	}

	if (!status)
	{
		status = listen(server->sock, server->config->maxclients);
	}
	if (status)
	{
		warn("Error bind/listen port %d", server->config->port);
		return -1;
	}
	return 0;
}

void httpserver_addmod(http_server_t *server, http_getctx_t modf, http_freectx_t unmodf, void *arg)
{
	http_server_mod_t *mod = vcalloc(1, sizeof(*mod));
	mod->func = modf;
	mod->freectx = unmodf;
	mod->arg = arg;
	mod->next = server->mod;
	server->mod = mod;
}

void httpserver_addconnector(http_server_t *server, char *vhost, http_connector_t func, void *funcarg)
{
	http_connector_list_t *callback;
	
	callback = vcalloc(1, sizeof(*callback));
	if (vhost)
	{
		int length = strlen(vhost);
		callback->vhost = malloc(length + 1);
		strcpy(callback->vhost, vhost);
	}
	callback->func = func;
	callback->arg = funcarg;
	callback->next = server->callbacks;
	server->callbacks = callback;
}

void httpserver_connect(http_server_t *server)
{
	vthread_attr_t attr;

#ifndef VTHREAD
	_httpserver_connect(server);
#else
	vthread_create(&server->thread, &attr, (vthread_routine)_httpserver_connect, (void *)server, sizeof(*server));
#endif
}

void httpserver_disconnect(http_server_t *server)
{
	if (server->thread)
	{
		server->run = 0;
#ifdef VTHREAD
		vthread_join(server->thread, NULL);
		server->thread = 0;
#endif
	}
}

void httpserver_destroy(http_server_t *server)
{
	if (server->mod)
		vfree(server->mod);
	vfree(server);
#ifdef WIN32
	WSACleanup();
#endif
}

/*************************************************************
 * httpmessage public functions
 */
void *httpmessage_private(http_message_t *message, void *data)
{
	if (data != NULL)
	{
		message->private = data;
	}
	return message->private;
}

int httpmessage_parsecgi(http_message_t *message, char *data, int size)
{
	buffer_t tempo;
	tempo.data = data;
	tempo.offset = data;
	tempo.length = size;
	tempo.size = size;
	if (message->state == PARSE_INIT)
		message->state = PARSE_HEADER;
	int ret = _httpmessage_parserequest(message, &tempo);
	return ret;
}

http_message_result_e httpmessage_result(http_message_t *message, http_message_result_e result)
{
	if (result > 0)
		message->result = result;
	return message->result;
}

static void _httpmessage_fillheaderdb(http_message_t *message)
{
	int i;
	buffer_t *storage = message->headers_storage;
	if (storage == NULL)
		return;
	char *key = storage->data;
	char *value = NULL;
	for (i = 0; i < storage->length; i++)
	{
		if (storage->data[i] == ':' && value == NULL)
		{
			storage->data[i] = '\0';
			value = storage->data + i + 1;
			while (*value == ' ')
				value++;
		}
		else if (storage->data[i] == '\0')
		{
			_httpmessage_addheader(message, key, value);
			key = storage->data + i + 1;
			value = NULL;
		}
	}
}

void httpmessage_addheader(http_message_t *message, char *key, char *value)
{
	if (message->headers_storage == NULL)
		message->headers_storage = _buffer_create();
	key = _buffer_append(message->headers_storage, key, strlen(key));
	_buffer_append(message->headers_storage, ":", 1);
	value = _buffer_append(message->headers_storage, value, strlen(value) + 1);
}

static void _httpmessage_addheader(http_message_t *message, char *key, char *value)
{
	dbentry_t *headerinfo;
	headerinfo = vcalloc(1, sizeof(dbentry_t));
	headerinfo->key = key;
	headerinfo->value = value;
	headerinfo->next = message->headers;
	message->headers = headerinfo;
	dbg("header %s => %s", key, value);
	if (!strncasecmp(key, str_connection, 10) && strcasestr(value, "Keep-Alive") )
	{
		message->keepalive = 1;
	}
	if (!strncasecmp(key, str_contentlength, 14))
	{
		message->content_length = atoi(value);
	}
}

char *httpmessage_addcontent(http_message_t *message, char *type, char *content, int length)
{
	if (message->content == NULL)
		message->content = _buffer_create();

	if (message->state < PARSE_CONTENT)
	{
		if (type == NULL)
		{
			httpmessage_addheader(message, (char *)str_contenttype, "text/plain");
		}
		else
		{
			httpmessage_addheader(message, (char *)str_contenttype, type);
		}
		/* end the header part */
		if (message->version > HTTP09)
			_buffer_append(message->content, "\r\n", 2);
		message->state = PARSE_CONTENT;
	}
	if (content != NULL)
	{
		if (length == -1)
			length = strlen(content);
		_buffer_append(message->content, content, length);
	}
	if (message->content_length == 0)
		message->content_length = length;
	return message->content->data;
}

int httpmessage_keepalive(http_message_t *message)
{
	message->keepalive = 1;
	return message->client->sock;
}

static char default_value[8] = {0};
char *httpmessage_SERVER(http_message_t *message, char *key)
{
	char *value = default_value;
	char host[NI_MAXHOST], service[NI_MAXSERV];
	memset(default_value, 0, sizeof(default_value));

	if (!strcasecmp(key, "name"))
	{
		value = message->client->server->config->hostname;
	}
	else if (!strcasecmp(key, "software"))
	{
		value = httpserver_software;
	}
	else if (!strcasecmp(key, "protocol"))
	{
		value = _http_message_version[(message->client->server->config->version & HTTPVERSION_MASK)];
	}
	else if (!strcasecmp(key, "port"))
	{
		//snprintf(value, 5, "%d", message->client->server->config->port);
		struct sockaddr_in sin;
		socklen_t len = sizeof(sin);
		if (getsockname(message->client->server->sock, (struct sockaddr *)&sin, &len) == 0)
		{
			getnameinfo((struct sockaddr *) &sin, len,
				0, 0,
				service, NI_MAXSERV, NI_NUMERICSERV);
			value = service;
		}
	}
	else if (!strcasecmp(key, "addr"))
	{
		struct sockaddr_in sin;
		socklen_t len = sizeof(sin);
		if (getsockname(message->client->server->sock, (struct sockaddr *)&sin, &len) == 0)
		{
			getnameinfo((struct sockaddr *) &sin, len,
				host, NI_MAXHOST,
				0, 0, NI_NUMERICHOST);
			value = host;
		}
	}
	return value;
}

char *httpmessage_REQUEST(http_message_t *message, char *key)
{
	char *value = default_value;
	char host[NI_MAXHOST], service[NI_MAXSERV];
	if (!strcasecmp(key, "uri"))
	{
		if (message->uri != NULL)
			value = message->uri->data;
	}
	else if (!strcasecmp(key, "query"))
	{
		if (message->query != NULL)
			value = message->query;
	}
	else if (!strcasecmp(key, "scheme"))
	{
		strcpy(value, "http");
	}
	else if (!strcasecmp(key, "method"))
	{
		switch (message->type)
		{
			case MESSAGE_TYPE_GET:
				value = "GET";
			break;
			case MESSAGE_TYPE_POST:
				value = "POST";
			break;
			case MESSAGE_TYPE_HEAD:
				value = "HEAD";
			break;
#ifndef HTTP_METHOD_PARTIAL
			case MESSAGE_TYPE_PUT:
				value = "PUT";
			break;
			case MESSAGE_TYPE_DELETE:
				value = "DELETE";
			break;
#endif
			default:
			break;
		}
	}
	else if (!strcasecmp(key, str_contentlength))
	{
		if (message->content != NULL)
		{
			value = (char *)message->content_length;
		}
	}
	else if (!strcasecmp(key, "content"))
	{
		if (message->content != NULL)
		{
			value = message->content->data;
		}
	}
	else if (!strncasecmp(key, "remote_addr", 11))
	{
		getnameinfo((struct sockaddr *) &message->client->addr, sizeof(message->client->addr),
			host, NI_MAXHOST, 0, 0, NI_NUMERICHOST);
		value = host;
	}
	else if (!strncasecmp(key, "remote_", 7))
	{
		getnameinfo((struct sockaddr *) &message->client->addr, sizeof(message->client->addr),
			host, NI_MAXHOST,
			service, NI_MAXSERV, NI_NUMERICSERV);

		if (!strcasecmp(key + 7, "host"))
			value = host;
		if (!strcasecmp(key + 7, "port"))
			value = service;
	}
	else
	{
		dbentry_t *header = message->headers;
		while (header != NULL)
		{
			if (!strcasecmp(header->key, key))
			{
				value = header->value;
				break;
			}
			header = header->next;
		}
	}
	return value;
}

char *httpmessage_SESSION(http_message_t *message, char *key, char *value)
{
	dbentry_t *sessioninfo = message->client->session;
	
	while (sessioninfo && strcmp(sessioninfo->key, key))
		sessioninfo = sessioninfo->next;
	if (!sessioninfo)
	{
		sessioninfo = vcalloc(1, sizeof(*sessioninfo));
		if (!message->client->session_storage)
			message->client->session_storage = _buffer_create();
		sessioninfo->key = 
			_buffer_append(message->client->session_storage, key, strlen(key) + 1);
		sessioninfo->next = message->client->session;
		message->client->session = sessioninfo;
	}
	if (value != NULL)
	{
		if (strlen(value) <= strlen(sessioninfo->value))
			strcpy(sessioninfo->value, value);
		else
			sessioninfo->value = 
				_buffer_append(message->client->session_storage, value, strlen(value) + 1);
	}
	return sessioninfo->value;
}
