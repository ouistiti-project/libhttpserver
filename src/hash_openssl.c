/*****************************************************************************
 * hash.c: hash functions for modules
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

#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/err.h>

#include "ouistiti/hash.h"
#include "ouistiti/log.h"

static void *MD5_init();
static void *SHA1_init();
static void *SHA256_init();
static void *SHA512_init();
static void HASH_update(void *ctx, const char *in, size_t len);
static int HASH_finish(void *ctx, char *out);
static int HASH_length(void *ctx);

static void *HMACSHA256_initkey(const char *key, size_t keylen);
static void *HMACSHA1_initkey(const char *key, size_t keylen);
static void *HMAC_initkey(const char *digest, const char *key, size_t keylen);
static void HMAC_update(void *ctx, const char *in, size_t len);
static int HMAC_finish(void *ctx, char *out);
static int HMAC_length(void *ctx);

const hash_t *hash_md5 = &(const hash_t)
{
	.size = 16,
	.name = "MD5",
	.nameid = '1',
	.init = MD5_init,
	.update = HASH_update,
	.finish = HASH_finish,
	.length = HASH_length,
};
const hash_t *hash_sha1 = &(const hash_t)
{
	.size = 20,
	.name = "SHA1",
	.nameid = '2',
	.init = SHA1_init,
	.update = HASH_update,
	.finish = HASH_finish,
	.length = HASH_length,
};
const hash_t *hash_sha224 = NULL;
const hash_t *hash_sha256 = &(const hash_t)
{
	.size = 32,
	.name = "SHA-256",
	.nameid = '5',
	.init = SHA256_init,
	.update = HASH_update,
	.finish = HASH_finish,
	.length = HASH_length,
};
const hash_t *hash_sha512 = &(const hash_t)
{
	.size = 64,
	.name = "SHA-512",
	.nameid = '6',
	.init = SHA512_init,
	.update = HASH_update,
	.finish = HASH_finish,
	.length = HASH_length,
};
const hash_t *hash_macsha256 = &(const hash_t)
{
	.size = 32,
	.name = "HMACSHA256",
	.nameid = '5',
	.initkey = HMACSHA256_initkey,
	.update = HMAC_update,
	.finish = HMAC_finish,
	.length = HMAC_length,
};

const hash_t *hash_macsha1 = &(const hash_t)
{
	.size = 20,
	.name = "HMACSHA1",
	.nameid = '5',
	.initkey = HMACSHA1_initkey,
	.update = HMAC_update,
	.finish = HMAC_finish,
	.length = HMAC_length,
};

#if OPENSSL_VERSION_NUMBER >= 0x30000000
EVP_MAC *g_mac = NULL;
#endif

static void *MD5_init()
{
	EVP_MD_CTX *pctx;
	pctx = EVP_MD_CTX_create();
	if (EVP_DigestInit_ex(pctx, EVP_md5(), NULL) != 1)
		return pctx;
	return pctx;
}
static void *SHA1_init()
{
	EVP_MD_CTX *pctx;
	pctx = EVP_MD_CTX_create();
	if (EVP_DigestInit_ex(pctx, EVP_sha1(), NULL) != 1)
		return pctx;
	return pctx;
}
static void *SHA256_init()
{
	EVP_MD_CTX *pctx;
	pctx = EVP_MD_CTX_create();
	if (EVP_DigestInit_ex(pctx, EVP_sha256(), NULL) != 1)
		return pctx;
	return pctx;
}
static void *SHA512_init()
{
	EVP_MD_CTX *pctx;
	pctx = EVP_MD_CTX_create();
	if (EVP_DigestInit_ex(pctx, EVP_sha512(), NULL) != 1)
		return NULL;
	return pctx;
}
static void HASH_update(void *ctx, const char *in, size_t len)
{
	EVP_MD_CTX *pctx = (EVP_MD_CTX *)ctx;
	EVP_DigestUpdate(pctx, in, len);
}
static int HASH_finish(void *ctx, char *out)
{
	EVP_MD_CTX *pctx = (EVP_MD_CTX *)ctx;
	unsigned int len = 0;
	EVP_DigestFinal_ex(pctx, (unsigned char *)out, &len);
	EVP_MD_CTX_destroy(pctx);
	return 0;
}

static int HASH_length(void *ctx)
{
	return EVP_MD_CTX_get_size(ctx);
}

static void *HMACSHA256_initkey(const char *key, size_t keylen)
{
	return HMAC_initkey("SHA256", key, keylen);
}

static void *HMACSHA1_initkey(const char *key, size_t keylen)
{
	return HMAC_initkey("SHA1", key, keylen);
}

static void *HMAC_initkey(const char *digest, const char *key, size_t keylen)
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000
	EVP_MAC_CTX *pctx = NULL;
	pctx = EVP_MAC_CTX_new(g_mac);

	OSSL_PARAM hmac_params[2];
	hmac_params[0] =
			OSSL_PARAM_construct_utf8_string("digest", (char *)digest, 0);
	hmac_params[1] = OSSL_PARAM_construct_end();

	if (pctx && ! EVP_MAC_init(pctx, (const unsigned char *)key, keylen, hmac_params))
	{
		err("hash: initialization error");
		ERR_print_errors_fp(stderr);
		pctx = NULL;
	}
#else
	HMAC_CTX *pctx = HMAC_CTX_new();
	HMAC_Init_ex(pctx, key, keylen, EVP_sha256(), NULL);
#endif
	return pctx;
}

static void HMAC_update(void *ctx, const char *input, size_t len)
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000
	EVP_MAC_update(ctx, (const unsigned char *)input, len);
#else
	HMAC_Update(ctx, input, len);
#endif
}

static int HMAC_finish(void *ctx, char *output)
{
	size_t len = HASH_MAX_SIZE;
#if OPENSSL_VERSION_NUMBER >= 0x30000000
	EVP_MAC_final(ctx, NULL, &len, HASH_MAX_SIZE);
	EVP_MAC_final(ctx, (unsigned char *)output, &len, HASH_MAX_SIZE);
	EVP_MAC_CTX_free(ctx);
#else
	HMAC_Final(ctx, output, (unsigned int *)&len);
	HMAC_CTX_free(ctx);
#endif
	return (int)len;
}

static int HMAC_length(void *ctx)
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000
	return EVP_MAC_CTX_get_mac_size(ctx);
#else
	return HMAC_size((ctx);
#endif
}

static void __attribute__ ((constructor))_init(void)
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000
	g_mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
#endif
}

static void __attribute__ ((destructor))_finit(void)
{
#if OPENSSL_VERSION_NUMBER >= 0x30000000
	EVP_MAC_free(g_mac);
#endif
}
