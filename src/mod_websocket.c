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
/**
 * CAUTION!!!
 * Websocket module is not able to run on TLS socket if VTHREAD is not
 * activated.
 */
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
#include <signal.h>
#include <sys/wait.h>

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

#include "httpserver/log.h"
#include "httpserver/httpserver.h"
#include "httpserver/uri.h"
#include "httpserver/mod_websocket.h"
#include "httpserver/utils.h"
#include "httpserver/websocket.h"

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
	char *filepath;
	int socket;
	pid_t pid;
};

static const char str_connection[] = "Connection";
static const char str_upgrade[] = "Upgrade";
static const char str_websocket[] = "websocket";
static const char str_protocol[] = "Sec-WebSocket-Protocol";
static const char str_accept[] = "Sec-WebSocket-Accept";
static const char str_key[] = "Sec-WebSocket-Key";

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

static int _checkname(mod_websocket_t *config, char *pathname)
{
	if (pathname[0] == '.')
	{
		return  EREJECT;
	}
	if (utils_searchexp(pathname, config->deny) == ESUCCESS &&
		utils_searchexp(pathname, config->allow) != ESUCCESS)
	{
		return  EREJECT;
	}
	return ESUCCESS;
}

static int websocket_connector(void *arg, http_message_t *request, http_message_t *response)
{
	int ret = EREJECT;
	_mod_websocket_ctx_t *ctx = (_mod_websocket_ctx_t *)arg;

	if (ctx->filepath == NULL)
	{
		char *connection = httpmessage_REQUEST(request, str_connection);

		if (strcasestr(connection, str_upgrade))
		{
			char *upgrade = httpmessage_REQUEST(request, str_upgrade);

			if (strcasestr(upgrade, str_websocket))
			{
				char *protocol = NULL;
				char *uri = utils_urldecode(httpmessage_REQUEST(request, "uri"));
				if (_checkname(ctx->mod->config, uri) == ESUCCESS)
				{
					struct stat filestat;
					char *filepath = utils_buildpath(ctx->mod->config->docroot, uri, "", "", &filestat);

					protocol = httpmessage_REQUEST(request, str_protocol);
					if (protocol == NULL || protocol[0] == '\0')
					{
						protocol = basename(uri);
					}
					if (filepath == NULL)
					{
						filepath = utils_buildpath(ctx->mod->config->docroot, protocol, "", "", &filestat);
					}
					else if (S_ISDIR(filestat.st_mode))
					{
						filepath = utils_buildpath(ctx->mod->config->docroot, uri, protocol, "", &filestat);
					}
					if (filepath && S_ISSOCK(filestat.st_mode))
						ctx->filepath = filepath;
					else if (filepath)
					{
						free(filepath);
					}
				}
				if (ctx->filepath)
				{
					if (protocol != NULL)
						httpmessage_addheader(response, str_protocol, protocol);

					_mod_websocket_handshake(ctx, request, response);
					httpmessage_addheader(response, str_connection, (char *)str_upgrade);
					httpmessage_addheader(response, str_upgrade, (char *)str_websocket);
					httpmessage_addcontent(response, "none", "", -1);
					httpmessage_result(response, RESULT_101);
					dbg("result 101");
					ret = ECONTINUE;
				}
				else
				{
					httpmessage_result(response, RESULT_404);
					ret = ESUCCESS;
				}
				free(uri);
			}
		}
	}
	else
	{
		ctx->socket = httpmessage_lock(response);
		ctx->pid = ctx->mod->run(ctx->mod->runarg, ctx->socket, ctx->filepath, request);
		free(ctx->filepath);
		ctx->filepath = NULL;

		ret = ESUCCESS;
	}
	return ret;
}

static void *_mod_websocket_getctx(void *arg, http_client_t *ctl, struct sockaddr *addr, int addrsize)
{
	_mod_websocket_t *mod = (_mod_websocket_t *)arg;

	_mod_websocket_ctx_t *ctx = calloc(1, sizeof(*ctx));
	ctx->mod = mod;
	httpclient_addconnector(ctl, mod->vhost, websocket_connector, ctx, str_websocket);

	return ctx;
}

static void _mod_websocket_freectx(void *arg)
{
	_mod_websocket_ctx_t *ctx = (_mod_websocket_ctx_t *)arg;

	if (ctx->pid > 0)
	{
#ifdef VTHREAD
		dbg("websocket: waitpid");
		waitpid(ctx->pid, NULL, 0);
		warn("websocket: freectx");
#else
		/**
		 * ignore SIGCHLD allows the child to die without to create a z$
		 */
		struct sigaction action;
		action.sa_flags = SA_SIGINFO;
		sigemptyset(&action.sa_mask);
		action.sa_handler = SIG_IGN;
		sigaction(SIGCHLD, &action, NULL);
#endif
	}
	free(ctx);
}

void *mod_websocket_create(http_server_t *server, char *vhost, void *config, mod_websocket_run_t run, void *runarg)
{
	_mod_websocket_t *mod = calloc(1, sizeof(*mod));

	mod->vhost = vhost;
	mod->config = config;
	mod->run = run;
	mod->runarg = runarg;
	httpserver_addmod(server, _mod_websocket_getctx, _mod_websocket_freectx, mod, str_websocket);
	warn("websocket support %s", mod->config->docroot);
	return mod;
}

void mod_websocket_destroy(void *data)
{
	free(data);
}

static int _websocket_socket(char *filepath)
{
	int sock;
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, filepath, sizeof(addr.sun_path) - 1);

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
		int ret;
		fd_set rdfs;
		int maxfd = socket;
		FD_ZERO(&rdfs);
		FD_SET(socket, &rdfs);
		maxfd = (maxfd > client)?maxfd:client;
		FD_SET(client, &rdfs);

		ret = select(maxfd + 1, &rdfs, NULL, NULL, NULL);
		if (ret > 0 && FD_ISSET(socket, &rdfs))
		{
			int length = 0;

			ret = ioctl(socket, FIONREAD, &length);
			if (ret == 0 && length > 0)
			{
				char *buffer = calloc(1, length);
				ret = info->recvreq(info->ctx, (char *)buffer, length);
				//char buffer[64];
				//ret = read(socket, buffer, 63);
				if (ret > 0)
				{
					char *out = calloc(1, length);
					ret = websocket_unframed(buffer, ret, out, arg);
					ret = send(client, out, ret, MSG_NOSIGNAL);
					free(out);
				}
				else if (ret < 0)
				{
					warn("websocket: %d %d error %s", ret, length, strerror(errno));
					end = 1;
				}
				free(buffer);
			}
			else
			{
				char buffer[64];
				ret = read(socket, buffer, 63);
				warn("websocket: %d %d error %s", ret, length, strerror(errno));
				end = 1;
			}
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
						if (outlength == EINCOMPLETE)
							continue;
						if (outlength == EREJECT)
							break;
						size += length;
					}
					free(out);
				}
				free(buffer);
			}
		}
		else if (errno != EAGAIN)
		{
			warn("websocket: error %s", strerror(errno));
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
int default_websocket_run(void *arg, int socket, char *filepath, http_message_t *request)
{
	pid_t pid;
	int wssock = _websocket_socket(filepath);

	if (wssock > 0)
	{
		_websocket_main_t info = {.socket = socket, .client = wssock};
		http_client_t *ctl = httpmessage_client(request);
		info.ctx = httpclient_context(ctl);
		info.recvreq = httpclient_addreceiver(ctl, NULL, NULL);
		info.sendresp = httpclient_addsender(ctl, NULL, NULL);

		websocket_init(&_wsdefaul_config);

		if ((pid = fork()) == 0)
		{
			_websocket_main(&info);
			err("websocket: process died");
			exit(0);
		}
		close(wssock);
	}
	return pid;
}
