/*****************************************************************************
 * mod_cookie.c: cookie parser
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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "httpserver/log.h"
#include "httpserver/httpserver.h"
#include "httpserver/mod_cookie.h"

typedef struct _mod_cookie_ctx_s _mod_cookie_ctx_t;
typedef struct _mod_cookie_s _mod_cookie_t;

static void *_mod_cookie_getctx(void *arg, http_client_t *ctl, struct sockaddr *addr, int addrsize);
static void _mod_cookie_freectx(void *vctx);
static int _cookie_connector(void *arg, http_message_t *request, http_message_t *response);

static const char str_Cookie[] = "Cookie";
static const char str_SetCookie[] = "Set-Cookie";

struct _mod_cookie_ctx_s
{
	http_client_t *ctl;
	_mod_cookie_t *mod;
};

struct _mod_cookie_s
{
	char *vhost; //Useless only to create a structure
};

void *mod_cookie_create(http_server_t *server, char *vhost, mod_cookie_t *modconfig)
{
	_mod_cookie_t *mod;

	mod = calloc(1, sizeof(*mod));
	mod->vhost = vhost;

	httpserver_addmod(server, _mod_cookie_getctx, _mod_cookie_freectx, mod, str_Cookie);

	return mod;
}

void mod_cookie_destroy(void *arg)
{
	_mod_cookie_t *mod = (_mod_cookie_t *)arg;
	free(mod);
}

static void *_mod_cookie_getctx(void *arg, http_client_t *ctl, struct sockaddr *addr, int addrsize)
{
	_mod_cookie_t *mod = (_mod_cookie_t *)arg;
	_mod_cookie_ctx_t *ctx = calloc(1, sizeof(*ctx));

	ctx->ctl = ctl;
	ctx->mod = mod;

	httpclient_addconnector(ctl, NULL, _cookie_connector, ctx, str_Cookie);

	return ctx;
}

static void _mod_cookie_freectx(void *vctx)
{
	_mod_cookie_ctx_t *ctx = (_mod_cookie_ctx_t *)vctx;
	free(ctx);
}

typedef struct _cookie_s _cookie_t;
struct _cookie_s
{
	char *key;
	char *value;
	_cookie_t *next;
};

typedef struct _cookiesession_s
{
	char *data;
	_cookie_t *first;
	_cookie_t *next;
} _cookiesession_t;

void _cookie_free(_cookiesession_t *cookie)
{
	if (cookie == NULL)
		return;
	if (cookie->data)
		free(cookie->data);
		
	_cookie_t *it = cookie->first;
	while (it)
	{
		_cookie_t *next = it->next;
		free(it);
		it = next;
	}
	free(cookie);
}

static int _cookie_connector(void *arg, http_message_t *request, http_message_t *response)
{
	_mod_cookie_ctx_t *ctx = (_mod_cookie_ctx_t *)arg;
	_mod_cookie_t *mod = ctx->mod;

	const char *data = httpmessage_REQUEST(request, str_Cookie);
	if (data == NULL || data[0] == '\0')
		return EREJECT;

	int size = strlen(data);
	int length = 0;
	_cookiesession_t * cookie = NULL;
	cookie = (_cookiesession_t *)httpmessage_SESSION(request, str_Cookie, NULL);
	if (cookie != NULL)
		_cookie_free(cookie);
	cookie = calloc(1, sizeof(*cookie));
	cookie->data = calloc(size + 1, sizeof(char));
	strcpy(cookie->data, data);
	httpmessage_SESSION(request, str_Cookie, cookie);
	data = cookie->data;

	data += size + 1;
	char *offset = cookie->data;
	char *key = offset;
	char *value = NULL;
	while (offset < data)
	{
		int insert = 0;
		switch (*offset)
		{
			case '\0':
			{
				insert = 1;
			}
			break;
			case '=':
			{
				if (value == NULL)
				{
					//*offset  = '\0';
					value = offset + 1;
				}
			}
			break;
			case '\r':
			{
				*offset = '\0';
			}
			break;
			case ';':
			case '\n':
			{
				*offset = '\0';
				insert = 1;
			}
			break;
		}
		if (insert)
		{
			if (key[0] != '$')
			{
				_cookie_t *it = calloc(1, sizeof(*cookie));
				it->key = key;
				it->value = value;
				it->next = cookie->first;
				if (cookie->next == NULL)
					cookie->next = it;
				cookie->first = it;
			}
			key = offset + 1;
			value = NULL;
		}
		offset++;
	}
	return EREJECT;
}

const char *cookie_get(http_message_t *request, const char *key)
{
	const char *value = NULL;
	_cookiesession_t * cookie = NULL;
	cookie = (_cookiesession_t *)httpmessage_SESSION(request, str_Cookie, NULL);
	if (cookie != NULL)
	{
		_cookie_t *it = cookie->next;
		do
		{
			it = it->next;
			if (it == NULL)
				it = cookie->first;
			if (it == NULL)
				break;
			if (key == NULL || !strncmp(it->key, key, strlen(key)))
			{
				//value = cookie->value;
				value = it->key;
				cookie->next = it;
				break;
			}
			it = it->next;
		}
		while (it != cookie->next);
	}
	return value;
}

void cookie_set(http_message_t *response, const char *key, char *value)
{
	char *keyvalue = malloc(strlen(key) + 1 + strlen(value) + 1);
	sprintf(keyvalue, "%s=%s", key, value);
	httpmessage_addheader(response, str_SetCookie, keyvalue);
	free(keyvalue);
}
