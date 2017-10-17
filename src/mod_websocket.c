/*****************************************************************************
 * mod_websocket.c: websocket server module
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
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <errno.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>

#if defined(MBEDTLS)
# include <mbedtls/sha1.h>
# define SHA1_ctx mbedtls_sha1_context
# define SHA1_init(pctx) \
	do { \
		mbedtls_sha1_init((pctx)); \
		mbedtls_sha1_starts((pctx)); \
	} while(0)
# define SHA1_update(pctx, in, len) \
	mbedtls_sha1_update((pctx), in, len)
# define SHA1_finish(out, pctx) \
	do { \
		mbedtls_sha1_finish((pctx), out); \
		mbedtls_sha1_free((pctx)); \
	} while(0)
#else
typedef struct SHA1_ctx_s{ char *input; int inputlen;} SHA1_ctx;
# define SHA1_init(pctx)
# define SHA1_update(pctx, in, len) \
	do { \
		(pctx)->input = in; \
		(pctx)->inputlen = len; \
	} while(0)
# define SHA1_finish(out, pctx) \
	do { \
		memcpy(out, (pctx)->input, (pctx)->inputlen); \
	} while(0)
#endif
#if defined(MBEDTLS)
# include <mbedtls/base64.h>
# define BASE64_encode(in, inlen, out, outlen) \
	do { \
		size_t cnt = 0; \
		mbedtls_base64_encode(out, outlen, &cnt, in, inlen); \
	}while(0)
# define BASE64_decode(in, inlen, out, outlen) \
	do { \
		size_t cnt = 0; \
		mbedtls_base64_decode(out, outlen, &cnt, in, inlen); \
	}while(0)
#else
# include "b64/cencode.h"
# define BASE64_encode(in, inlen, out, outlen) \
	do { \
		base64_encodestate state; \
		base64_init_encodestate(&state); \
		int cnt = base64_encode_block(in, inlen, out, &state); \
		cnt += base64_encode_blockend(out + cnt, &state); \
		out[cnt - 1] = '\0'; \
	}while(0)		 
#endif

#include "httpserver/httpserver.h"
#include "httpserver/uri.h"
#include "httpserver/mod_websocket.h"
#include "httpserver/utils.h"
#include "httpserver/websocket.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

typedef struct _mod_websocket_s _mod_websocket_t;
typedef struct _mod_websocket_ctx_s _mod_websocket_ctx_t;

struct _mod_websocket_s
{
	mod_websocket_t *config;
	void *vhost;
	mod_websocket_run_t run;
	void *runarg;
};

struct _mod_websocket_ctx_s
{
	_mod_websocket_t *mod;
	char *protocol;
	int socket;
};

static char *str_connection = "Connection";
static char *str_upgrade = "Upgrade";
static char *str_websocket = "websocket";
static char *str_protocol = "Sec-WebSocket-Protocol";
static char *str_accept = "Sec-WebSocket-Accept";
static char *str_key = "Sec-WebSocket-Key";

static void _mod_websocket_handshake(_mod_websocket_ctx_t *ctx, http_message_t *request, http_message_t *response)
{
	char *key = httpmessage_REQUEST(request, str_key);
	if (key && key[0] != 0)
	{
		char accept[20] = {0};
		SHA1_ctx ctx;
		SHA1_init(&ctx);
		SHA1_update(&ctx, key, strlen(key));
		SHA1_update(&ctx, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", sizeof("258EAFA5-E914-47DA-95CA-C5AB0DC85B11") -1);
		SHA1_finish(accept, &ctx);

		char out[40];
		BASE64_encode(accept, 20, out, 40);

		httpmessage_addheader(response, str_accept, out);
	}
}

static int _mod_websocket_check(_mod_websocket_ctx_t *ctx, char * protocol)
{
	int ret = EREJECT;

	if (protocol)
	{
		char *service = ctx->mod->config->services;
		int length = 0;
		char *end = service;

		while (end != NULL)
		{
			end = strchr(service, ',');
			if (end)
				length = end - service;
			else
				length = strlen(service);

			if (strlen(protocol) == length && !strncmp(protocol, service, length))
			{
				ret = ESUCCESS;
				break;
			}
			service += length + 1;
		}
	}

	return ret;
}

static int websocket_connector(void *arg, http_message_t *request, http_message_t *response)
{
	int ret = EREJECT;
	_mod_websocket_ctx_t *ctx = (_mod_websocket_ctx_t *)arg;

	if (ctx->protocol == NULL)
	{
		char *connection = httpmessage_REQUEST(request, str_connection);

		if (strcasestr(connection, str_upgrade))
		{
			char *upgrade = httpmessage_REQUEST(request, str_upgrade);

			if (strcasestr(upgrade, str_websocket))
			{
				char *protocol = httpmessage_REQUEST(request, str_protocol);
				if (protocol[0] != '\0')
				{
					ctx->protocol = malloc(strlen(protocol) + 1);
					strcpy(ctx->protocol, protocol);
					httpmessage_addheader(response, str_protocol, ctx->protocol);
				}
				else
				{
					ctx->protocol = utils_urldecode(httpmessage_REQUEST(request, "uri"));
				}
				ret = _mod_websocket_check(ctx, ctx->protocol);

				if (ret == ESUCCESS)
				{
					ctx->socket = httpmessage_lock(response);
					_mod_websocket_handshake(ctx, request, response);
					httpmessage_addheader(response, str_connection, str_upgrade);
					httpmessage_addheader(response, str_upgrade, str_websocket);
					httpmessage_addcontent(response, "none", "", -1);
					httpmessage_result(response, RESULT_101);
					dbg("result 101");
					ret = ECONTINUE;
				}
				else
				{
					httpmessage_result(response, RESULT_404);
					free(ctx->protocol);
					ctx->protocol = NULL;
					ret = ESUCCESS;
				}
			}
		}
	}
	else
	{
		ctx->mod->run(ctx->mod->runarg, ctx->socket, ctx->protocol, request);
		free(ctx->protocol);
		ctx->protocol = NULL;
		ret = ESUCCESS;
	}
	return ret;
}

static void *_mod_websocket_getctx(void *arg, http_client_t *ctl, struct sockaddr *addr, int addrsize)
{
	_mod_websocket_t *mod = (_mod_websocket_t *)arg;

	_mod_websocket_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->mod = mod;
	httpclient_addconnector(ctl, mod->vhost, websocket_connector, ctx);

	return ctx;
}

static void _mod_websocket_freectx(void *arg)
{
	_mod_websocket_ctx_t *ctx = (_mod_websocket_ctx_t *)arg;

	free(ctx);
}

void *mod_websocket_create(http_server_t *server, char *vhost, void *config, mod_websocket_run_t run, void *runarg)
{
	_mod_websocket_t *mod = calloc(1, sizeof(*mod));

	mod->vhost = vhost;
	mod->config = config;
	mod->run = run;
	mod->runarg = runarg;
	httpserver_addmod(server, _mod_websocket_getctx, _mod_websocket_freectx, mod);

	return mod;
}

void mod_websocket_destroy(void *data)
{
	free(data);
}

static int _websocket_socket(void *arg, char *protocol)
{
	mod_websocket_t *config = (mod_websocket_t *)arg;
	int sock;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path) - 1, "%s/%s", config->path, protocol);

	dbg("websocket %s", addr.sun_path);
	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock > 0)
	{
		int ret = connect(sock, (struct sockaddr *) &addr, sizeof(addr));
		if (ret < 0)
		{
			close(sock);
			sock = -1;
		}
	}
	if (sock == -1)
	{
		warn("websocket error: %s", strerror(errno));
	}
	return sock;
}

typedef struct _websocket_main_s _websocket_main_t;
struct _websocket_main_s
{
	int client;
	int socket;
	http_recv_t recvreq;
	http_send_t sendresp;
	void *ctx;
};

static int websocket_close(void *arg, int status)
{
	_websocket_main_t *info = (_websocket_main_t *)arg;
	char message[] = { 0x88, 0x02, 0x03, 0xEA};
	return info->sendresp(info->ctx, message, sizeof(message));
}

static int websocket_pong(void *arg, char *data)
{
	_websocket_main_t *info = (_websocket_main_t *)arg;
	char message[] = { 0x8A, 0x00};
	return info->sendresp(info->ctx, message, sizeof(message));
}

static void *_websocket_main(void *arg)
{
	_websocket_main_t *info = (_websocket_main_t *)arg;
	int socket = info->socket;
	int client = info->client;
	int end = 0;

	while (!end)
	{
		fd_set rdfs;
		int maxfd = socket;
		FD_ZERO(&rdfs);
		FD_SET(socket, &rdfs);
		maxfd = (maxfd > client)?maxfd:client;
		FD_SET(client, &rdfs);
		int ret = select(maxfd + 1, &rdfs, NULL, NULL, NULL);
		if (ret > 0 && FD_ISSET(socket, &rdfs))
		{
			int length = 0;
			ret = ioctl(socket, FIONREAD, &length);
			if (ret == 0 && length > 0)
			{
				char *buffer = calloc(1, length);
				ret = info->recvreq(info->ctx, (char *)buffer, length);
				//ret = read(socket, buffer, 63);
				if (ret > 0)
				{
					char *out = calloc(1, length);
					ret = websocket_unframed(buffer, ret, out, arg);
					ret = send(client, out, ret, MSG_NOSIGNAL);
					free(out);
				}
				free(buffer);
			}
			else
				end = 1;
		}
		else if (ret > 0 && FD_ISSET(client, &rdfs))
		{
			int length;
			ret = ioctl(client, FIONREAD, &length);
			while (length > 0)
			{
				char *buffer;
				buffer = calloc(1, length);
				ret = recv(client, buffer, length, MSG_NOSIGNAL);
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
						outlength = info->sendresp(info->ctx, (char *)out, outlength);
						size += length;
					}
					free(out);
				}
				free(buffer);
			}
		}
		else if (errno != EAGAIN)
		{
			end = 1;
		}
	}
	close(socket);
	close(client);
	return 0;
}

static websocket_t _wsdefaul_config =
{
	.onclose = websocket_close,
	.onping = websocket_pong,
	.type = WS_TEXT,
};
int default_websocket_run(void *arg, int socket, char *protocol, http_message_t *request)
{
	int wssock = _websocket_socket(arg, protocol);

	if (wssock > 0)
	{
		_websocket_main_t info = {.socket = socket, .client = wssock};
		http_client_t *ctl = httpmessage_client(request);
		info.ctx = httpclient_context(ctl);
		info.recvreq = httpclient_addreceiver(ctl, NULL, NULL);
		info.sendresp = httpclient_addsender(ctl, NULL, NULL);

		websocket_init(&_wsdefaul_config);
		_websocket_main(&info);
	}
	else
	{
		close(socket);
	}
	return wssock;
}
