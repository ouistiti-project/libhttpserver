/*****************************************************************************
 * mod_mbedtls.c: callbacks and management of https connection
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
#include <errno.h>

#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/certs.h>
#include <mbedtls/x509.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#include "httpserver.h"
#include "mod_mbedtls.h"

#define HANDSHAKE 0x01
#define RECV_COMPLETE 0x02

typedef struct _mod_mbedtls_s
{
	mbedtls_ssl_context ssl;
	int state;
} _mod_mbedtls_t;

typedef struct _mod_mbedtls_config_s
{
	mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509_crt srvcert;
    mbedtls_x509_crt cachain;
    mbedtls_pk_context pkey;
    mbedtls_dhm_context dhm;
} _mod_mbedtls_config_t;

static http_server_config_t mod_mbedtls_config;

static void *_mod_mbedtls_getctx(http_client_t *ctl, struct sockaddr *addr, int addrsize);
static void _mod_mbedtls_freectx(void *vctx);
static int _mod_mbedtls_recv(void *vctx, char *data, int size);
static int _mod_mbedtls_send(void *vctx, char *data, int size);

void *mod_mbedtls_create(mod_mbedtls_t *modconfig)
{
	int ret;
	int is_set_pemkey = 0;
	_mod_mbedtls_config_t *config;

	if (!modconfig)
		return NULL;

	config = calloc(1, sizeof(*config));
	mbedtls_x509_crt_init(&config->srvcert);
	mbedtls_x509_crt_init(&config->cachain);

	mbedtls_ssl_config_init(&config->conf);

	mbedtls_ctr_drbg_init(&config->ctr_drbg);
	mbedtls_entropy_init(&config->entropy);

	ret = mbedtls_ssl_config_defaults(&config->conf,
		MBEDTLS_SSL_IS_SERVER,
		MBEDTLS_SSL_TRANSPORT_STREAM,
		MBEDTLS_SSL_PRESET_DEFAULT);
	if (ret)
		printf("mbedtls_ssl_config_defaults %d\n", ret);

	if (modconfig->crtfile)
	{
		ret = mbedtls_x509_crt_parse_file(&config->srvcert, (const char *) modconfig->crtfile);
		if (ret)
			printf("mbedtls_x509_crt_parse_file %d\n", ret);
		else
			is_set_pemkey++;
		mbedtls_pk_init(&config->pkey);
		if (modconfig->pemfile)
		{
			ret =  mbedtls_pk_parse_keyfile(&config->pkey, (const char *) modconfig->pemfile, NULL);
			if (ret)
				printf("mbedtls_pk_parse_keyfile %d\n", ret);
			else
				is_set_pemkey++;
		}
		else
		{
			ret =  mbedtls_pk_parse_keyfile(&config->pkey, (const char *) modconfig->crtfile, NULL);
			if (ret)
				printf("mbedtls_pk_parse_keyfile %d\n", ret);
			else
				is_set_pemkey++;
		}
	}
	if (modconfig->cachain)
	{
		ret = mbedtls_x509_crt_parse_file(&config->cachain, (const char *) modconfig->cachain);
		if (ret)
			printf("mbedtls_x509_crt_parse_file cachain %d\n", ret);
		else
			mbedtls_ssl_conf_ca_chain(&config->conf, &config->cachain, NULL);
	}

	if (modconfig->pers)
	{
		ret = mbedtls_ctr_drbg_seed(&config->ctr_drbg, mbedtls_entropy_func, &config->entropy,
			(const unsigned char *) modconfig->pers, strlen(modconfig->pers));
		if (ret)
			printf("mbedtls_ctr_drbg_seed %d\n", ret);
		else
			mbedtls_ssl_conf_rng(&config->conf, mbedtls_ctr_drbg_random, &config->ctr_drbg );
	}

	if (is_set_pemkey == 2)
	{
		ret = mbedtls_ssl_conf_own_cert(&config->conf, &config->srvcert, &config->pkey);
		if (ret)
			printf("mbedtls_ssl_conf_own_cert %d\n", ret);
	}

	if (modconfig->dhmfile)
	{
		mbedtls_dhm_init(&config->dhm);
		ret = mbedtls_dhm_parse_dhmfile(&config->dhm, modconfig->dhmfile);
		if (ret)
			printf("mbedtls_dhm_parse_dhmfile %d\n", ret);
	}
	return config;
}

void mod_mbedtls_destroy(void *mod)
{
	_mod_mbedtls_config_t *config = (_mod_mbedtls_config_t *)mod;

	mbedtls_dhm_free(&config->dhm);
	mbedtls_x509_crt_free(&config->srvcert);
	mbedtls_pk_free(&config->pkey);
	mbedtls_ctr_drbg_free(&config->ctr_drbg);
	mbedtls_entropy_free(&config->entropy);
	mbedtls_ssl_config_free(&config->conf);
}

void *mod_mbedtls_getmod(void *mod)
{
	mod_mbedtls_config.callback.generic_ctx = mod;
	return (void *)&mod_mbedtls_config;
}

static int _mod_mbedtls_read(void *ctl, unsigned char *data, int size)
{
	int ret = httpclient_recv(ctl, data, size);
	if (ret < 0  && errno == EAGAIN)
	{
		ret = MBEDTLS_ERR_SSL_WANT_READ;
	}
	else if (ret < 0)
		ret = MBEDTLS_ERR_NET_RECV_FAILED;
	return ret;
}

static int _mod_mbedtls_write(void *ctl, unsigned char *data, int size)
{
	int ret = httpclient_send(ctl, data, size);
	if (ret < 0  && errno == EAGAIN)
	{
		ret = MBEDTLS_ERR_SSL_WANT_WRITE;
	}
	else if (ret < 0)
		ret = MBEDTLS_ERR_NET_SEND_FAILED;
	return ret;
}

static void *_mod_mbedtls_getctx(http_client_t *ctl, struct sockaddr *addr, int addrsize)
{
	_mod_mbedtls_t *ctx = calloc(1, sizeof(*ctx));
	http_server_config_t *sconfig = httpclient_getconfig(ctl);
	_mod_mbedtls_config_t *config = (_mod_mbedtls_config_t *)(sconfig->callback.generic_ctx);

	mbedtls_ssl_init(&ctx->ssl);
	mbedtls_ssl_setup(&ctx->ssl, &config->conf);
	mbedtls_ssl_set_bio(&ctx->ssl, ctl, (mbedtls_ssl_send_t *)_mod_mbedtls_write, (mbedtls_ssl_recv_t *)_mod_mbedtls_read, NULL);
	httpclient_addreceiver(ctl, _mod_mbedtls_recv, ctx);
	httpclient_addsender(ctl, _mod_mbedtls_send, ctx);
	return ctx;
}

static void _mod_mbedtls_freectx(void *vctx)
{
	int ret;
	_mod_mbedtls_t *ctx = (_mod_mbedtls_t *)vctx;
	while ((ret = mbedtls_ssl_close_notify(&ctx->ssl)) == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
	mbedtls_ssl_free(&ctx->ssl);
}

static int _mod_mbedtls_recv(void *vctx, char *data, int size)
{
	int ret;
	_mod_mbedtls_t *ctx = (_mod_mbedtls_t *)vctx;
	if (!(ctx->state & HANDSHAKE))
	{
		ctx->state &= RECV_COMPLETE;
		while((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0 )
		{
			if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
				break;
		}
		if (ret)
		{
			return EINCOMPLETE;
		}
		ctx->state |= HANDSHAKE;
	}
	if (ctx->state & RECV_COMPLETE)
		return ESUCCESS;

	do
	{
		ret = mbedtls_ssl_read(&ctx->ssl, data, size);
	}
	while (ret == MBEDTLS_ERR_SSL_WANT_READ);
	if (ret < size)
		ctx->state |= RECV_COMPLETE;
	return ret;
}

static int _mod_mbedtls_send(void *vctx, char *data, int size)
{
	int ret;
	_mod_mbedtls_t *ctx = (_mod_mbedtls_t *)vctx;
	ret = mbedtls_ssl_write(&ctx->ssl, data, size);
	return ret;
}

static http_server_config_t mod_mbedtls_config = 
{
	.addr = NULL,
	.port = 443,
	.callback =
	{
		.getctx = _mod_mbedtls_getctx,
		.freectx = _mod_mbedtls_freectx,
	},
};
