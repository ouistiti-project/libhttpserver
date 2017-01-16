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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#ifndef WIN32
# include <err.h>

# include <sys/socket.h>
# include <sys/ioctl.h>
# include <sys/un.h>
# include <net/if.h>
# include <netinet/in.h>
# include <netinet/tcp.h>
# include <arpa/inet.h>
# include <netdb.h>

#else
# define warn(...) fprintf(stderr, __VA_ARGS__)

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

#ifdef DEBUG
# define dbg(...)	fprintf(stderr, __VA_ARGS__)
#else
# define dbg(...)
#endif


#include "vthread.h"
#include "httpserver.h"

struct dbentry_s
{
	char *key;
	char *value;
	struct dbentry_s *next;
};
typedef struct dbentry_s dbentry_t;

typedef struct buffer_s
{
	char *data;
	char *offset;
	int size;
	int length;
} buffer_t;

struct http_connector_list_s
{
	char *url;
	int url_length;
	http_connector_t func;
	void *arg;
	struct http_connector_list_s *next;
};
typedef struct http_connector_list_s http_connector_list_t;

typedef struct http_server_mod_s
{
	void *arg;
	http_getctx_t func;
	http_freectx_t freectx;
} http_server_mod_t;

#define CLIENT_STARTED 0x0100
#define CLIENT_RUNNING 0x0200
#define CLIENT_STOPPED 0x0400
#define CLIENT_KEEPALIVE 0x8000
#define CLIENT_MACHINEMASK 0x00FF
#define CLIENT_REQUEST 0x0000
#define CLIENT_PARSER 0x0001
#define CLIENT_RESPONSEHEADER 0x0002
#define CLIENT_RESPONSECONTENT 0x0003
#define CLIENT_COMPLETE 0x0004
struct http_client_s
{
	int sock;
	int state;
	http_server_t *server;
	vthread_t thread;
	http_freectx_t freectx;
	http_recv_t recvreq;
	http_send_t sendresp;
	http_connector_list_t *callbacks;
	void *ctx;
	dbentry_t *session;
	buffer_t *session_storage;
#ifndef WIN32
	struct sockaddr_un addr;
#else
	struct sockaddr_in addr;
#endif
	struct http_client_s *next;
};
typedef struct http_client_s http_client_t;

struct http_message_s
{
	http_message_result_e result;
	int keepalive;
	http_client_t *client;
	enum
	{
		MESSAGE_TYPE_GET,
		MESSAGE_TYPE_POST,
		MESSAGE_TYPE_HEAD,
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
	} state;
	buffer_t *content;
	buffer_t *uri;
	char *version;
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

static void _httpmessage_addheader(http_message_t *message, char *key, char *value);

/********************************************************************/
#define CHUNKSIZE 64
static buffer_t * _buffer_create()
{
	buffer_t *buffer = calloc(1, sizeof(*buffer));
	buffer->data = calloc(1, CHUNKSIZE);
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

		data = realloc(buffer->data, buffer->size + chunksize);
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
}

static void _buffer_destroy(buffer_t *buffer)
{
	free(buffer->data);
	free(buffer);
}

static http_message_t * _httpmessage_create(http_server_t *server, http_message_t *parent)
{
	http_message_t *message;

	message = calloc(1, sizeof(*message));
	message->content = _buffer_create();
	if (parent)
	{
		message->type = parent->type;
		message->client = parent->client;
	}
	return message;
}
static void _httpmessage_destroy(http_message_t *message)
{
	if (message->uri)
		_buffer_destroy(message->uri);
	if (message->content)
		_buffer_destroy(message->content);
	if (message->headers_storage)
		_buffer_destroy(message->headers_storage);
	free(message);
}

static int _httpmessage_parserequest(http_message_t *message, buffer_t *data)
{
	int ret = ECONTINUE;
	static char *key = NULL;
	static char *value = NULL;
	do
	{
		int next = message->state;
		switch (message->state)
		{
			case PARSE_INIT:
			{
				key = NULL;
				value = NULL;
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
				else
				{
					data->offset++;
				}
			}
			break;
			case PARSE_URI:
			{
				char *uri = data->offset;
				int length = 0;
				while (data->offset < (data->data + data->size) && next == PARSE_URI)
				{
					switch (*data->offset)
					{
						case ' ':
						{
							next = PARSE_VERSION;
						}
						break;
						case '\r':
						{
							next = PARSE_HEADER;
							*data->offset = '\0';
							if (*data->offset == '\n')
								data->offset++;
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
				
				if (message->uri == NULL)
					message->uri = _buffer_create();
				_buffer_append(message->uri, uri, length);
			}
			break;
			case PARSE_VERSION:
			{
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
				while (data->offset < (data->data + data->size) && next == PARSE_HEADER)
				{
					switch (*data->offset)
					{
						case ':':
						{
							*data->offset = '\0';
							header = _buffer_append(message->headers_storage, header, length + 1);
							if (key == NULL)
							{
								key = header;
								length = 0;
							}
							header = data->offset + 1;
						}
						case ' ':
						{
							/* remove first spaces */
							if (header == data->offset)
								header = data->offset + 1;
						}
						break;
						case '\r':
						{
							*data->offset = '\0';
						}
						break;
						case '\n':
						{
							if (length > 0)
							{
								*data->offset = '\0';
								header = _buffer_append(message->headers_storage, header, length + 1);
								if (value == NULL)
								{
									value = header;
									length = 0;
								}
								next = PARSE_HEADERNEXT;
							}
							else
								next = PARSE_CONTENT;
						}
						break;
						default:
						{
							length++;
						}
					}
					data->offset++;
				}

				/* not enougth data to complete the line */
				if (next == PARSE_HEADER)
				{
					header = _buffer_append(message->headers_storage, header, length);
					if (key == NULL)
						key = header;
					else if (value == NULL)
						value = header;
				}
			}
			break;
			case PARSE_HEADERNEXT:
			{
				if (key != NULL && value != NULL)
				{
					_httpmessage_addheader(message, key, value);
				}
				key = NULL;
				value = NULL;
				next = PARSE_HEADER;
			}
			break;
			case PARSE_CONTENT:
			{
				char *header = data->offset;
				int length = 0;
				while (data->offset < (data->data + data->size))
				{
					if (data->offset >= (data->data + data->length))
					{
						next = PARSE_END;
					}
					data->offset++;
					length++;
				}
				_buffer_append(message->content, header, length);
			}
			case PARSE_END:
			{
				ret = ESUCCESS;
			}
			break;
		}
		if (next == message->state)
			ret = EINCOMPLETE;
		message->state = next;
	} while (ret == ECONTINUE);
	return ret;
}

void httpclient_addconnector(http_client_t *client, char *url, http_connector_t func, void *funcarg)
{
	http_connector_list_t *callback;

	callback = calloc(1, sizeof(*callback));
	if (url)
	{
		callback->url_length = strlen(url);
		callback->url = malloc(callback->url_length + 1);
		strcpy(callback->url, url);
	}
	callback->func = func;
	callback->arg = funcarg;
	callback->next = client->callbacks;
	client->callbacks = callback;
}

void httpclient_addreceiver(http_client_t *client, http_recv_t func, void *arg)
{
	client->recvreq = func;
	client->ctx = arg;
}

void httpclient_addsender(http_client_t *client, http_send_t func, void *arg)
{
	client->sendresp = func;
	client->ctx = arg;
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

static const char *_http_message_result[] =
{
	"200 OK",
	"400 Bad Request",
	"404 File Not Found",
};

static int _httpmessage_buildheader(http_client_t *client, http_message_t *response, buffer_t *header)
{
	dbentry_t *headers = response->headers;
	_buffer_append(header, "HTTP/1.1 ", 9);
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
	if (response->content != NULL)
	{
		char content_length[32];
		snprintf(content_length, 31, "Content-Length: %d\r\n", (int)strlen(response->content->data));
		_buffer_append(header, content_length, strlen(content_length));
	}
	_buffer_append(header, "\r\n", 2);
	return ESUCCESS;
}

static int _httpclient_connect(http_client_t *client)
{
	http_message_t *request = NULL;
	http_message_t *response = NULL;
	http_server_t *server = client->server;
	http_connector_list_t *callback = client->callbacks;
	int size = CHUNKSIZE - 1;
	int ret = EINCOMPLETE;

	client->state &= ~CLIENT_STARTED;
	client->state |= CLIENT_RUNNING;
	do
	{
		switch (client->state & CLIENT_MACHINEMASK)
		{
			case CLIENT_REQUEST:
			{
				request = _httpmessage_create(client->server, NULL);
				request->client = client;
				buffer_t *tempo = _buffer_create();
				do
				{
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
							warn("recv returns\n");
							break;
						}
						else
							size = 0;
					}
					if (size > 0)
					{
						tempo->length = size;
						ret = _httpmessage_parserequest(request, tempo);
					}
					_buffer_reset(tempo);
				} while (ret != ESUCCESS);
				_buffer_destroy(tempo);
				if (size < 0)
				{
					client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
					break;
				}
				client->state = CLIENT_PARSER | (client->state & ~CLIENT_MACHINEMASK);
			}
			break;
			case CLIENT_PARSER:
			{
				response = _httpmessage_create(server, request);
				if (ret == ESUCCESS)
				{
					char *cburl = NULL;
					while (callback != NULL)
					{
						cburl = callback->url;
						if (cburl != NULL)
						{
							char *path = request->uri->data;
							if (cburl[0] != '/' && path[0] == '/')
								path++;
							if (!strncasecmp(cburl, path, callback->url_length))
								cburl = NULL;
						}
						if (cburl == NULL && callback->func)
						{
							ret = callback->func(callback->arg, request, response);
							if (ret != EREJECT)
								break;
						}
						callback = callback->next;
					}
					if (callback == NULL)
					{
						response->result = RESULT_404;
					}
					if (response->keepalive)
					{
						client->state |= CLIENT_KEEPALIVE;
					}
				}
				else if (ret != EINCOMPLETE && size > 0)
				{
					response->result = RESULT_400;
					request->type = MESSAGE_TYPE_HEAD;
					ret = ESUCCESS;
				}
				client->state = CLIENT_RESPONSEHEADER | (client->state & ~CLIENT_MACHINEMASK);
			}
			break;
			case CLIENT_RESPONSEHEADER:
			{
				buffer_t *header = _buffer_create();
				_httpmessage_buildheader(client, response, header);

				/**
				 * here, it is the call to the sendresp callback from the
				 * server configuration.
				 * see http_server_config_t and httpserver_create
				 */
				size = client->sendresp(client->ctx, header->data, header->length);
				if (size < 0)
				{
					client->state &= ~CLIENT_KEEPALIVE;
					client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
					break;
				}
				_buffer_destroy(header);
				client->state = CLIENT_RESPONSECONTENT | (client->state & ~CLIENT_MACHINEMASK);
			}
			break;
			case CLIENT_RESPONSECONTENT:
			{
				if (response->result == RESULT_200 &&
					response->content->length > 0 &&
					request->type != MESSAGE_TYPE_HEAD)
				{
					size = client->sendresp(client->ctx, response->content->data, response->content->length);
					if (size < 0)
					{
						client->state &= ~CLIENT_KEEPALIVE;
						client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
						break;
					}
				}

				while (ret == ECONTINUE)
				{
					if (callback && callback->func)
					{
						ret = callback->func(callback->arg, request, response);
					}
					if (response->result == RESULT_200 &&
						response->content->length > 0 &&
						request->type != MESSAGE_TYPE_HEAD)
					{
						size = client->sendresp(client->ctx, response->content->data, response->content->length);
						if (size < 0)
						{
							client->state &= ~CLIENT_KEEPALIVE;
							client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
							break;
						}
					}
				}
				client->state = CLIENT_COMPLETE | (client->state & ~CLIENT_MACHINEMASK);
			}
			break;
			case CLIENT_COMPLETE:
			{
				setsockopt(client->sock, IPPROTO_TCP, TCP_NODELAY, (char *) &(int) {1}, sizeof(int));
				if (response)
					_httpmessage_destroy(response);
				if (request)
					_httpmessage_destroy(request);
				client->state |= CLIENT_STOPPED;
				if (!(client->state & CLIENT_KEEPALIVE))
				{
					if (client->freectx)
						client->freectx(client->ctx);
					shutdown(client->sock, SHUT_RDWR);
			#ifndef WIN32
					close(client->sock);
			#else
					closesocket(client->sock);
			#endif
					client->sock = -1;
				}
				else
				{
					dbg("keepalive\n");
				}
			}
			break;
		}
	} while(!(client->state & CLIENT_STOPPED));
	return 0;
}

static int _httpserver_connect(http_server_t *server)
{
	int ret = 0;
	int maxfd = 0;
	fd_set rfds;

	server->run = 1;
	while(server->run)
	{
		FD_ZERO(&rfds);
		FD_SET(server->sock, &rfds);
		maxfd = server->sock;
		http_client_t *client = server->clients;
		while (client != NULL)
		{
			if (client->state & CLIENT_STOPPED)
			{
				vthread_join(client->thread, NULL);

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
				if (client->session_storage)
					free(client->session_storage);
				free(client);
				client = client2;
			}
			else if (client->sock > 0)
			{
				FD_SET(client->sock, &rfds);
				maxfd = (maxfd > client->sock)? maxfd:client->sock;
				client = client->next;
			}
		}
		ret = select(maxfd +1, &rfds, NULL, NULL, NULL);
		if (ret > 0)
		{
			if (FD_ISSET(server->sock, &rfds))
			{
				http_client_t *client = calloc(1, sizeof(*client));
				client->server = server;
				// Client connection request recieved
				// Create new client socket to communicate
				unsigned int size = sizeof(client->addr);
				client->sock = accept(server->sock, (struct sockaddr *)&client->addr, &size);

				client->recvreq = httpclient_recv;
				client->sendresp = httpclient_send;
				client->ctx = client;
				client->freectx = NULL;
				if (server->mod && server->mod->func)
				{
					client->ctx = server->mod->func(server->mod->arg, client, (struct sockaddr *)&client->addr, size);
					client->freectx = server->mod->freectx;
				}

				http_connector_list_t *callback = server->callbacks;
				while (callback != NULL)
				{
					httpclient_addconnector(client, callback->url, callback->func, callback->arg);
					callback = callback->next;
				}

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
					if (FD_ISSET(client->sock, &rfds))
					{
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
					}
					client = next;
				}
			}
		}
	}
	return ret;
}

static http_server_config_t defaultconfig = { 
	.addr = NULL,
	.port = 80,
	.maxclient = 10,
	.chunksize = 64,
};

http_server_t *httpserver_create(http_server_config_t *config)
{
	http_server_t *server;
	int status;

	server = calloc(1, sizeof(*server));
	if (config)
		server->config = config;
	else
		server->config = &defaultconfig;
#ifdef WIN32
	WSADATA wsaData = {0};
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
	if (server->config->addr == NULL)
	{
		server->sock = socket(PF_INET, SOCK_STREAM, IPPROTO_IP);
		if ( server->sock < 0 )
		{
		 warn("Error creating socket");
		 free(server);
		 return NULL;
		}

		if (setsockopt(server->sock, SOL_SOCKET, SO_REUSEADDR, (void *)&(int){ 1 }, sizeof(int)) < 0)
				warn("setsockopt(SO_REUSEADDR) failed");
#ifdef SO_REUSEPORT
		if (setsockopt(server->sock, SOL_SOCKET, SO_REUSEPORT, (void *)&(int){ 1 }, sizeof(int)) < 0)
				warn("setsockopt(SO_REUSEPORT) failed");
#endif

		int socklen = sizeof(struct sockaddr_in);
		struct sockaddr_in saddr;

		saddr.sin_family = AF_INET;
		saddr.sin_port = htons(server->config->port);
		saddr.sin_addr.s_addr = htonl(INADDR_ANY); // bind socket to any interface
		status = bind(server->sock, (struct sockaddr *)&saddr, socklen);
	}
	else
	{
		struct addrinfo hints;
		struct addrinfo *result, *rp;

		memset(&hints, 0, sizeof(struct addrinfo));
		hints.ai_family = AF_INET;    /* Allow IPv4 or IPv6 */
		hints.ai_socktype = SOCK_STREAM; /* Stream socket */
		hints.ai_flags = AI_PASSIVE;    /* For wildcard IP address */
		hints.ai_protocol = 0;          /* Any protocol */
		hints.ai_canonname = NULL;
		hints.ai_addr = NULL;
		hints.ai_next = NULL;

		status = getaddrinfo(server->config->addr, NULL, &hints, &result);
		if (status != 0) {
			fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
			return NULL;
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
		status = listen(server->sock, server->config->maxclient);
	}
	if (status)
	{
		warn("Error bind/listen socket");
		free(server);
		return NULL;
	}
	return server;
}

void httpserver_addmod(http_server_t *server, http_getctx_t mod, http_freectx_t unmod, void *arg)
{
	if (!server->mod)
		server->mod = calloc(1, sizeof(*server->mod));
	server->mod->func = mod;
	server->mod->freectx = unmod;
	server->mod->arg = arg;
}

void httpserver_addconnector(http_server_t *server, char *url, http_connector_t func, void *funcarg)
{
	http_connector_list_t *callback;
	
	callback = calloc(1, sizeof(*callback));
	if (url)
	{
		callback->url_length = strlen(url);
		callback->url = malloc(callback->url_length + 1);
		strcpy(callback->url, url);
	}
	callback->func = func;
	callback->arg = funcarg;
	callback->next = server->callbacks;
	server->callbacks = callback;
}

void httpserver_connect(http_server_t *server)
{
	vthread_attr_t attr;

	vthread_create(&server->thread, &attr, (vthread_routine)_httpserver_connect, (void *)server, sizeof(*server));
}

void httpserver_disconnect(http_server_t *server)
{
	if (server->thread)
	{
		server->run = 0;
		vthread_join(server->thread, NULL);
		server->thread = 0;
	}
}

void httpserver_destroy(http_server_t *server)
{
	if (server->mod)
		free(server->mod);
	free(server);
#ifdef WIN32
	WSACleanup();
#endif
}

void *httpmessage_private(http_message_t *message, void *data)
{
	if (data != NULL)
		message->private = data;
	return message->private;
}

void httpmessage_addheader(http_message_t *message, char *key, char *value)
{
	if (message->headers_storage == NULL)
		message->headers_storage = _buffer_create();
	key = _buffer_append(message->headers_storage, key, strlen(key) + 1);
	value = _buffer_append(message->headers_storage, value, strlen(value) + 1);
	_httpmessage_addheader(message, key, value);
}

static void _httpmessage_addheader(http_message_t *message, char *key, char *value)
{
	dbentry_t *headerinfo;
	headerinfo = calloc(1, sizeof(dbentry_t));
	headerinfo->key = key;
	headerinfo->value = value;
	headerinfo->next = message->headers;
	message->headers = headerinfo;
	if (!strncasecmp(key, "Connection", 10) && !strncasecmp(value, "KeepAlive", 10) )
		message->keepalive = 1;
}

void httpmessage_addcontent(http_message_t *message, char *type, char *content, int length)
{
	if (type == NULL)
	{
		httpmessage_addheader(message, "Content-Type", "text/plain");
	}
	else
	{
		httpmessage_addheader(message, "Content-Type", type);
	}
	if (length == -1)
		length = strlen(content);
	if (content != NULL)
	{
		_buffer_append(message->content, content, length);
	}
}

int httpmessage_keepalive(http_message_t *message)
{
	message->keepalive = 1;
	return message->client->sock;
}

char *httpmessage_SERVER(http_message_t *message, char *key)
{
	char *value = "";
	char host[NI_MAXHOST], service[NI_MAXSERV];

	if (!strcasecmp(key, "uri"))
	{
		if (message->uri != NULL)
			value = message->uri->data;
	}
	else if (!strcasecmp(key, "protocol"))
	{
		if (message->version != NULL)
			value = message->version;
	}
	else if (!strncasecmp(key, "remote_", 7))
	{
		getnameinfo((struct sockaddr *) &message->client->addr, sizeof(message->client->addr),
			host, NI_MAXHOST,
			service, NI_MAXSERV, NI_NUMERICSERV);

		if (!strcasecmp(key + 7, "host"))
			value = host;
		if (!strcasecmp(key + 7, "service"))
			value = host;
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
			default:
			break;
		}
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

char *httpmessage_REQUEST(http_message_t *message, char *key)
{
	char *value = "";
	if (!strcasecmp(key, "uri"))
	{
		if (message->uri != NULL)
			value = message->uri->data;
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
		sessioninfo = calloc(1, sizeof(*sessioninfo));
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
