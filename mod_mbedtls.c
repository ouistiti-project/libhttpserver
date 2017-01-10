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

#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/certs.h>
#include <mbedtls/x509.h>
#include <mbedtls/ssl.h>

#include "httpserver.h"

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
} _mod_mbedtls_config_t;

static const char _mod_mbedtls_pers[] = "ssl_pthread_server";
static http_server_config_t mod_mbedtls_config;

void *mod_mbedtls_create(char * crtfile, char *pemfile, char *cachain)
{
	int ret;
	_mod_mbedtls_config_t *config = calloc(1, sizeof(*config));

	mbedtls_x509_crt_init(&config->srvcert);
	mbedtls_x509_crt_init(&config->cachain);

	mbedtls_ssl_config_init(&config->conf);

	mbedtls_ctr_drbg_init(&config->ctr_drbg);
	mbedtls_entropy_init(&config->entropy);

	if (crtfile)
	{
		ret = mbedtls_x509_crt_parse_file(&config->srvcert, (const char *) crtfile);
		if (ret)
			printf("mbedtls_x509_crt_parse_file %d\n", ret);
		mbedtls_pk_init(&config->pkey);
		if (pemfile)
		{
			ret =  mbedtls_pk_parse_keyfile(&config->pkey, (const char *) pemfile, NULL);
			if (ret)
				printf("mbedtls_pk_parse_keyfile %d\n", ret);
		}
		else
		{
			ret =  mbedtls_pk_parse_keyfile(&config->pkey, (const char *) crtfile, NULL);
			if (ret)
				printf("mbedtls_pk_parse_keyfile %d\n", ret);
		}
	}
	if (cachain)
	{
		ret = mbedtls_x509_crt_parse_file(&config->cachain, (const char *) pemfile);
		if (ret)
			printf("mbedtls_x509_crt_parse_file cachain %d\n", ret);
	}

	ret = mbedtls_ctr_drbg_seed(&config->ctr_drbg, mbedtls_entropy_func, &config->entropy,
		(const unsigned char *) _mod_mbedtls_pers, strlen(_mod_mbedtls_pers));
	if (ret)
		printf("mbedtls_ctr_drbg_seed %d\n", ret);

	ret = mbedtls_ssl_config_defaults(&config->conf,
		MBEDTLS_SSL_IS_SERVER,
		MBEDTLS_SSL_TRANSPORT_STREAM,
		MBEDTLS_SSL_PRESET_DEFAULT);
	if (ret)
		printf("mbedtls_ssl_config_defaults %d\n", ret);

	mbedtls_ssl_conf_rng(&config->conf, mbedtls_ctr_drbg_random, &config->ctr_drbg );
	mbedtls_ssl_conf_ca_chain(&config->conf, &config->cachain, NULL);
	ret = mbedtls_ssl_conf_own_cert(&config->conf, &config->srvcert, &config->pkey);
	if (ret)
		printf("mbedtls_ssl_conf_own_cert %d\n", ret);

	return config;
}

void mod_mbedtls_destroy(void *mod)
{
	_mod_mbedtls_config_t *config = (_mod_mbedtls_config_t *)mod;

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

static void *_mod_mbedtls_getctx(http_client_t *ctl, struct sockaddr *addr, int addrsize)
{
	_mod_mbedtls_t *ctx = calloc(1, sizeof(*ctx));
	http_server_config_t *sconfig = httpclient_getconfig(ctl);
	_mod_mbedtls_config_t *config = (_mod_mbedtls_config_t *)(sconfig->callback.generic_ctx);

	mbedtls_ssl_init(&ctx->ssl);
	mbedtls_ssl_setup(&ctx->ssl, &config->conf);
	mbedtls_ssl_set_bio(&ctx->ssl, ctl, (mbedtls_ssl_send_t *)httpclient_send, (mbedtls_ssl_recv_t *)httpclient_recv, NULL);
	return ctx;
}

static void _mod_mbedtls_freectx(void *vctx)
{
}

static int _mod_mbedtls_recv(void *vctx, char *data, int size)
{
	int ret;
	_mod_mbedtls_t *ctx = (_mod_mbedtls_t *)vctx;
	if (!(ctx->state & HANDSHAKE))
	{
		printf("handshake hello\n");
		ctx->state &= RECV_COMPLETE;
		while((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0 )
		{
			if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
				break;
		}
		if (ret)
		{
			printf("handshake error 0x%04x\n", ret);
			return EINCOMPLETE;
		}
		ctx->state |= HANDSHAKE;
		printf("handshake goodbye\n");
	}
	printf("%s hello\n", __FUNCTION__);
	if (ctx->state & RECV_COMPLETE)
		return ESUCCESS;

	ret = mbedtls_ssl_read(&ctx->ssl, data, size);
	if (ret < size)
		ctx->state |= RECV_COMPLETE;
	printf("%s goodbye %d\n", __FUNCTION__, ret);
	return ret;
}

static int _mod_mbedtls_send(void *vctx, char *data, int size)
{
	int ret;
	_mod_mbedtls_t *ctx = (_mod_mbedtls_t *)vctx;
	printf("%s hello\n", __FUNCTION__);
	mbedtls_ssl_write(&ctx->ssl, data, size);
	printf("%s goodbye\n", __FUNCTION__);
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
		.recvreq = _mod_mbedtls_recv,
		.sendresp = _mod_mbedtls_send,
	},
};
