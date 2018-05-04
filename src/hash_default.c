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

#include "httpserver/hash.h"
#include "httpserver/log.h"

static void *MD5_init();
static void MD5_update(void *ctx, const char *in, size_t len);
static int MD5_finish(void *ctx, char *out);
static void *SHA1_init();
static void SHA1_update(void *ctx, const char *in, size_t len);
static int SHA1_finish(void *ctx, char *out);

const hash_t *hash_md5 = &(const hash_t)
{
	.size = 16,
	.name = "MD5",
	.init = MD5_init,
	.update = MD5_update,
	.finish = MD5_finish,
};

const hash_t *hash_sha1 = &(const hash_t)
{
	.size = 20,
	.name = "SHA1",
	.init = SHA1_init,
	.update = SHA1_update,
	.finish = SHA1_finish,
};
const hash_t *hash_sha224 = NULL;
const hash_t *hash_sha256 = NULL;
const hash_t *hash_sha512 = NULL;

#if defined (MD5_RONRIVEST)

# include "md5-c/global.h"
# include "md5-c/md5.h"

static void *MD5_init()
{
	MD5_CTX *pctx;
	pctx = calloc(1, sizeof(*pctx));
	MD5Init(pctx);
	return pctx;
}
static void MD5_update(void *ctx, const char *in, size_t len)
{
	MD5_CTX *pctx = (MD5_CTX *)ctx;
	MD5Update(pctx, in, len);
}
static int MD5_finish(void *ctx, char *out)
{
	MD5_CTX *pctx = (MD5_CTX *)ctx;
	MD5Final(out, pctx);
	free(pctx);
}

#else

# include "md5/md5.h"

static void *MD5_init()
{
	md5_state_t *pctx;
	pctx = calloc(1, sizeof(*pctx));
	md5_init(pctx);
	return pctx;
}
static void MD5_update(void *ctx, const char *in, size_t len)
{
	md5_state_t *pctx = (md5_state_t *)ctx;
	md5_append(pctx, in, len);
}
static int MD5_finish(void *ctx, char *out)
{
	md5_state_t *pctx = (md5_state_t *)ctx;
	md5_finish(pctx, out);
	free(pctx);
}
#endif

#ifdef LIBSHA1
#include "libsha1.h"
static void *SHA1_init()
{
	sha1_ctx *pctx;
	pctx = calloc(1, sizeof(*pctx));
	sha1_begin(pctx);
	return pctx;
}
static void SHA1_update(void *ctx, const char *in, size_t len)
{
	sha1_ctx *pctx = (sha1_ctx *)ctx;
	sha1_hash(in, len, pctx);
}
static int SHA1_finish(void *ctx, char *out)
{
	sha1_ctx *pctx = (sha1_ctx *)ctx;
	sha1_end(out, pctx);
	free(pctx);
}
#else
static void *SHA1_init()
{
	return NULL + 1;
}
static void SHA1_update(void *ctx, const char *in, size_t len)
{
}
static int SHA1_finish(void *ctx, char *out)
{
}
#endif

