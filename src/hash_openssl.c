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

#include "httpserver/hash.h"
#include "httpserver/log.h"

static void *MD5_init();
static void *SHA1_init();
static void *SHA256_init();
static void *SHA512_init();
static void HASH_update(void *ctx, const char *in, size_t len);
static int HASH_finish(void *ctx, char *out);

static void *HMAC_initkey(const char *key, size_t keylen);
static void HMAC_update(void *ctx, const char *in, size_t len);
static int HMAC_finish(void *ctx, char *out);

const hash_t *hash_md5 = &(const hash_t)
{
	.size = 16,
	.name = "MD5",
	.nameid = '1',
	.init = MD5_init,
	.update = HASH_update,
	.finish = HASH_finish,
};
const hash_t *hash_sha1 = &(const hash_t)
{
	.size = 20,
	.name = "SHA1",
	.nameid = '2',
	.init = SHA1_init,
	.update = HASH_update,
	.finish = HASH_finish,
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
};
const hash_t *hash_sha512 = &(const hash_t)
{
	.size = 64,
	.name = "SHA-512",
	.nameid = '6',
	.init = SHA512_init,
	.update = HASH_update,
	.finish = HASH_finish,
};
const hash_t *hash_macsha256 = &(const hash_t)
{
	.size = 32,
	.name = "HMACSHA256",
	.nameid = '5',
	.initkey = HMAC_initkey,
	.update = HMAC_update,
	.finish = HMAC_finish,
};


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
	EVP_DigestFinal_ex(pctx, out, &len);
	EVP_MD_CTX_destroy(pctx);
}

static void *HMAC_initkey(const char *key, size_t keylen)
{
	HMAC_CTX *pctx = HMAC_CTX_new();
	HMAC_Init_ex(pctx, key, keylen, EVP_sha256(), NULL);
	return pctx;
}

static void HMAC_update(void *ctx, const char *input, size_t len)
{
	HMAC_Update(ctx, input, len);
}

static int HMAC_finish(void *ctx, char *output)
{
	int len = 32;
	HMAC_Final(ctx, output, &len);
	HMAC_CTX_free(ctx);
	return len;
}

