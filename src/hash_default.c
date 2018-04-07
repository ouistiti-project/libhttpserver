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

#if defined (MD5_RONRIVEST)

# include "md5-c/global.h"
# include "md5-c/md5.h"

void *MD5_init()
{
	MD5_CTX *pctx;
	pctx = calloc(1, sizeof(*pctx));
	MD5Init(pctx);
	return pctx;
}
void MD5_update(void *ctx, const char *in, size_t len)
{
	MD5_CTX *pctx = (MD5_CTX *)ctx;
	MD5Update(pctx, in, len);
}
int MD5_finish(void *ctx, char *out)
{
	MD5_CTX *pctx = (MD5_CTX *)ctx;
	MD5Final(out, pctx);
	free(pctx);
}

#else

# include "md5/md5.h"

void *MD5_init()
{
	md5_state_t *pctx;
	pctx = calloc(1, sizeof(*pctx));
	md5_init(pctx);
	return pctx;
}
void MD5_update(void *ctx, const char *in, size_t len)
{
	md5_state_t *pctx = (md5_state_t *)ctx;
	md5_append(pctx, in, len);
}
int MD5_finish(void *ctx, char *out)
{
	md5_state_t *pctx = (md5_state_t *)ctx;
	md5_finish(pctx, out);
	free(pctx);
}
#endif

typedef struct SHA1_ctx_s{ char *input; int inputlen;} SHA1_ctx;

void *SHA1_init()
{
	SHA1_ctx *pctx;
	pctx = calloc(1, sizeof(*pctx));
	return pctx;
}

void SHA1_update(void *ctx, const char *in, size_t len)
{
	SHA1_ctx *pctx = (SHA1_ctx *)ctx;
	(pctx)->input = in; \
	(pctx)->inputlen = len; \
}
int SHA1_finish(void *ctx, char *out)
{
	SHA1_ctx *pctx = (SHA1_ctx *)ctx;
	memcpy(out, (pctx)->input, (pctx)->inputlen);
	free(pctx);
}


# include "b64/cencode.h"
# include "b64/cdecode.h"

void BASE64_encode(const char *in, int inlen, char *out, int outlen)
{
	base64_encodestate state;
	base64_init_encodestate(&state);
	int cnt = base64_encode_block(in, inlen, out, &state);
	cnt = base64_encode_blockend(out + cnt, &state);
	out[cnt - 1] = '\0';
}

void BASE64_decode(const char *in, int inlen, char *out, int outlen)
{
	base64_decodestate decoder;
	base64_init_decodestate(&decoder);
	int cnt = base64_decode_block(in, inlen, out, &decoder);
	out[cnt - 1] = '\0';
}

