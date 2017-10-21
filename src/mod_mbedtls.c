/*****************************************************************************
 * mod_mbedtls.c: callbacks and management of https connection
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
#include <string.h>
#include <errno.h>

#if !defined(MBEDTLS_CONFIG_FILE)
#include "mbedtls/config.h"
#else
#include MBEDTLS_CONFIG_FILE
#endif

#if defined(MBEDTLS_PLATFORM_C)
#include <mbedtls/platform.h>
#else
#include <stdio.h>
#include <stdlib.h>
#define mbedtls_free       free
#define mbedtls_calloc    calloc
#endif

#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/certs.h>
#include <mbedtls/x509.h>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#if MBEDTLS_VERSION_MAJOR==2 && MBEDTLS_VERSION_MINOR>=4
#include <mbedtls/net_sockets.h>
#elif MBEDTLS_VERSION_MAJOR==2 && MBEDTLS_VERSION_MINOR==2
#include <mbedtls/net.h>

typedef int (mbedtls_ssl_send_t)(void *, const unsigned char *, size_t);
typedef int (mbedtls_ssl_recv_t)(void *, unsigned char *, size_t);
#else
#error MBEDTLS not found
#endif
#include "httpserver.h"
#include "mod_mbedtls.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

#define HANDSHAKE 0x01
#define RECV_COMPLETE 0x02

typedef struct _mod_mbedtls_s
{
	mbedtls_ssl_context ssl;
	http_client_t *ctl;
	int state;
	http_recv_t recvreq;
	http_send_t sendresp;
	void *ctx;
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

static void *_mod_mbedtls_getctx(void *arg, http_client_t *ctl, struct sockaddr *addr, int addrsize);
static void _mod_mbedtls_freectx(void *vctx);
static int _mod_mbedtls_recv(void *vctx, char *data, int size);
static int _mod_mbedtls_send(void *vctx, char *data, int size);

void *mod_mbedtls_create(http_server_t *server, mod_mbedtls_t *modconfig)
{
	int ret;
	int is_set_pemkey = 0;
	_mod_mbedtls_config_t *config;

	if (!modconfig)
		return NULL;

	config = mbedtls_calloc(1, sizeof(*config));
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
		err("mbedtls_ssl_config_defaults %d\n", ret);

	if (modconfig->crtfile)
	{
		ret = mbedtls_x509_crt_parse_file(&config->srvcert, (const char *) modconfig->crtfile);
		if (ret)
			err("mbedtls_x509_crt_parse_file %d\n", ret);
		else
			is_set_pemkey++;
		mbedtls_pk_init(&config->pkey);
		if (modconfig->pemfile)
		{
			ret =  mbedtls_pk_parse_keyfile(&config->pkey, (const char *) modconfig->pemfile, NULL);
			if (ret)
				err("mbedtls_pk_parse_keyfile pem %d\n", ret);
			else
				is_set_pemkey++;
		}
		else
		{
			ret =  mbedtls_pk_parse_keyfile(&config->pkey, (const char *) modconfig->crtfile, NULL);
			if (ret)
				err("mbedtls_pk_parse_keyfile crt %d\n", ret);
			else
				is_set_pemkey++;
		}
	}
	if (modconfig->cachain)
	{
		ret = mbedtls_x509_crt_parse_file(&config->cachain, (const char *) modconfig->cachain);
		if (ret)
			err("mbedtls_x509_crt_parse_file cachain %d\n", ret);
		else
			mbedtls_ssl_conf_ca_chain(&config->conf, &config->cachain, NULL);
	}

	char *pers = httpserver_INFO(server, "name");
	if (pers)
	{
		ret = mbedtls_ctr_drbg_seed(&config->ctr_drbg, mbedtls_entropy_func, &config->entropy,
			(const unsigned char *) pers, strlen(pers));
		if (ret)
			err("mbedtls_ctr_drbg_seed %d\n", ret);
		else
			mbedtls_ssl_conf_rng(&config->conf, mbedtls_ctr_drbg_random, &config->ctr_drbg );
	}

	if (is_set_pemkey == 2)
	{
		ret = mbedtls_ssl_conf_own_cert(&config->conf, &config->srvcert, &config->pkey);
		if (ret)
			err("mbedtls_ssl_conf_own_cert %d\n", ret);
	}

	if (modconfig->dhmfile)
	{
		mbedtls_dhm_init(&config->dhm);
		ret = mbedtls_dhm_parse_dhmfile(&config->dhm, modconfig->dhmfile);
		if (ret)
			err("mbedtls_dhm_parse_dhmfile %d\n", ret);
	}

	httpserver_addmod(server, _mod_mbedtls_getctx, _mod_mbedtls_freectx, config);
	return config;
}

void mod_mbedtls_destroy(void *mod)
{
	_mod_mbedtls_config_t *config = (_mod_mbedtls_config_t *)mod;

	mbedtls_dhm_free(&config->dhm);
	mbedtls_x509_crt_free(&config->srvcert);
	mbedtls_x509_crt_free(&config->cachain);
	mbedtls_pk_free(&config->pkey);
	mbedtls_ctr_drbg_free(&config->ctr_drbg);
	mbedtls_entropy_free(&config->entropy);
	mbedtls_ssl_config_free(&config->conf);
	mbedtls_free(config);
}

static int _mod_mbedtls_read(void *arg, unsigned char *data, int size)
{
	_mod_mbedtls_t *ctx = (_mod_mbedtls_t *)arg;
	int ret = ctx->recvreq(ctx->ctx, (char *)data, size);
	if (ret == EINCOMPLETE)
		ret = MBEDTLS_ERR_SSL_WANT_READ;
	else if (ret == EREJECT)
		ret = MBEDTLS_ERR_NET_RECV_FAILED;
	return ret;
}

static int _mod_mbedtls_write(void *arg, unsigned char *data, int size)
{
	_mod_mbedtls_t *ctx = (_mod_mbedtls_t *)arg;
	int ret = ctx->sendresp(ctx->ctx, (char *)data, size);
	if (ret == EINCOMPLETE)
	{
		ret = MBEDTLS_ERR_SSL_WANT_WRITE;
	}
	else if (ret == EREJECT)
		ret = MBEDTLS_ERR_NET_SEND_FAILED;
	return ret;
}

static void *_mod_mbedtls_getctx(void *arg, http_client_t *ctl, struct sockaddr *addr, int addrsize)
{
	_mod_mbedtls_t *ctx = calloc(1, sizeof(*ctx));
	_mod_mbedtls_config_t *config = (_mod_mbedtls_config_t *)arg;

	ctx->ctl = ctl;
	ctx->ctx = httpclient_context(ctl);
	ctx->recvreq = httpclient_addreceiver(ctl, _mod_mbedtls_recv, ctx);
	ctx->sendresp = httpclient_addsender(ctl, _mod_mbedtls_send, ctx);

	mbedtls_ssl_init(&ctx->ssl);
	mbedtls_ssl_setup(&ctx->ssl, &config->conf);
	mbedtls_ssl_set_bio(&ctx->ssl, ctx, (mbedtls_ssl_send_t *)_mod_mbedtls_write, (mbedtls_ssl_recv_t *)_mod_mbedtls_read, NULL);
	if (!(ctx->state & HANDSHAKE))
	{
		int ret;
		ctx->state &= ~RECV_COMPLETE;
		dbg("TLS Handshake");
		while((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0 )
		{
			if(ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
				break;
		}
		if (ret == 0)
		{
			ctx->state |= HANDSHAKE;
		}
		else
		{
			warn("TLS Handshake error");
			mbedtls_ssl_free(&ctx->ssl);
			free(ctx);
			ctx = NULL;
		}
	}
	return ctx;
}

static void _mod_mbedtls_freectx(void *vctx)
{
	int ret;
	_mod_mbedtls_t *ctx = (_mod_mbedtls_t *)vctx;
	while ((ret = mbedtls_ssl_close_notify(&ctx->ssl)) == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE);
	dbg("TLS Close");
	mbedtls_ssl_free(&ctx->ssl);
	httpclient_addreceiver(ctx->ctl, ctx->recvreq, ctx->ctx);
	httpclient_addsender(ctx->ctl, ctx->sendresp, ctx->ctx);
	free(ctx);
}

static int _mod_mbedtls_recv(void *vctx, char *data, int size)
{
	int ret;
	_mod_mbedtls_t *ctx = (_mod_mbedtls_t *)vctx;
#ifdef SOCKET_BLOCKING
	ret = MBEDTLS_ERR_SSL_WANT_READ;
	while (ret == MBEDTLS_ERR_SSL_WANT_READ)
#endif
	{
		ret = mbedtls_ssl_read(&ctx->ssl, (unsigned char *)data, size);
	}
	if (ret == MBEDTLS_ERR_SSL_WANT_READ)
		ret = EINCOMPLETE;
	else if (ret < 0)
	{
		ret = EREJECT;
	}
	return ret;
}

static int _mod_mbedtls_send(void *vctx, char *data, int size)
{
	int ret;
	_mod_mbedtls_t *ctx = (_mod_mbedtls_t *)vctx;
	ret = mbedtls_ssl_write(&ctx->ssl, (unsigned char *)data, size);
	if (ret == MBEDTLS_ERR_SSL_WANT_WRITE)
		ret = EINCOMPLETE;
	else if (ret < 0)
		ret = EREJECT;
	return ret;
}
