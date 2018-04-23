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

# include <wolfssl/wolfcrypt/md5.h>
# include <wolfssl/wolfcrypt/sha.h>
# include <wolfssl/wolfcrypt/sha256.h>

#include "httpserver/hash.h"

void *MD5_init();
void MD5_update(void *ctx, const char *in, size_t len);
int MD5_finish(void *ctx, char *out);
hash_t *hash_md5 = &(hash_t)
{
	.size = 16,
	.name = "MD5",
	.init = MD5_init,
	.update = MD5_update,
	.finish = MD5_finish,
};

void *SHA1_init();
void SHA1_update(void *ctx, const char *in, size_t len);
int SHA1_finish(void *ctx, char *out);
hash_t *hash_sha1 = &(hash_t)
{
	.size = 20,
	.name = "SHA1",
	.init = SHA1_init,
	.update = SHA1_update,
	.finish = SHA1_finish,
};

hash_t *hash_sha224 = NULL;

void *SHA256_init();
void SHA256_update(void *ctx, const char *in, size_t len);
int SHA256_finish(void *ctx, char *out);
hash_t *hash_sha256 = &(hash_t)
{
	.size = 32,
	.name = "SHA-256",
	.init = SHA1_init,
	.update = SHA1_update,
	.finish = SHA1_finish,
};

hash_t *hash_sha512 = NULL;

void *MD5_init()
{
	Md5 *pctx;
	pctx = calloc(1, sizeof(*pctx));
	wc_InitMd5(pctx);
	return pctx;
}
void MD5_update(void *ctx, const char *in, size_t len)
{
	Md5 *pctx = (Md5 *)ctx;
	wc_Md5Update(pctx, in, len);
}
int MD5_finish(void *ctx, char *out)
{
	Md5 *pctx = (Md5 *)ctx;
	wc_Md5Final(pctx, out);
	free(pctx);
}

void *SHA1_init()
{
	Sha *pctx;
	pctx = calloc(1, sizeof(*pctx));
	wc_InitSha(pctx);
	return pctx;
}
void SHA1_update(void *ctx, const char *in, size_t len)
{
	Sha *pctx = (Sha *)ctx;
	wc_ShaUpdate(pctx, in, len);
}
int SHA1_finish(void *ctx, char *out)
{
	Sha *pctx = (Sha *)ctx;
	wc_ShaFinal(pctx, out);
	free(pctx);
}

void *SHA256_init()
{
	Sha256 *pctx;
	pctx = calloc(1, sizeof(*pctx));
	wc_InitSha256(pctx);
	return pctx;
}
void SHA256_update(void *ctx, const char *in, size_t len)
{
	Sha256 *pctx = (Sha256 *)ctx;
	wc_Sha256Update(pctx, in, len);
}
int SHA256_finish(void *ctx, char *out)
{
	Sha256 *pctx = (Sha256 *)ctx;
	wc_Sha256Final(pctx, out);
	free(pctx);
}
