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

# include <mbedtls/md5.h>
# include <mbedtls/sha1.h>
# include <mbedtls/base64.h>

#include "httpserver/hash.h"

void *MD5_init()
{
	mbedtls_md5_context *pctx;
	pctx = calloc(1, sizeof(*pctx));
	mbedtls_md5_init(pctx);
	mbedtls_md5_starts(pctx);
	return pctx;
}
void MD5_update(void *ctx, const char *in, size_t len)
{
	mbedtls_md5_context *pctx = (mbedtls_md5_context *)ctx;
	mbedtls_md5_update(pctx, in, len);
}
int MD5_finish(void *ctx, char *out)
{
	mbedtls_md5_context *pctx = (mbedtls_md5_context *)ctx;
	mbedtls_md5_finish(pctx, out);
	mbedtls_md5_free(pctx);
	free(pctx);
}

void *SHA1_init()
{
	mbedtls_sha1_context *pctx;
	pctx = calloc(1, sizeof(*pctx));
	mbedtls_sha1_init(pctx);
	mbedtls_sha1_starts(pctx);
	return pctx;
}
void SHA1_update(void *ctx, const char *in, size_t len)
{
	mbedtls_sha1_context *pctx = (mbedtls_sha1_context *)ctx;
	mbedtls_sha1_update(pctx, in, len);
}
int SHA1_finish(void *ctx, char *out)
{
	mbedtls_sha1_context *pctx = (mbedtls_sha1_context *)ctx;
	mbedtls_sha1_finish(pctx, out);
	mbedtls_sha1_free(pctx);
	free(pctx);
}

void BASE64_encode(const char *in, int inlen, char *out, int outlen)
{
	size_t cnt = 0;
	mbedtls_base64_encode(out, outlen, &cnt, in, inlen);
}

void BASE64_decode(const char *in, int inlen, char *out, int outlen)
{
	size_t cnt = 0;
	mbedtls_base64_decode(out, outlen, &cnt, in, inlen);
}
