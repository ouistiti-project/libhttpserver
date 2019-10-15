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
#include <unistd.h>
#include <errno.h>

#ifndef WIN32
# include <sys/types.h>
# include <sys/socket.h>
# include <sys/ioctl.h>
# include <sys/un.h>
# include <net/if.h>
# include <netinet/in.h>
# include <netinet/tcp.h>
# include <arpa/inet.h>
# include <netdb.h>
# include <fcntl.h>
# include <signal.h>

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

#include "../../compliant.h"
#include "log.h"
#include "httpserver.h"
#include "_httpserver.h"

#define tcp_dbg(...)

#ifndef HAVE_GETNAMEINFO
struct addrinfo
{
	int ai_family;
	int ai_socktype;
	int ai_flags;
	int ai_protocol;
	const struct sockaddr *ai_canonname;
	const struct sockaddr *ai_addr;
	socklen_t ai_addrlen;
	struct addrinfo *ai_next;
};
#define AI_PASSIVE 0x01
#define AI_ADDRCONFIG 0x02
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static void *tcpclient_create(void *config, http_client_t *clt)
{
	http_server_t *server = (http_server_t *)config;
	if (server && server->sock < 0)
		return NULL;

	// Client connection request recieved
	// Create new client socket to communicate
	clt->addr_size = sizeof(clt->addr);
	if (server)
	{
		clt->sock = accept(server->sock, (struct sockaddr *)&clt->addr, &clt->addr_size);
		if (clt->sock == -1)
		{
			dbg("tcp accept error %s", strerror(errno));
			return NULL;
		}
	}

	return clt;
}

#ifdef HTTPCLIENT_FEATURES
static int tcpclient_connect(void *ctl, const char *addr, int port)
{
	http_client_t *client = (http_client_t *)ctl;
	int family = AF_INET;
#ifdef IPV6
	family = AF_INET6;
	struct sockaddr_in6 *saddr;
	saddr = (struct sockaddr_in6 *)&client->addr;
	saddr->sin6_family = AF_INET6;
	saddr->sin6_port = htons(port);
	if (inet_pton(AF_INET6, addr, &saddr->sin6_addr) == 0)
	{
		family = AF_INET;
		inet_pton(AF_INET, addr, &saddr->sin6_addr);
	}
#else
	struct sockaddr_in *saddr;
	saddr = (struct sockaddr_in *)&client->addr;
	saddr->sin_family = family;
	saddr->sin_port = htons(port);
	inet_aton(addr, &saddr->sin_addr);
#endif
	client->addr_size = sizeof(*saddr);

	if (client->sock != 0)
		return EREJECT;
	client->sock = socket(family, SOCK_STREAM, 0);
	if (client->sock == -1)
	{
		client->sock = 0;
		return EREJECT;
	}

	if (connect(client->sock, (struct sockaddr *)&client->addr, client->addr_size) != 0)
	{
		err("server connection to %s failed: %s", addr, strerror(errno));
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
	if (ret < 0)
	{
		if (errno == EAGAIN)
			ret = EINCOMPLETE;
		else
			ret = EREJECT;
		//err("client %p recv error %s %d", client, strerror(errno), ret);
	}
	else
	{
		tcp_dbg("tcp recv %d %.*s", ret, ret, data);
	}
	return ret;
}

static int tcpclient_send(void *ctl, const char *data, int length)
{
	int ret;
	http_client_t *client = (http_client_t *)ctl;

	ret = send(client->sock, data, length, MSG_NOSIGNAL);
	if (ret < 0)
	{
		if (errno == EAGAIN)
			ret = EINCOMPLETE;
		else
			ret = EREJECT;
		//err("client %p send error %s %d", client, strerror(errno), ret);
	}
	else
	{
		tcp_dbg("tcp send %d %.*s", ret, length, data);
	}
	return ret;
}

static int tcpclient_status(void *ctl)
{
	http_client_t *client = (http_client_t *)ctl;
	if (client->sock < 0)
		return EREJECT;
	int nbbytes = 0;
	int ret = ioctl(client->sock, FIONREAD, &nbbytes);
	tcp_dbg("client status (%p %x) %d %d", client, client->state, ret, nbbytes);
	if (ret < 0)
		return EREJECT;
	if (nbbytes == 0)
		return EINCOMPLETE;
	return ESUCCESS;
}

static void tcpclient_flush(void *ctl)
{
	http_client_t *client = (http_client_t *)ctl;

	setsockopt(client->sock, IPPROTO_TCP, TCP_NODELAY, (char *) &(int) {1}, sizeof(int));
}

static void tcpclient_disconnect(void *ctl)
{
	http_client_t *client = (http_client_t *)ctl;
	if (client->sock > -1)
	{
		/**
		 * the client must receive information about the closing,
		 * but the rest of this software needs to be aware too.
		 * The real closing is done outside.
		 */
		shutdown(client->sock, SHUT_RDWR);
		warn("client %p shutdown", client);
	}
}

static void tcpclient_destroy(void *ctl)
{
	http_client_t *client = (http_client_t *)ctl;
	if (client->sock > 0)
	{
		/**
		 * Every body is aware about the closing.
		 * And the socket is really closed now.
		 * The socket must be close to free the
		 * file descriptor of the kernel.
		 */
		warn("client %p close", client);
#ifndef WIN32
		close(client->sock);
#else
		closesocket(client->sock);
#endif
	}
	client->sock = -1;
}

const httpclient_ops_t *tcpclient_ops = &(httpclient_ops_t)
{
	.scheme = str_defaultscheme,
	.default_port = 80,
	.create = tcpclient_create,
	.connect = tcpclient_connect,
	.recvreq = tcpclient_recv,
	.sendresp = tcpclient_send,
	.status = tcpclient_status,
	.flush = tcpclient_flush,
	.disconnect = tcpclient_disconnect,
	.destroy = tcpclient_destroy,
};

#ifdef TCP_SIGHANDLER
static void handler(int sig, siginfo_t *si, void *arg)
{
}
#endif

static int _tcpserver_start(http_server_t *server)
{
	int status;

#ifdef WIN32
	WSADATA wsaData = {0};
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#elif defined(TCP_SIGHANDLER)
/**
 * If the socket is open into more than one process, the sending on it
 * may return a SIGPIPE.
 * It is possible to disable the signal or to close the socket into
 * all process except the sender.
 */
	struct sigaction action;
	action.sa_flags = SA_SIGINFO;
	sigemptyset(&action.sa_mask);
	//action.sa_sigaction = handler;
	action.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &action, NULL);
#endif

	struct addrinfo hints;
	struct addrinfo *result, *rp;

	memset(&hints, 0, sizeof(struct addrinfo));
#ifdef IPV6
	hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
#else
	hints.ai_family = AF_INET;
#endif
	hints.ai_socktype = SOCK_STREAM; /* Stream socket */
	hints.ai_flags = AI_PASSIVE | AI_ADDRCONFIG;    /* For wildcard IP address */
	hints.ai_protocol = 0;          /* Any protocol */
	hints.ai_canonname = NULL;
	hints.ai_addr = NULL;
	hints.ai_next = NULL;

#ifdef HAVE_GETNAMEINFO
	status = getaddrinfo(server->config->addr, str_defaultscheme, &hints, &result);
#endif
	if (status != 0) {
		result = &hints;
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

		int ret;
		if (!rp->ai_addr)
		{
			struct sockaddr *saddr;
			int saddrlen;
#ifdef IPV6
			if (rp->ai_family == AF_INET6)
			{
				struct sockaddr_in6 saddr_in6;
				saddr_in6.sin6_port = htons(server->config->port);
				saddr_in6.sin6_flowinfo = 0;
				saddr_in6.sin6_addr = in6addr_any;
				saddr = (struct sockaddr *)&saddr_in6;
				saddrlen = sizeof(saddr_in6);
			}
			else if (rp->ai_family == AF_INET)
#endif
			{
				struct sockaddr_in saddr_in;
				saddr_in.sin_family = AF_INET;
				saddr_in.sin_addr.s_addr = htonl(INADDR_ANY);
				saddr_in.sin_port = htons(server->config->port);
				saddr = (struct sockaddr *)&saddr_in;
				saddrlen = sizeof(saddr_in);
			}

			status = bind(server->sock, saddr, saddrlen);
		}
		else
		{
#ifdef IPV6
			if (rp->ai_family == AF_INET6)
			{
				((struct sockaddr_in6 *)rp->ai_addr)->sin6_port = htons(server->config->port);
			}
			else if (rp->ai_family == AF_INET)
#endif
			{
				((struct sockaddr_in *)rp->ai_addr)->sin_port = htons(server->config->port);
			}
			status = bind(server->sock, rp->ai_addr, rp->ai_addrlen);
		}
		server->type = rp->ai_family;
		if (status == 0)
			/* Success */
			break;
		server->ops->close(server);
	}
#ifdef HAVE_GETNAMEINFO
	if (result != &hints)
		freeaddrinfo(result);
#endif

	if (status == 0)
	{
#if defined(SERVER_DEFER_ACCEPT) && defined(TCP_DEFER_ACCEPT)
		if (setsockopt(server->sock, IPPROTO_TCP, TCP_DEFER_ACCEPT, (void *)&(int){ 0 }, sizeof(int)) < 0)
				warn("setsockopt(TCP_DEFER_ACCEPT) failed");
#endif
#ifdef SERVER_NODELAY
		if (setsockopt(server->sock, IPPROTO_TCP, TCP_NODELAY, (void *)&(int){ 1 }, sizeof(int)) < 0)
				warn("setsockopt(TCP_NODELAY) failed");
#endif

		status = listen(server->sock, SOMAXCONN);//server->config->maxclients);
	}
	if (status)
	{
		if (server->config->addr)
			err("Error bind/listen %s port %d", server->config->addr, server->config->port);
		else
			err("Error bind/listen port %d", server->config->port);
		return -1;
	}
	return 0;
}

static http_client_t *_tcpserver_createclient(http_server_t *server)
{
	http_client_t * client = httpclient_create(server, server->protocol_ops, server->protocol);

	if (client != NULL)
	{
		char hoststr[NI_MAXHOST];

		int rc;
#ifdef HAVE_GETNAMEINFO
		rc = getnameinfo((struct sockaddr *)&client->addr,
			client->addr_size, hoststr, sizeof(hoststr), NULL, 0,
			NI_NUMERICHOST | NI_NUMERICSERV);
#else
		struct hostent *entity;

		entity = gethostbyaddr((void *)&client->addr, client->addr_size, server->type);
		strncpy(hoststr, entity->h_name, NI_MAXHOST);
		rc = 0;
#endif
		if (rc == 0)
			warn("new connection %p (%d) from %s %d", client, client->sock, hoststr, server->config->port);
#ifndef BLOCK_SOCKET
		int flags;
		flags = fcntl(httpclient_socket(client), F_GETFL, 0);
		fcntl(httpclient_socket(client), F_SETFL, flags | O_NONBLOCK);
		flags = fcntl(httpclient_socket(client), F_GETFD, 0);
		fcntl(httpclient_socket(client), F_SETFD, flags | FD_CLOEXEC);
#endif
	}
	return client;
}

static void _tcpserver_close(http_server_t *server)
{
	http_client_t *client = server->clients;
	while (client != NULL)
	{
		http_client_t *next = client->next;
		client->ops->disconnect(client->opsctx);
		client->ops->destroy(client->opsctx);
		client = next;
	}
	if (server->sock > 0)
		shutdown(server->sock, SHUT_RDWR);
	warn("server %p close", server);
	server->sock = -1;
#ifdef WIN32
	WSACleanup();
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
