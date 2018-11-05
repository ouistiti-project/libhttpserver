/*****************************************************************************
 * client_websocket.c: websocket client application
 * this file is part of https://github.com/ouistiti-project/libhttpserver
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define HAVE_GETOPT
#include <unistd.h>

#include <pthread.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <sys/ioctl.h>
#include <sys/select.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <libgen.h> // basename

#include <stdarg.h>

#include <pwd.h> // getpwnam

#include "httpserver/httpserver.h"
#include "httpserver/websocket.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define DAEMONIZE 0x01

#ifndef STATICKEY
#define STATICKEY "4851d4fa7a309fd21eda05699e9d8595"
#endif

#define CHUNKSIZE 64
#define HTTP_ENDLINE "\r\n"

typedef struct http_s http_t;
struct http_s
{
	http_message_t *message;
	int sock;
	int (*send)(http_t *thiz, const void *buf, size_t len);
	int (*recv)(http_t *thiz, void *buf, size_t len);
	void (*close)(http_t *thiz);
};

static int _http_send(http_t *thiz, const void *buf, size_t len)
{
	return send(thiz->sock, buf, len, MSG_NOSIGNAL);
}

static int _http_recv(http_t *thiz, void *buf, size_t len)
{
	return recv(thiz->sock, buf, len, MSG_NOSIGNAL);
}

static void _http_close(http_t *thiz)
{
	shutdown(thiz->sock, SHUT_RDWR);
	close(thiz->sock);
}

http_t *http_create(int sock)
{
	http_t *http = calloc(1, sizeof(*http));
	http->message = httpmessage_create(CHUNKSIZE);
	http->sock = sock;
	http->send = _http_send;
	http->recv = _http_recv;
	http->close = _http_close;
	return http;
}

int http_result(http_t *thiz)
{
	return httpmessage_result(thiz->message, 0);
}

const char *http_header(http_t *thiz, char *key)
{
	return httpmessage_REQUEST(thiz->message, key);
}

http_t *http_open(char *url, ...)
{
	int port = 80;

dbg("url: %s", url);
	char *host = strstr(url, "http://");
	if (host == NULL)
		return NULL;
	host += sizeof("http://") - 1;

	char *portname = strstr(host, ":");
	if (portname != NULL)
	{
		port = atoi(portname + 1);
	}
	else
		portname = host;

	char *pathname = strstr(portname, "/");
	if (pathname == NULL)
		return NULL;
	host = strndup(host, pathname - portname);
dbg("host: %s", host);
dbg("port: %d", port);
dbg("pathname: %s", pathname);


	struct sockaddr_in saddr;
	struct addrinfo hints;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;    /* Allow IPv4 or IPv6 */
	hints.ai_socktype = SOCK_STREAM; /* Stream socket */
	hints.ai_flags = AI_PASSIVE;    /* For wildcard IP address */
	hints.ai_protocol = 0;          /* Any protocol */
	hints.ai_canonname = NULL;
	hints.ai_addr = NULL;
	hints.ai_next = NULL;

	struct addrinfo *result, *rp;
	getaddrinfo(host, NULL, &hints, &result);

	int sock = -1;
	for (rp = result; rp != NULL; rp = rp->ai_next)
	{
		sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (sock == -1)
			continue;

		((struct sockaddr_in *)rp->ai_addr)->sin_port = htons(port);
		if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0)
			break;
		close(sock);
		sock = -1;
	}

	if (sock == -1)
	{
		err("socket error on %s : %s", url, strerror(errno));
		return NULL;
	}

	http_t *thiz = http_create(sock);

	int ret;
	ret = thiz->send(thiz, "GET ", 4);
	ret = thiz->send(thiz, pathname, strlen(pathname));
	ret = thiz->send(thiz, " HTTP/1.1", sizeof(" HTTP/1.1") -1);
	ret = thiz->send(thiz, HTTP_ENDLINE, sizeof(HTTP_ENDLINE) -1);
	ret = thiz->send(thiz, "Host: ", 6);
	ret = thiz->send(thiz, host, strlen(host));
	ret = thiz->send(thiz, HTTP_ENDLINE, sizeof(HTTP_ENDLINE) -1);

	va_list ap;
	va_start(ap, url);
	char *header = va_arg(ap, char *);
	while (header != NULL)
	{
		ret = thiz->send(thiz, header, strlen(header));
		ret = thiz->send(thiz, HTTP_ENDLINE, sizeof(HTTP_ENDLINE) -1);
		header = va_arg(ap, char *);
	}
	va_end(ap);
	ret = thiz->send(thiz, HTTP_ENDLINE, sizeof(HTTP_ENDLINE) -1);

	do
	{
		char buffer[CHUNKSIZE];
		int length = thiz->recv(thiz, buffer, sizeof(buffer) - 1);
		if (length > 0)
		{
			buffer[length] = 0;
			int rest = length;

			ret = httpmessage_parsecgi(thiz->message, buffer, &rest);
		}
		else
			break;
	} while (ret == EINCOMPLETE);

	return thiz;
}

void http_close(http_t *thiz)
{
	thiz->close(thiz);
}

typedef struct _websocket_s _websocket_t;
struct _websocket_s
{
	http_t *http;
	int sock;
	pthread_t thread;
	pthread_attr_t attr;
};

static int _websocket_close(void *arg, int status)
{
	_websocket_t *thiz = (_websocket_t *)arg;
	char message[] = { 0x88, 0x02, 0x03, 0xEA};
	return thiz->http->send(thiz->http, message, sizeof(message));
}

static int _websocket_pong(void *arg, char *data)
{
	_websocket_t *thiz = (_websocket_t *)arg;
	char message[] = { 0x8A, 0x00};
	return thiz->http->send(thiz->http, message, sizeof(message));
}

static int _websocket_ping(void *arg, char *data)
{
	_websocket_t *thiz = (_websocket_t *)arg;
	char message[] = { 0x8A, 0x00};
	return thiz->http->send(thiz->http, message, sizeof(message));
}

static websocket_t _wsdefaul_config =
{
	.onclose = _websocket_close,
	.onping = _websocket_pong,
	.type = WS_TEXT,
};

static void *_websocket_run(void *arg)
{
	_websocket_t *thiz = (_websocket_t *)arg;
	int run = 1;

	do
	{
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(thiz->sock, &rfds);
		FD_SET(thiz->http->sock, &rfds);
		int maxfd = (thiz->sock > thiz->http->sock)? thiz->sock:thiz->http->sock;

		int ret = select(maxfd + 1, &rfds, NULL, NULL, NULL);
		if (ret > 0 && FD_ISSET(thiz->http->sock, &rfds))
		{
			int length = 0;

			ret = ioctl(thiz->http->sock, FIONREAD, &length);
			if (ret == 0 && length > 0)
			{
				char *buffer = calloc(1, length);
				ret = thiz->http->recv(thiz->http, buffer, length);
				if (ret > 0)
				{
					char *out = calloc(1, length);
					ret = websocket_unframed(buffer, ret, out, arg);
					dbg("recv %s", out);
					ret = send(thiz->sock, out, ret, MSG_NOSIGNAL);
					free(out);
				}
				else if (ret < 0)
				{
					warn("websocket: %d %d error %s", ret, length, strerror(errno));
					run = 0;
				}
				free(buffer);
			}
			else
			{
				char buffer[64];
				ret = read(thiz->sock, buffer, 63);
				warn("websocket: %d %d error %s", ret, length, strerror(errno));
				run = 0;
			}
		}
		else if (ret > 0 && FD_ISSET(thiz->sock, &rfds))
		{
			int length;
			ret = ioctl(thiz->sock, FIONREAD, &length);
			if (ret == 0 && length > 0)
			{
				char *buffer;
				buffer = calloc(1, length);
				while (length > 0)
				{
					ret = recv(thiz->sock, buffer, length, MSG_NOSIGNAL);
					if (ret > 0)
					{
						length -= ret;
						ssize_t size = 0;
						char *out = calloc(1, ret + MAX_FRAGMENTHEADER_SIZE);
						while (size < ret)
						{
							ssize_t length;
							int outlength = 0;
							length = websocket_framed(WS_TEXT, (char *)buffer, ret, out, &outlength, arg);
							outlength = thiz->http->send(thiz->http, (char *)out, outlength);
							if (outlength == EINCOMPLETE)
								continue;
							if (outlength == EREJECT)
								break;
							size += length;
						}
						free(out);
						if (size < ret)
						{
							ret = -1;
							break;
						}
					}
					else
						break;
				}
				free(buffer);
			}
			else
				ret = -1;
			if (ret < 0)
			{
				run = 0;
				warn("websocket: client died");
			}
		}
		else if (errno != EAGAIN)
		{
			warn("websocket: error %s", strerror(errno));
			run = 0;
		}
	} while (run);

	return NULL;
}

_websocket_t *websocket_create(int sock, http_t *http)
{
	_websocket_t *thiz = NULL;
	char *handshake = http_header(http, "Sec-WebSocket-Accept");

	if (handshake != NULL)
	{
		warn("handshake: %s", handshake);

		thiz = calloc(1, sizeof(*thiz));
		thiz->http = http;
		thiz->sock = sock;
		pthread_create(&thiz->thread, &thiz->attr, _websocket_run, thiz);
	}
	return thiz;
}

void help(char **argv)
{
	fprintf(stderr, "%s -R <socket directory> -u <URL>", argv[0]);
	exit(0);
}

static void _setpidfile(char *pidfile)
{
	if (pidfile[0] != '\0')
	{
		int pidfd = open(pidfile,O_RDWR|O_CREAT,0640);
		if (pidfd > 0)
		{
			char buffer[32];
			int length;
			pid_t pid = 1;

#ifdef HAVE_LOCKF
			if (lockf(pidfd, F_TLOCK,0)<0)
			{
				err("server already running");
				exit(0);
			}
#endif
			pid = getpid();
			length = snprintf(buffer, 32, "%d\n", pid);
			write(pidfd, buffer, length);
			/**
			 * the file must be open while the process is running
			close(pidfd);
			 */ 
		}
		else
		{
			err("pid file error %s", strerror(errno));
			pidfile = NULL;
		}
	}
}

const char *str_username = "www-data";

int main(int argc, char **argv)
{
	char *root = "/var/run/websocket";
	char *url = NULL;
	char *pidfile = NULL;
	char *name = basename(argv[0]);
	const char *username = str_username;
	int mode = 0;
	int opt;

	do
	{
		opt = getopt(argc, argv, "R:hDU:u:p:");
		switch (opt)
		{
			case 'R':
				root = optarg;
			break;
			case 'p':
				pidfile = optarg;
			break;
			case 'u':
				username = optarg;
			break;
			case 'U':
				url = optarg;
			break;
			case 'h':
				help(argv);
				return -1;
			break;
			case 'D':
				mode |= DAEMONIZE;
			break;
		}
	} while(opt != -1);

	if (url == NULL)
	{
		help(argv);
		return -1;
	}

	if (access(root, R_OK|W_OK|X_OK))
	{
		if (mkdir(root, 0777))
		{
			err("access %s error %s", root, strerror(errno));
			return -1;
		}
		chmod(root, 0777);
	}

	if ((mode & DAEMONIZE) && fork() != 0)
	{
		return 0;
	}

	if (pidfile)
		_setpidfile(pidfile);

	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock > 0)
	{
		int ret = 0;
		struct sockaddr_un addr;
		memset(&addr, 0, sizeof(struct sockaddr_un));

		addr.sun_family = AF_UNIX;
		snprintf(addr.sun_path, sizeof(addr.sun_path) - 1, "%s/%s", root, name);
		unlink(addr.sun_path);

		ret = bind(sock, (struct sockaddr *) &addr, sizeof(addr));
		if (ret == 0)
		{

			chmod(addr.sun_path, 0777);
			ret = listen(sock, 3);
		}
		if (getuid() == 0)
		{
			struct passwd *user = NULL;
			user = getpwnam(username);
			setgid(user->pw_gid);
			seteuid(user->pw_uid);
		}

		if (ret == 0)
		{
			int newsock = 0;
			do
			{
				fd_set rfds;
				int maxfd = sock;
				FD_ZERO(&rfds);
				FD_SET(sock, &rfds);
				ret = select(maxfd + 1, &rfds, NULL, NULL, NULL);
				if (ret > 0)
				{
					if (FD_ISSET(sock, &rfds))
					{
						struct sockaddr_storage addr;
						int addrsize = sizeof(addr);
						newsock = accept(sock, (struct sockaddr *)&addr, &addrsize);
						if (newsock > 0)
						{
							int result = 0;
							http_t *http = http_open(url, 
										"Connection: Upgrade",
										"Upgrade: websocket",
										"Sec-WebSocket-Key:"STATICKEY,
										NULL);
							if (http != NULL)
							{
								result = http_result(http);
							}
							dbg("connection result %d", result);
							if (result == 101)
								websocket_create(newsock, http);
							else if (result == 200)
							{
							}
							else if (result == 401 | result == 403)
							{
							}
							else
								http_close(http);
						}						
					}				
				}
			} while(newsock > 0);
		}
		
		unlink(addr.sun_path);
		if (ret)
		{
			err("%s: error %s\n", argv[0], strerror(errno));
		}
	}
	return 0;
}
