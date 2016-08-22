/*****************************************************************************
 * httpserver.c: Simple HTTP server
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <err.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <pthread.h>
#include <signal.h>

#include "httpserver.h"

#define ESUCCESS 0
#define EINCOMPLETE -1
#define ECONTINUE -2
#define ESPACE -3

struct http_client_s
{
	int sock;
	struct sockaddr_un addr;
	struct http_client_s *next;
};
typedef struct http_client_s http_client_t;

struct http_header_s
{
	char *key;
	char *value;
	struct http_header_s *next;
};
typedef struct http_header_s http_header_t;

struct http_message_s
{
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
	char *buffer;
	char *offset;
	int buff_size;
	char *uri;
	char *version;
	http_header_t *headers;
	char *content;
	int content_length;
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
	pthread_t thread;
	http_client_t *clients;
	http_server_callback_t *callbacks;
};

/********************************************************************/
static http_message_t * _httpserver_message_create(http_server_t *server, http_message_t *parent)
{
	http_message_t *message;

	message = calloc(1, sizeof(*message));
	message->buffer = calloc(1, 64);
	message->buff_size = 64;
	message->offset = message->buffer;
	return message;
}
static void _httpserver_message_destroy(http_message_t *message)
{
	free(message->buffer);
	message->buffer == NULL;
	free(message);
}

static int _httpserver_readline(char **out, http_message_t *message)
{
	int size = 0;
	char *offset;
	if (*out)
	{
		size += strlen(*out);
	}
	size += message->buff_size;
	size -= message->offset - message->buffer;
	*out = realloc(*out, size + 1);
	offset = *out;
	offset += strlen(*out);
	while (message->offset < message->buffer +message->buff_size)
	{
		if (*message->offset == ' ')
		{
			*offset = 0; // add zero terminating  line
			message->offset++;
			return ESPACE;
		}
		if (*message->offset == '\r')
			message->offset++;
		if (*message->offset == '\n')
		{
			*message->offset++;
			break;
		}
		*offset = *message->offset;
		offset++;
		message->offset++;
		if (message->offset > message->buffer +message->buff_size)
			return EINCOMPLETE;
	}
	*offset = 0; // add zero terminating  line
	return ESUCCESS;
}

static int _httpserver_parserequest(http_server_t *server, http_message_t *message)
{
	switch (message->state)
	{
		case PARSE_INIT:
		{
			if (!strncasecmp(message->offset,"GET ",4))
			{
				message->type = MESSAGE_TYPE_GET;
				message->offset += 4;
				message->state = PARSE_URI;
			}
			else if (!strncasecmp(message->offset,"POST ",5))
			{
				message->type = MESSAGE_TYPE_POST;
				message->offset += 5;
				message->state = PARSE_URI;
			}
			else if (!strncasecmp(message->offset,"HEAD ",5))
			{
				message->type = MESSAGE_TYPE_HEAD;
				message->offset += 5;
				message->state = PARSE_HEADER;
			}
			while (*message->offset == ' ')
				message->offset++;
		}
		break;
		case PARSE_URI:
		{
			int ret = _httpserver_readline(&message->uri, message);
			switch (ret)
			{
				case ESPACE:
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
			int ret = _httpserver_readline(&message->version, message);
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
			ret = _httpserver_readline(&header, message);
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
						http_header_t *headerinfo;
						headerinfo = calloc(1, sizeof(http_header_t));
						headerinfo->key = strtok(header, ": ");
						headerinfo->value = strtok(NULL, "");
						headerinfo->next = message->headers;
						message->headers = headerinfo;
					}
				}
				break;
				case EINCOMPLETE:
					return EINCOMPLETE;
			}
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
			FD_SET(client->sock, &rfds);
			maxfd = (maxfd > client->sock)? maxfd:client->sock;
			client = client->next;
		}
		ret = select(maxfd +1, &rfds, NULL, NULL, NULL);
		if (ret > 0)
		{
			if (FD_ISSET(server->sock, &rfds))
			{
				http_client_t *client = calloc(1, sizeof(*client));
				// Client connection request recieved
				// Create new client socket to communicate
				int size = sizeof(client->addr);
				client->sock = accept(server->sock, (struct sockaddr *)&client->addr, &size);
				fcntl(client->sock, F_SETFL, fcntl(client->sock, F_GETFL, 0) | O_NONBLOCK);
				client->next = server->clients;
				server->clients = client;
				ret = 1;
			}
			client = server->clients;
			while (client != NULL)
			{
				http_message_t *request = _httpserver_message_create(server, NULL);
				if (FD_ISSET(client->sock, &rfds))
				{
					int size = 1;
					request->client = client;
					while (size > 0)
					{
						size = read(client->sock, request->buffer, request->buff_size);
						if (size <= 0)
							break;
						// parse the data while the message is complete
						do
						{
							ret = _httpserver_parserequest(server, request);
						}
						while (ret != EINCOMPLETE && ret != ESUCCESS);
					}
					if (ret == ESUCCESS)
					{
						http_server_callback_t *callback = server->callbacks;
						http_message_t *response = _httpserver_message_create(server, request);
						response->client = client;
						while (callback != NULL)
						{
							if ((callback->url == NULL) || (!strncasecmp(callback->url, request->uri, callback->url_length)))
							{
								int close = 0;
								if (callback->func)
									callback->func(callback->arg, request, response);

								send(client->sock, "HTTP/1.1 200 OK\r\n", 17, 0);
								http_header_t *header = response->headers;
								while (header != NULL)
								{
									send(client->sock, header->key, strlen(header->key), 0);
									send(client->sock, ": ", 2, 0);
									send(client->sock, header->value, strlen(header->value), 0);
									send(client->sock, "\r\n", 2, 0);
									header = header->next;
								}
								if (response->keepalive)
								{
									send(client->sock, "Connection: keep-alive\r\n", 24, 0);
								}
								else
								{
									send(client->sock, "Connection: close\r\n", 19, 0);
									close = 1;
								}
								if (response->content != NULL)
								{
									char content_length[32];
									snprintf(content_length, 31, "Content-Length: %d\r\n", response->content_length);
									send(client->sock, content_length, strlen(content_length), 0);
									send(client->sock, "\r\n", 2, 0);
									send(client->sock, response->content, response->content_length, 0);
								}
								send(client->sock, "\r\n", 2, 0);
								_httpserver_message_destroy(response);
								if (close)
								{
									shutdown(client->sock, SHUT_RDWR);
									
									if (client == server->clients)
									{
										server->clients = client->next;
										free(client);
									}
									else
									{
										http_client_t *client2 = server->clients;
										while (client2->next != client) client2 = client2->next;
										client2->next = client->next;
										free(client);
									}
								}
							}
							callback = callback->next;
						}
					}
					
				}
				if (ret != ESUCCESS)
				{
					send(client->sock, "HTTP/1.1 400 Bad Request\r\n", 17, 0);
				}
				_httpserver_message_destroy(request);
				client = client->next;
			}
		}
	}
	return ret;
}

http_server_t *httpserver_create(char *address, int port, int maxclient)
{
	http_server_t *server;
	int status;

	server = calloc(1, sizeof(*server));
	if (address == NULL)
	{
		server->sock = socket(PF_INET, SOCK_STREAM, IPPROTO_IP);
		if ( server->sock < 0 )
		{
		 warn("Error creating socket");
		 free(server);
		 return NULL;
		}

		if (setsockopt(server->sock, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 }, sizeof(int)) < 0)
				warn("setsockopt(SO_REUSEADDR) failed");
#ifdef SO_REUSEPORT
		if (setsockopt(server->sock, SOL_SOCKET, SO_REUSEPORT, &(int){ 1 }, sizeof(int)) < 0)
				warn("setsockopt(SO_REUSEADDR) failed");
#endif

		int socklen = sizeof(struct sockaddr_in);
		struct sockaddr_in saddr;

		saddr.sin_family = AF_INET;
		saddr.sin_port = htons(port);
		saddr.sin_addr.s_addr = htonl(INADDR_ANY); // bind socket to any interface
		status = bind(server->sock, (struct sockaddr *)&saddr, socklen);
		warn("Error bind socket");
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

		status = getaddrinfo(address, NULL, &hints, &result);
		if (status != 0) {
			fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
			return NULL;
		}

		/* getaddrinfo() returns a list of address structures.
		Try each address until we successfully bind(2).
		If socket(2) (or bind(2)) fails, we (close the socket
		and) try the next address. */

		for (rp = result; rp != NULL; rp = rp->ai_next)
		{
			server->sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
			if (server->sock == -1)
				continue;

			if (setsockopt(server->sock, SOL_SOCKET, SO_REUSEADDR, &(int){ 1 }, sizeof(int)) < 0)
					warn("setsockopt(SO_REUSEADDR) failed");
#ifdef SO_REUSEPORT
			if (setsockopt(server->sock, SOL_SOCKET, SO_REUSEPORT, &(int){ 1 }, sizeof(int)) < 0)
					warn("setsockopt(SO_REUSEADDR) failed");
#endif

			((struct sockaddr_in *)rp->ai_addr)->sin_port = htons(port);
			if (bind(server->sock, rp->ai_addr, rp->ai_addrlen) == 0)
				break;                  /* Success */

			close(server->sock);
		}
		freeaddrinfo(result); 
	}

	if (!status)
	{
		status = listen(server->sock, maxclient);
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

typedef void *(*pthread_routine)(void*);
void httpserver_connect(http_server_t *server)
{
	pthread_attr_t attr;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

//	sigset_t set;
//	sigemptyset(&set);
//	sigaddset(&set, SIGINT);
//	sigaddset(&set,SIGTERM);
//	pthread_sigmask(SIG_BLOCK, &set, NULL);

	pthread_create(&server->thread, &attr, (pthread_routine)_httpserver_connect, (void *)server);
}

void httpserver_disconnect(http_server_t *server)
{
	if (server->thread)
	{
		pthread_kill(server->thread, SIGINT);
		pthread_join(server->thread, NULL);
		server->thread = 0;
	}
}

void httpserver_destroy(http_server_t *server)
{
	free(server);
}

void httpmessage_addheader(http_message_t *message, char *key, char *value)
{
	http_header_t *headerinfo;
	headerinfo = calloc(1, sizeof(http_header_t));
	headerinfo->key = key;
	headerinfo->value = value;
	headerinfo->next = message->headers;
	message->headers = headerinfo;
}

void httpmessage_addcontent(http_message_t *message, char *type, char *content, int length)
{
	int size = 0;
	if (type == NULL)
	{
		httpmessage_addheader(message, "Content-Type", "text/plain");
	}
	else
	{
		httpmessage_addheader(message, "Content-Type", type);
	}
	if (message->content != NULL)
		size = strlen(message->content);
	if (length == -1)
		length = strlen(content);
	message->content = realloc(message->content, length + size + 1);
	memcpy(message->content + size, content, length);
	*(message->content + size + length) = 0;
	message->content_length = size + length;
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
		http_header_t *header = message->headers;
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
#ifdef TEST

int test_func(void *arg, http_message_t *request, http_message_t *response)
{
	char content[] = "<html><body>coucou<br/></body></html>";
	httpmessage_addheader(response, "Server", "libhttpserver");
	httpmessage_addcontent(response, "text/html", content, strlen(content));
	return 0;
}

int main(int argc, char * const *argv)
{
	http_server_t *server = httpserver_create(NULL, 80, 10);
	if (server)
	{
		httpserver_addconnector(server, NULL, test_func, NULL);
		httpserver_connect(server);
		pause();
		httpserver_disconnect(server);
	}
	return 0;
}
#endif
