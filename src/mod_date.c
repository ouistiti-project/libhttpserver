/*****************************************************************************
 * mod_date.c: callbacks and management of connection
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
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "log.h"
#include "httpserver.h"
#include "mod_date.h"

typedef struct _mod_date_config_s _mod_date_config_t;
typedef struct _mod_date_s _mod_date_t;

static http_server_config_t mod_date_config;

static void *_mod_date_getctx(void *arg, http_client_t *ctl, struct sockaddr *addr, int addrsize);
static void _mod_date_freectx(void *vctx);
static int _mod_date_recv(void *vctx, char *data, int size);
static int _mod_date_send(void *vctx, char *data, int size);
static int _date_connector(void *arg, http_message_t *request, http_message_t *response);

static const char str_date[] = "date";

struct _mod_date_s
{
	_mod_date_config_t *config;
	http_client_t *ctl;
};

struct _mod_date_config_s
{
	char *header_key;
	char *header_value;
};

void *mod_date_create(http_server_t *server)
{
	_mod_date_config_t *config;

	config = calloc(1, sizeof(*config));

	config->header_key = calloc(1, sizeof("Date") + 1);
	strcpy(config->header_key, "Date");

	config->header_value = calloc(1, 30);

	httpserver_addconnector(server, _date_connector, config, CONNECTOR_DOCFILTER, str_date);

	return config;
}

void mod_date_destroy(void *mod)
{
	_mod_date_config_t *config = (_mod_date_config_t *)mod;
	free(config->header_key);
	free(config->header_value);
	free(config);
}

static int _date_connector(void *arg, http_message_t *request, http_message_t *response)
{
	_mod_date_t *ctx = (_mod_date_t *)arg;
	_mod_date_config_t *config = ctx->config;

	time_t t;
	struct tm *tmp;

	t = time(NULL);
	tmp = gmtime(&t);
	strftime(config->header_value, 30, "%a, %d %b %Y %T GMT", tmp);

	httpmessage_addheader(response, config->header_key, config->header_value);
	/* reject the request to allow other connectors to set the response */
	return EREJECT;
}

const module_t mod_date =
{
	.name = str_date,
	.create = (module_create_t)mod_date_create,
	.destroy = mod_datte_destroy,
};
#ifdef MODULES
extern module_t mod_info __attribute__ ((weak, alias ("mod_date")));
#endif
