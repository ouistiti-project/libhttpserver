/*****************************************************************************
 * utils.h: http utils  for modules
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

#ifndef __HASH_H__
#define __HASH_H__

typedef struct base64_s base64_t;
struct base64_s
{
	int (*encode)(const char *in, size_t inlen, char *out, size_t outlen);
	struct{
		void *(*init)();
		size_t (*update)(void *ctx, char *out, const char *data, size_t length);
		size_t (*finish)(void *ctx, char *out);
		size_t (*length)(void *ctx, size_t inlen);
	} encoder;
	int (*decode)(const char *in, size_t inlen, char *out, size_t outlen);
	struct{
		void *(*init)();
		size_t (*update)(void *ctx, char *out, const char *data, size_t length);
		size_t (*finish)(void *ctx, char *out);
		size_t (*length)(void *ctx, size_t inlen);
	} decoder;
};

#ifdef SHA512
#define HASH_MAX_SIZE 64
#else
#define HASH_MAX_SIZE 32
#endif

extern const base64_t *base64;
extern const base64_t *base64_urlencoding;
extern const base64_t *base32;

typedef struct hash_s hash_t;
struct hash_s
{
	const int size;
	/// name used by digest authentication
	const char *name;
	/// nameid used for unix password storage
	const int nameid;
	void *(*init)();
	void *(*initkey)(const char *key, size_t keylen);
	void (*update)(void *ctx, const char *in, size_t len);
	int (*finish)(void *ctx, char *out);
	int (*length)(void *ctx);
};

extern const hash_t *hash_md5;
extern const hash_t *hash_sha1;
extern const hash_t *hash_sha224;
extern const hash_t *hash_sha256;
extern const hash_t *hash_sha512;
extern const hash_t *hash_macsha256;
extern const hash_t *hash_macsha1;

#endif
