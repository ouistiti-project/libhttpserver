/*****************************************************************************
 * mod_websocket.c: callbacks and management of request method
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

#if defined(MBEDTLS)
# include <mbedtls/sha1.h>
# define SHA1_ctx mbedtls_sha1_context
# define SHA1_init(pctx) \
	do { \
		mbedtls_sha1_init(pctx); \
		mbedtls_sha1_starts(pctx); \
	} while(0)
# define SHA1_update(pctx, in, len) \
	mbedtls_sha1_update(pctx, in, len)
# define SHA1_finish(out, pctx) \
	mbedtls_sha1_finish(pctx, out)
#else
# define SHA1_compute(in, inlen, out, outlen) \
	do { \
		memcpy(out, in, outlen); \
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

static int _mod_websocket_check(_mod_websocket_ctx_t *ctx, char * protocol, http_message_t *request, http_message_t *response)
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
			if (!strncmp(protocol, service, length))
			{
				int socket = httpmessage_lock(response);
				if (ctx->mod->run(ctx->mod->runarg, socket, protocol, request) > 0)
				{
					httpmessage_addheader(response, str_connection, str_upgrade);
					httpmessage_addheader(response, str_upgrade, str_websocket);
					httpmessage_addcontent(response, "none", "", -1);
					httpmessage_result(response, RESULT_101);
					dbg("Result 101");
					ret = ESUCCESS;
				}
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

	char *connection = httpmessage_REQUEST(request, str_connection);
	_mod_websocket_handshake(ctx, request, response);

	if (strcasestr(connection, str_upgrade))
	{
		char *upgrade = httpmessage_REQUEST(request, str_upgrade);

		if (strcasestr(upgrade, str_websocket))
		{
			char *protocol = httpmessage_REQUEST(request, str_protocol);
			if (protocol[0] != '\0')
			{
				httpmessage_addheader(response, str_protocol, protocol);
				ret = _mod_websocket_check(ctx, protocol, request, response);
			}
		}
	}
	if (ret == EREJECT)
	{
		char *protocol = utils_urldecode(httpmessage_REQUEST(request, "uri"));
		ret = _mod_websocket_check(ctx, protocol, request, response);
		free(protocol);
	}
	return ret;
}

static void *_mod_websocket_getctx(void *arg, http_client_t *ctl, struct sockaddr *addr, int addrsize)
{
	_mod_websocket_t *mod = (_mod_websocket_t *)arg;

	_mod_websocket_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->mod = mod;
	httpclient_addconnector(ctl, mod->vhost, websocket_connector, ctx);

	return NULL;
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
	httpserver_addmod(server, _mod_websocket_getctx, NULL, mod);

	return mod;
}

void mod_websocket_destroy(void *data)
{
	free(data);
}
