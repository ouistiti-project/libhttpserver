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
#include <unistd.h>
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

#include "httpserver.h"
#include "_httpserver.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
# define dbg(...)
#endif

#ifdef HTTPCLIENT_FEATURES
static int tcpclient_connect(void *ctl, char *addr, int port)
{
	http_client_t *client = (http_client_t *)ctl;

	struct sockaddr_in *saddr;

	client->addr_size = sizeof(*saddr);
	saddr = (struct sockaddr_in *)&client->addr;
	saddr->sin_family = AF_INET;
	saddr->sin_port = htons(port);
	inet_aton(addr, &saddr->sin_addr);

	if (client->sock != 0)
		return EREJECT;
	client->sock = socket(AF_INET, SOCK_STREAM, 0);
	if (client->sock == -1)
	{
		client->sock = 0;
		return EREJECT;
	}

	if (connect(client->sock, (struct sockaddr *)&client->addr, client->addr_size) != 0)
	{
		err("server connection failed: %s", strerror(errno));
		close(client->sock);
		client->sock = 0;
		return EREJECT;
	}
	return ESUCCESS;
}
#else
#define tcpclient_connect NULL
#endif

static int tcpclient_recv(void *ctl, char *data, int length)
{
	http_client_t *client = (http_client_t *)ctl;
	int ret = recv(client->sock, data, length, MSG_NOSIGNAL);
	return ret;
}

static int tcpclient_send(void *ctl, char *data, int length)
{
	http_client_t *client = (http_client_t *)ctl;
	int ret = send(client->sock, data, length, MSG_NOSIGNAL);
	return ret;
}

static void tcpclient_flush(void *ctl)
{
	http_client_t *client = (http_client_t *)ctl;
	setsockopt(client->sock, IPPROTO_TCP, TCP_NODELAY, (char *) &(int) {1}, sizeof(int));
}

static void tcpclient_close(void *ctl)
{
	http_client_t *client = (http_client_t *)ctl;
	if (client->sock > -1)
	{
		shutdown(client->sock, SHUT_RDWR);
#ifndef WIN32
		close(client->sock);
#else
		closesocket(client->sock);
#endif
	}
	client->sock = -1;
}

httpclient_ops_t *httpclient_ops = &(httpclient_ops_t)
{
	.connect = tcpclient_connect,
	.recvreq = tcpclient_recv,
	.sendresp = tcpclient_send,
	.flush = tcpclient_flush,
	.close = tcpclient_close,
};

static int _tcpserver_start(http_server_t *server)
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
#else
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
			server->ops->close(server);
		}
		freeaddrinfo(result); 
	}

	if (!status)
	{
		status = listen(server->sock, server->config->maxclients);
	}
	if (status)
	{
		if (server->config->addr)
			warn("Error bind/listen %s port %d", server->config->addr, server->config->port);
		else
			warn("Error bind/listen port %d", server->config->port);
		return -1;
	}
	return 0;
}

static http_client_t *_tcpserver_createclient(http_server_t *server)
{
	http_client_t * client = httpclient_create(server, server->config->chunksize);
	client->ops = httpclient_ops;
	client->ctx = client;

	// Client connection request recieved
	// Create new client socket to communicate
	client->addr_size = sizeof(client->addr);
	client->sock = accept(server->sock, (struct sockaddr *)&client->addr, &client->addr_size);
	char hoststr[NI_MAXHOST];
	char portstr[NI_MAXSERV];

	int rc = getnameinfo((struct sockaddr *)&client->addr, 
		client->addr_size, hoststr, sizeof(hoststr), portstr, sizeof(portstr), 
		NI_NUMERICHOST | NI_NUMERICSERV);

	if (rc == 0) 
		warn("new connection %p from %s %s", client, hoststr, portstr);
	return client;
}

static void _tcpserver_close(http_server_t *server)
{
	http_client_t *client = server->clients;
	while (client != NULL)
	{
		http_client_t *next = client->next;
		client->ops->close(client);
		client = next;
	}

#ifndef WIN32
	close(server->sock);
#else
	closesocket(server->sock);
#endif
}

//httpserver_ops_t *tcpops = &(httpserver_ops_t)
httpserver_ops_t *httpserver_ops = &(httpserver_ops_t)
{
	.start = _tcpserver_start,
	.createclient = _tcpserver_createclient,
	.close = _tcpserver_close,
};

//httpserver_ops_t *httpserver_ops __attribute__ ((weak, alias ("tcpops")));
