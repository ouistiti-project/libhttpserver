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

#include "vthread.h"
#include "httpserver.h"

#ifdef DEBUG
# define dbg	printf
#else
# define dbg(...)
#endif

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

#define CLIENT_STARTED 0x01
#define CLIENT_RUNNING 0x02
#define CLIENT_STOPPED 0x04
#define CLIENT_KEEPALIVE 0x80
struct http_client_s
{
	int sock;
	int state;
	http_server_t *server;
	vthread_t thread;
	http_freectx_t freectx;
	http_recv_t recvreq;
	http_send_t sendresp;
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
		PARSE_CONTENT,
		PARSE_END,
	} state;
	buffer_t *content;
	char *uri;
	char *version;
	dbentry_t *headers;
};

struct http_server_callback_s
{
	char *url;
	int url_length;
	http_connector_t func;
	void *arg;
	struct http_server_callback_s *next;
};
typedef struct http_server_callback_s http_server_callback_t;

struct http_server_s
{
	int sock;
	int run;
	vthread_t thread;
	http_client_t *clients;
	http_server_callback_t *callbacks;
	http_server_config_t *config;
};

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
	if (buffer->data + buffer->size < buffer->offset + length + 1)
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
		message->headers = parent->headers;
		message->client = parent->client;
	}
	return message;
}
static void _httpmessage_destroy(http_message_t *message)
{
	_buffer_destroy(message->content);
	free(message);
}

static int _httpmessage_readline(char **out, http_message_t *message)
{
	if (*out == NULL)
		*out = message->content->offset;
	while (message->content->offset < message->content->data + message->content->size)
	{
		if (*message->content->offset == ' ')
		{
			message->content->offset++;
			return ESPACE;
		}
		if (*message->content->offset == '\r')
		{
			*message->content->offset = 0; // add zero terminating  line
			message->content->offset++;
		}
		if (*message->content->offset == '\n')
		{
			*message->content->offset = 0; // add zero terminating  line
			message->content->offset++;
			return ESUCCESS;
		}
		message->content->offset++;
	}
	return EINCOMPLETE;
}

static int _httpserver_parserequest(http_server_t *server, http_message_t *message)
{
	switch (message->state)
	{
		case PARSE_INIT:
		{
			if (!strncasecmp(message->content->offset,"GET ",4))
			{
				message->type = MESSAGE_TYPE_GET;
				message->content->offset += 4;
				message->state = PARSE_URI;
			}
			else if (!strncasecmp(message->content->offset,"POST ",5))
			{
				message->type = MESSAGE_TYPE_POST;
				message->content->offset += 5;
				message->state = PARSE_URI;
			}
			else if (!strncasecmp(message->content->offset,"HEAD ",5))
			{
				message->type = MESSAGE_TYPE_HEAD;
				message->content->offset += 5;
				message->state = PARSE_HEADER;
			}
			while (*message->content->offset == ' ')
				message->content->offset++;
		}
		break;
		case PARSE_URI:
		{
			int ret = _httpmessage_readline(&message->uri, message);
			switch (ret)
			{
				case ESPACE:
					*(message->content->offset - 1) = 0; // add zero terminating  line
					message->state = PARSE_VERSION;
				break;
				case ESUCCESS:
					message->state = PARSE_HEADER;
				break;
				case EINCOMPLETE:
					return EINCOMPLETE;
				break;
				default:
				break;
			}
			return ECONTINUE;
		}
		break;
		case PARSE_VERSION:
		{
			int ret = _httpmessage_readline(&message->version, message);
			switch (ret)
			{
				case ESPACE:
				break;
				case ESUCCESS:
					message->state = PARSE_HEADER;
				break;
				case EINCOMPLETE:
					return EINCOMPLETE;
				break;
				default:
				break;
			}
			return ECONTINUE;
		}
		break;
		case PARSE_HEADER:
		{
			int ret;
			char *header = NULL;
			do
			{
				ret = _httpmessage_readline(&header, message);
				switch (ret)
				{
					case ESPACE:
					break;
					case ESUCCESS:
					{
						if (header[0] == 0) // this is an empty line, the end of the header
						{
							message->state = PARSE_CONTENT;
						}
						else
						{
							char *key = strtok(header, ": ");
							char *value = strtok(NULL, "");
							httpmessage_addheader(message, key, value);
						}
					}
					break;
					case EINCOMPLETE:
						return EINCOMPLETE;
				}
			} while (ret != ESUCCESS);
			return ECONTINUE;
		}
		break;
		case PARSE_CONTENT:
		{
			message->state = PARSE_END;
			return ECONTINUE;
		}
		case PARSE_END:
		{
			return ESUCCESS;
		}
		break;
	}
	return ECONTINUE;
}

static void _httpserver_closeclient(http_server_t *server, http_client_t *client)
{
#ifndef WIN32
	shutdown(client->sock, SHUT_RDWR);
	close(client->sock);
#else
	closesocket(client->sock);
#endif
	if (client == server->clients)
	{
		server->clients = client->next;
	}
	else
	{
		http_client_t *client2 = server->clients;
		while (client2->next != client) client2 = client2->next;
		client2->next = client->next;
	}
	if (client->session_storage)
		free(client->session_storage);
	free(client);
}

int httpclient_recv(void *ctl, char *data, int length)
{
	http_client_t *client = (http_client_t *)ctl;
	int ret = recv(client->sock, data, length, MSG_NOSIGNAL);
	dbg("recv %d\n", ret);
	return ret;
}

int httpclient_send(void *ctl, char *data, int length)
{
	http_client_t *client = (http_client_t *)ctl;
	int ret = send(client->sock, data, length, MSG_NOSIGNAL);
	dbg("send %d\n", ret);
	return ret;
}

http_server_config_t *httpclient_getconfig(void *ctl)
{
	http_client_t *client = (http_client_t *)ctl;

	return client->server->config;
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
	http_server_callback_t *callback = server->callbacks;
	int size = CHUNKSIZE - 1;
	int ret = EINCOMPLETE;

	dbg("%s hello\n",__FUNCTION__);
	client->state &= ~CLIENT_STARTED;
	client->state |= CLIENT_RUNNING;
	request = _httpmessage_create(client->server, NULL);
	request->client = client;
	while (size == CHUNKSIZE - 1)
	{
		char data[CHUNKSIZE];
		size = client->recvreq(client->ctx, data, CHUNKSIZE - 1);
		if (size < 0)
		{
			if (errno != EAGAIN)
				warn("recv returns\n");
			else
				size = 0;
			break;
		}
		if (size > 0)
			_buffer_append(request->content, data, size);
	}
	if (size < 0)
	{
		goto socket_closed;
	}
	// parse the data while the message is complete
	request->content->offset = request->content->data;
	do
	{
		ret = _httpserver_parserequest(server, request);
	}
	while (ret != EINCOMPLETE && ret != ESUCCESS);
	response = _httpmessage_create(server, request);
	if (ret == ESUCCESS)
	{
		char *cburl = NULL;
		while (callback != NULL)
		{
			cburl = callback->url;
			if (cburl != NULL)
			{
				char *path = request->uri;
				if (cburl[0] != '/' && path[0] == '/')
					path++;
				if (!strncasecmp(cburl, path, callback->url_length))
					cburl = NULL;
			}
			if (cburl == NULL && callback->func)
			{
				ret = callback->func(callback->arg, request, response);
				if (ret == ESUCCESS)
					break;
			}
			callback = callback->next;
		}
		if (callback == NULL)
			response->result = RESULT_404;
		if (response->keepalive)
			client->state |= CLIENT_KEEPALIVE;
	}
	else if (ret != EINCOMPLETE && size > 0)
	{
		response->result = RESULT_400;
		request->type = MESSAGE_TYPE_HEAD;
		ret = ESUCCESS;
	}
	buffer_t *header = _buffer_create();
	_httpmessage_buildheader(client, response, header);

	size = client->sendresp(client->ctx, header->data, header->length);
	if (size < 0)
	{
		client->state &= CLIENT_KEEPALIVE;
		goto socket_closed;
	}
	_buffer_destroy(header);

	if (response->content != NULL && request->type != MESSAGE_TYPE_HEAD)
	{
		size = client->sendresp(client->ctx, response->content->data, response->content->length);
		if (size < 0)
		{
			client->state &= CLIENT_KEEPALIVE;
			goto socket_closed;
		}
	}

	while (ret != ESUCCESS)
	{
		if (callback && callback->func)
		{
			ret = callback->func(callback->arg, request, response);
		}
		if (response->content != NULL && request->type != MESSAGE_TYPE_HEAD)
		{
			size = client->sendresp(client->ctx, response->content->data, response->content->length);
			if (size < 0)
			{
				client->state &= CLIENT_KEEPALIVE;
				goto socket_closed;
			}
		}
	}
	setsockopt(client->sock, IPPROTO_TCP, TCP_NODELAY, (char *) &(int) {1}, sizeof(int));

socket_closed:
	if (response)
		_httpmessage_destroy(response);
	if (request)
		_httpmessage_destroy(request);
	if (!(client->state & CLIENT_KEEPALIVE))
	{
		if (client->freectx)
			client->freectx(client->ctx);
		_httpserver_closeclient(server, client);
	}
	else
	{
		dbg("keepalive\n");
	}
	client->state |= CLIENT_STOPPED;
	client->state &= ~(CLIENT_RUNNING | CLIENT_STARTED);
	dbg("%s goodbye\n",__FUNCTION__);
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
			if (!(client->state & CLIENT_STARTED))
			{
				FD_SET(client->sock, &rfds);
				maxfd = (maxfd > client->sock)? maxfd:client->sock;
			}
			client = client->next;
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
				dbg("new client\n");
				if (server->config && server->config->callback.getctx)
				{
					client->ctx = server->config->callback.getctx(client, (struct sockaddr *)&client->addr, size);
					client->freectx = server->config->callback.freectx;
					client->recvreq = server->config->callback.recvreq;
					client->sendresp = server->config->callback.sendresp;
				}
				else
				{
					client->freectx = NULL;
					client->recvreq = httpclient_recv;
					client->sendresp = httpclient_send;
					client->ctx = client;
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
						dbg("%s hello\n",__FUNCTION__);
						if (!(client->state & CLIENT_STARTED))
						{
							vthread_attr_t attr;
							client->state &= ~CLIENT_STOPPED;
							vthread_create(&client->thread, &attr, (vthread_routine)_httpclient_connect, (void *)client, sizeof(*client));
							client->state |= CLIENT_STARTED;
						}
						dbg("%s goodbye\n",__FUNCTION__);
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
	.callback = { NULL, NULL, NULL},
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

void httpserver_addconnector(http_server_t *server, char *url, http_connector_t func, void *funcarg)
{
	http_server_callback_t *callback;
	
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
		vthread_join(server->thread, NULL);
		server->thread = 0;
	}
}

void httpserver_destroy(http_server_t *server)
{
	free(server);
#ifdef WIN32
	WSACleanup();
#endif
}

void httpmessage_addheader(http_message_t *message, char *key, char *value)
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
			value = message->uri;
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
			value = message->uri;
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
