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
#include <string.h>

#include "ouistiti/hash.h"
#include "ouistiti/log.h"

# include "b64/cencode.h"
# include "b64/cdecode.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

static void *BASE64_init();
static void *BASE64_initurl();
static size_t BASE64_update(void *pstate, char *out, const char *in, size_t inlen);
static size_t BASE64_finish(void *pstate, char *out);
static size_t BASE64_length(void *pstate, size_t inlen);

static int BASE64_encode1(const char *in, size_t inlen, char *out, size_t outlen);
static int BASE64_encode2(const char *in, size_t inlen, char *out, size_t outlen);
static int BASE64_decode(const char *in, size_t inlen, char *out, size_t outlen);
const base64_t *base64 = &(const base64_t)
{
	.encode = &BASE64_encode1,
	.encoder = {
		.init = BASE64_init,
		.update = BASE64_update,
		.finish = BASE64_finish,
	},
	.decode = &BASE64_decode,
};

const base64_t *base64_urlencoding = &(const base64_t)
{
	.encode = &BASE64_encode2,
	.encoder = {
		.init = BASE64_initurl,
		.update = BASE64_update,
		.finish = BASE64_finish,
		.length = BASE64_length,
	},
	.decode = &BASE64_decode,
};

static void *BASE64_init()
{
	base64_encodestate *pstate = calloc(1, sizeof(*pstate));
	base64_init_encodestate(pstate, base64_encoding_std);
	return pstate;
}

static void *BASE64_initurl()
{
	base64_encodestate *pstate = calloc(1, sizeof(*pstate));
	base64_init_encodestate(pstate, base64_encoding_url);
	return pstate;
}

static size_t BASE64_update(void *pstate, char *out, const char *in, size_t inlen)
{
	return base64_encode_block(in, inlen, out, pstate);
}

static size_t BASE64_finish(void *pstate, char *out)
{
	return base64_encode_blockend(out, pstate);
}

static size_t BASE64_length(void *pstate, size_t inlen)
{
	return inlen + ((size_t)(inlen + 2) / 3);
}

static int BASE64_encode1(const char *in, size_t inlen, char *out, size_t outlen)
{
	base64_encodestate state;
	base64_init_encodestate(&state, base64_encoding_std);

	int cnt = base64_encode_block(in, inlen, out, &state);
	cnt += base64_encode_blockend(out + cnt, &state);

	return cnt;
}

static int BASE64_encode2(const char *in, size_t inlen, char *out, size_t outlen)
{
	base64_encodestate state;
	base64_init_encodestate(&state, base64_encoding_url);

	int cnt = base64_encode_block(in, inlen, out, &state);
	cnt += base64_encode_blockend(out + cnt, &state);

	return cnt;
}

static int BASE64_decode(const char *in, size_t inlen, char *out, size_t outlen)
{
	if (outlen < (inlen / 1.5) + 2)
	{
		err("base64: outlen to small for decoding");
		return -1;
	}
	base64_decodestate decoder;
	base64_init_decodestate(&decoder);
	int cnt = base64_decode_block(in, inlen, out, &decoder);
	int i;
	for (i = cnt-1; i < outlen; i++)
	{
		if ((out[i] == '\n') ||
			(out[i] == '\r'))
			out[i] = '\0';
	}
	return cnt;
}

static int BASE32_encode(const char *in, size_t inlen, char *out, size_t outlen)
{
	base64_encodestate state;
	base32_init_encodestate(&state, base32_encoding_std);

	int cnt = base32_encode_block(in, inlen, out, &state);
	cnt += base32_encode_blockend(out + cnt, &state);

	return cnt;
}

static void *BASE32_init()
{
	base64_encodestate *pstate = calloc(1, sizeof(*pstate));
	base32_init_encodestate(pstate, base32_encoding_std);
	return pstate;
}

static size_t BASE32_update(void *pstate, char *out, const char *in, size_t inlen)
{
	return base32_encode_block(in, inlen, out, pstate);
}

static size_t BASE32_finish(void *pstate, char *out)
{
	return base32_encode_blockend(out, pstate);
}

#if 0
static size_t BASE32_length(void *pstate, size_t inlen)
{
	return inlen + ((size_t)(inlen + 4) / 2);
}
#endif

const base64_t *base32 = &(const base64_t)
{
	.encode = &BASE32_encode,
	.encoder = {
		.init = BASE32_init,
		.update = BASE32_update,
		.finish = BASE32_finish,
	},
};
