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

# include "b64/cencode.h"
# include "b64/cdecode.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

static void BASE64_encode(const char *in, int inlen, char *out, int outlen);
static void BASE64_decode(const char *in, int inlen, char *out, int outlen);
const base64_t *base64 = &(const base64_t)
{
	.encode = BASE64_encode,
	.decode = BASE64_decode,
};

static void BASE64_encode(const char *in, int inlen, char *out, int outlen)
{
	base64_encodestate state;
	base64_init_encodestate(&state);
	int cnt = base64_encode_block(in, inlen, out, &state);
	cnt = base64_encode_blockend(out + cnt, &state);
	int i;
	for (i = cnt-1; i < outlen; i++)
	{
		if ((out[i] == '\n') ||
			(out[i] == '\r'))
			out[i] = '\0';
	}
}

static void BASE64_decode(const char *in, int inlen, char *out, int outlen)
{
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
}

