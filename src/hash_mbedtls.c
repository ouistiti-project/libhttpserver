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

# include <mbedtls/version.h>
# include <mbedtls/md5.h>
# include <mbedtls/sha1.h>
# include <mbedtls/sha256.h>
# include <mbedtls/base64.h>
# include <mbedtls/md.h>
# include <mbedtls/pk.h>
# include <mbedtls/asn1.h>
# include <mbedtls/asn1write.h>
# include <mbedtls/bignum.h>
# include <mbedtls/entropy.h>
# include <mbedtls/ctr_drbg.h>

#include "ouistiti/hash.h"
#include "ouistiti/log.h"

#define err(format, ...) fprintf(stderr, "\x1B[31m"format"\x1B[0m\n",  ##__VA_ARGS__)
#define warn(format, ...) fprintf(stderr, "\x1B[35m"format"\x1B[0m\n",  ##__VA_ARGS__)
#ifdef DEBUG
#define dbg(format, ...) fprintf(stderr, "\x1B[32m"format"\x1B[0m\n",  ##__VA_ARGS__)
#else
#define dbg(...)
#endif

static int BASE64_encode1(const char *in, size_t inlen, char *out, size_t outlen);
static int BASE64_encode2(const char *in, size_t inlen, char *out, size_t outlen);
static int BASE64_encode(const char *in, size_t inlen, char *out, size_t outlen);
static int BASE64_decode(const char *in, size_t inlen, char *out, size_t outlen);
static void *MD5_init();
static void MD5_update(void *ctx, const char *in, size_t len);
static int MD5_finish(void *ctx, char *out);
static void *SHA1_init();
static void SHA1_update(void *ctx, const char *in, size_t len);
static int SHA1_finish(void *ctx, char *out);
static void *SHA256_init();
static void SHA256_update(void *ctx, const char *in, size_t len);
static int SHA256_finish(void *ctx, char *out);
static void *HMACSHA256_initkey(const char *key, size_t keylen);
static void *HMACSHA1_initkey(const char *key, size_t keylen);
static void HMAC_update(void *ctx, const char *in, size_t len);
static int HMAC_finish(void *ctx, char *out);
static int HMAC_length(void *ctx);
static void *SIGN_initkey(const char *key, size_t keylen);
static const char *SIGN_algo(void *ctx);
static int SIGN_sign(void *ctx, const char *in, size_t inlen, char *out, size_t outlen);
static int SIGN_verify(void *ctx, const char *in, size_t inlen, const char *sign, size_t signlen);
static void SIGN_freekey(void *ctx);

#ifndef LIBB64
const base64_t *base64 = &(const base64_t)
{
	.encode = &BASE64_encode1,
	.decode = &BASE64_decode,
};
const base64_t *base64_urlencoding = &(const base64_t)
{
	.encode = &BASE64_encode2,
	.decode = &BASE64_decode,
};
#endif
const hash_t *hash_md5 = &(const hash_t)
{
	.size = 16,
	.name = "MD5",
	.nameid = '1',
	.init = &MD5_init,
	.update = &MD5_update,
	.finish = &MD5_finish,
};
const hash_t *hash_sha1 = &(const hash_t)
{
	.size = 20,
	.name = "SHA1",
	.nameid = '2',
	.init = &SHA1_init,
	.update = &SHA1_update,
	.finish = &SHA1_finish,
};
const hash_t *hash_sha224 = NULL;
const hash_t *hash_sha256 = &(const hash_t)
{
	.size = 32,
	.name = "SHA-256",
	.nameid = '5',
	.init = &SHA256_init,
	.update = &SHA256_update,
	.finish = &SHA256_finish,
};
const hash_t *hash_sha512 = NULL;
const hash_t *hash_macsha1 = &(const hash_t)
{
	.size = 20,
	.name = "hmac-sha!",
	.nameid = '2',
	.initkey = &HMACSHA1_initkey,
	.update = &HMAC_update,
	.finish = &HMAC_finish,
	.length = &HMAC_length,
};
const hash_t *hash_macsha256 = &(const hash_t)
{
	.size = 32,
	.name = "hmac-sha256",
	.nameid = '5',
	.initkey = &HMACSHA256_initkey,
	.update = &HMAC_update,
	.finish = &HMAC_finish,
	.length = &HMAC_length,
};
const sign_t *sign_sha256 = &(const sign_t)
{
	.name = "sign-sha256",
	.initkey = &SIGN_initkey,
	.algo = &SIGN_algo,
	.sign = &SIGN_sign,
	.verify = &SIGN_verify,
	.freekey = &SIGN_freekey,
};

static void *MD5_init()
{
	mbedtls_md5_context *pctx;
	pctx = calloc(1, sizeof(*pctx));
#if MBEDTLS_VERSION_NUMBER > 0x02070000
	if (mbedtls_md5_starts_ret(pctx))
	{
		free(pctx);
		return NULL;
	}
#else
	mbedtls_md5_init(pctx);
	mbedtls_md5_starts(pctx);
#endif
	return pctx;
}
static void MD5_update(void *ctx, const char *in, size_t len)
{
	mbedtls_md5_context *pctx = (mbedtls_md5_context *)ctx;
#if MBEDTLS_VERSION_NUMBER > 0x02070000
	mbedtls_md5_update_ret(pctx, in, len);
#else
	mbedtls_md5_update(pctx, in, len);
#endif
}
static int MD5_finish(void *ctx, char *out)
{
	mbedtls_md5_context *pctx = (mbedtls_md5_context *)ctx;
#if MBEDTLS_VERSION_NUMBER > 0x02070000
	if (mbedtls_md5_finish_ret(pctx, out))
	{
		free(pctx);
		return -1;
	}
#else
	mbedtls_md5_finish(pctx, out);
	mbedtls_md5_free(pctx);
#endif
	free(pctx);
	return 0;
}

static void *SHA1_init()
{
#if MBEDTLS_VERSION_NUMBER > 0x02070000
	mbedtls_sha1_context *pctx;
	pctx = calloc(1, sizeof(*pctx));
	if (mbedtls_sha1_starts_ret(pctx))
	{
		free(pctx);
		return NULL;
	}
#else
	mbedtls_sha1_context *pctx;
	pctx = calloc(1, sizeof(*pctx));
	mbedtls_sha1_init(pctx);
	mbedtls_sha1_starts(pctx);
#endif
	return pctx;
}
static void SHA1_update(void *ctx, const char *in, size_t len)
{
	mbedtls_sha1_context *pctx = (mbedtls_sha1_context *)ctx;
#if MBEDTLS_VERSION_NUMBER > 0x02070000
	mbedtls_sha1_update_ret(pctx, in, len);
#else
	mbedtls_sha1_update(pctx, in, len);
#endif
}
static int SHA1_finish(void *ctx, char *out)
{
	mbedtls_sha1_context *pctx = (mbedtls_sha1_context *)ctx;
#if MBEDTLS_VERSION_NUMBER > 0x02070000
	if (mbedtls_sha1_finish_ret(pctx, out))
	{
		free(pctx);
		return -1;
	}
#else
	mbedtls_sha1_finish(pctx, out);
	mbedtls_sha1_free(pctx);
#endif
	free(pctx);
	return 0;
}

static void *SHA256_init()
{
#if MBEDTLS_VERSION_NUMBER > 0x02070000
	mbedtls_sha256_context *pctx;
	pctx = calloc(1, sizeof(*pctx));
	if (mbedtls_sha256_starts_ret(pctx, 0))
	{
		free(pctx);
		return NULL;
	}
#else
	mbedtls_sha256_context *pctx;
	pctx = calloc(1, sizeof(*pctx));
	mbedtls_sha256_init(pctx);
	mbedtls_sha256_starts(pctx, 0);
#endif
	return pctx;
}
static void SHA256_update(void *ctx, const char *in, size_t len)
{
	mbedtls_sha256_context *pctx = (mbedtls_sha256_context *)ctx;
#if MBEDTLS_VERSION_NUMBER > 0x02070000
	mbedtls_sha256_update_ret(pctx, in, len);
#else
	mbedtls_sha256_update(pctx, in, len);
#endif
}
static int SHA256_finish(void *ctx, char *out)
{
	mbedtls_sha256_context *pctx = (mbedtls_sha256_context *)ctx;
#if MBEDTLS_VERSION_NUMBER > 0x02070000
	if (mbedtls_sha256_finish_ret(pctx, out))
	{
		free(pctx);
		return -1;
	}
#else
	mbedtls_sha256_finish(pctx, out);
	mbedtls_sha256_free(pctx);
#endif
	free(pctx);
	return 0;
}

static void *HMAC_initkey(const char *key, size_t keylen, int type)
{
	mbedtls_md_context_t *pctx;
	pctx = calloc(1, sizeof(*pctx));

	const mbedtls_md_info_t *info = mbedtls_md_info_from_type(type);
	mbedtls_md_init(pctx);
	mbedtls_md_setup(pctx, info, 1);
	mbedtls_md_hmac_starts(pctx, key, keylen);
	return pctx;
}

static void *HMACSHA256_initkey(const char *key, size_t keylen)
{
	return HMAC_initkey(key, keylen, MBEDTLS_MD_SHA256);
}

static void *HMACSHA1_initkey(const char *key, size_t keylen)
{
	return HMAC_initkey(key, keylen, MBEDTLS_MD_SHA1);
}

static void HMAC_update(void *ctx, const char *input, size_t len)
{
	mbedtls_md_context_t *pctx = (mbedtls_md_context_t *)ctx;
	mbedtls_md_hmac_update(pctx, input, len);
};
static int HMAC_length(void *ctx)
{
	return mbedtls_md_get_size(ctx);
};
static int HMAC_finish(void *ctx, char *output)
{
	int count;
	mbedtls_md_context_t *pctx = (mbedtls_md_context_t *)ctx;
	mbedtls_md_hmac_finish(pctx, output);
	count = mbedtls_md_get_size(pctx->md_info);
	free(pctx);
	return count;
}

typedef struct _sign_ctx_s _sign_ctx_t;
struct _sign_ctx_s
{
	mbedtls_pk_context pk;
	mbedtls_entropy_context entropy;
	mbedtls_ctr_drbg_context drbg;
	int ec;
	int private;
	/// size of r and s into raw ECDSA signature
	size_t ecsize;
};

static void SIGN_freekey(void *arg)
{
	_sign_ctx_t *ctx = (_sign_ctx_t *)arg;
	mbedtls_pk_free(&ctx->pk);
	mbedtls_ctr_drbg_free(&ctx->drbg);
	mbedtls_entropy_free(&ctx->entropy);
	free(ctx);
}

static void *SIGN_initkey(const char *key, size_t keylen)
{
	_sign_ctx_t *ctx = calloc(1, sizeof(*ctx));
	/// mbedtls requires a PEM buffer terminated by '\0' and its length including '\0'
	unsigned char *pem = calloc(1, keylen + 1);
	if (ctx == NULL || pem == NULL)
	{
		free(ctx);
		free(pem);
		return NULL;
	}
	memcpy(pem, key, keylen);
	mbedtls_pk_init(&ctx->pk);
	mbedtls_entropy_init(&ctx->entropy);
	mbedtls_ctr_drbg_init(&ctx->drbg);
	const char pers[] = "ouistiti_sign";
	int ret = mbedtls_ctr_drbg_seed(&ctx->drbg, mbedtls_entropy_func, &ctx->entropy,
				(const unsigned char *)pers, sizeof(pers) - 1);
	if (ret == 0)
	{
		ctx->private = 1;
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
		ret = mbedtls_pk_parse_key(&ctx->pk, pem, keylen + 1, NULL, 0, mbedtls_ctr_drbg_random, &ctx->drbg);
#else
		ret = mbedtls_pk_parse_key(&ctx->pk, pem, keylen + 1, NULL, 0);
#endif
		if (ret != 0)
		{
			mbedtls_pk_free(&ctx->pk);
			mbedtls_pk_init(&ctx->pk);
			ctx->private = 0;
			ret = mbedtls_pk_parse_public_key(&ctx->pk, pem, keylen + 1);
		}
	}
	free(pem);
	if (ret != 0)
	{
		err("sign: key format not supported (%d)", ret);
		SIGN_freekey(ctx);
		return NULL;
	}
	if (mbedtls_pk_can_do(&ctx->pk, MBEDTLS_PK_ECDSA))
		ctx->ec = 1;
	else if (!mbedtls_pk_can_do(&ctx->pk, MBEDTLS_PK_RSA))
	{
		err("sign: key type not supported (only RSA and EC)");
		SIGN_freekey(ctx);
		return NULL;
	}
	ctx->ecsize = (mbedtls_pk_get_bitlen(&ctx->pk) + 7) / 8;
	return ctx;
}

static const char *SIGN_algo(void *arg)
{
	const _sign_ctx_t *ctx = (const _sign_ctx_t *)arg;
	if (ctx->ec)
		return "ES256";
	return "RS256";
}

static int _sign_der2raw(const _sign_ctx_t *ctx, unsigned char *der, size_t derlen, char *out, size_t outlen)
{
	if (outlen < 2 * ctx->ecsize)
		return -1;
	unsigned char *p = der;
	const unsigned char *end = der + derlen;
	size_t len = 0;
	mbedtls_mpi r, s;
	mbedtls_mpi_init(&r);
	mbedtls_mpi_init(&s);
	int ret = -1;
	if (mbedtls_asn1_get_tag(&p, end, &len, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) == 0 &&
		mbedtls_asn1_get_mpi(&p, end, &r) == 0 &&
		mbedtls_asn1_get_mpi(&p, end, &s) == 0 &&
		mbedtls_mpi_write_binary(&r, (unsigned char *)out, ctx->ecsize) == 0 &&
		mbedtls_mpi_write_binary(&s, (unsigned char *)out + ctx->ecsize, ctx->ecsize) == 0)
		ret = 2 * (int)ctx->ecsize;
	mbedtls_mpi_free(&r);
	mbedtls_mpi_free(&s);
	return ret;
}

/**
 * the DER signature is written at the end of der buffer,
 * the function returns its length and its start into *start
 */
static int _sign_raw2der(const _sign_ctx_t *ctx, const char *raw, size_t rawlen,
			unsigned char *der, size_t dersize, unsigned char **start)
{
	if (rawlen != 2 * ctx->ecsize)
		return -1;
	mbedtls_mpi r, s;
	mbedtls_mpi_init(&r);
	mbedtls_mpi_init(&s);
	unsigned char *p = der + dersize;
	int ret = -1;
	if (mbedtls_mpi_read_binary(&r, (const unsigned char *)raw, ctx->ecsize) == 0 &&
		mbedtls_mpi_read_binary(&s, (const unsigned char *)raw + ctx->ecsize, ctx->ecsize) == 0)
	{
		int len = 0;
		int tmp;
		if ((tmp = mbedtls_asn1_write_mpi(&p, der, &s)) > 0 && (len += tmp) &&
			(tmp = mbedtls_asn1_write_mpi(&p, der, &r)) > 0 && (len += tmp) &&
			(tmp = mbedtls_asn1_write_len(&p, der, len)) > 0 && (len += tmp) &&
			(tmp = mbedtls_asn1_write_tag(&p, der, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE)) > 0)
		{
			ret = len + tmp;
			*start = p;
		}
	}
	mbedtls_mpi_free(&r);
	mbedtls_mpi_free(&s);
	return ret;
}

static int SIGN_sign(void *arg, const char *in, size_t inlen, char *out, size_t outlen)
{
	_sign_ctx_t *ctx = (_sign_ctx_t *)arg;
	if (!ctx->private)
	{
		err("sign: private key required to sign");
		return -1;
	}
	unsigned char hash[32];
	if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const unsigned char *)in, inlen, hash) != 0)
		return -1;
	unsigned char sign[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
	size_t signlen = 0;
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
	int ret = mbedtls_pk_sign(&ctx->pk, MBEDTLS_MD_SHA256, hash, sizeof(hash),
				sign, sizeof(sign), &signlen, mbedtls_ctr_drbg_random, &ctx->drbg);
#else
	int ret = mbedtls_pk_sign(&ctx->pk, MBEDTLS_MD_SHA256, hash, sizeof(hash),
				sign, &signlen, mbedtls_ctr_drbg_random, &ctx->drbg);
#endif
	if (ret != 0)
	{
		err("sign: signature error (%d)", ret);
		return -1;
	}
	if (ctx->ec)
		return _sign_der2raw(ctx, sign, signlen, out, outlen);
	if (signlen > outlen)
		return -1;
	memcpy(out, sign, signlen);
	return (int)signlen;
}

static int SIGN_verify(void *arg, const char *in, size_t inlen, const char *sign, size_t signlen)
{
	_sign_ctx_t *ctx = (_sign_ctx_t *)arg;
	unsigned char hash[32];
	if (mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const unsigned char *)in, inlen, hash) != 0)
		return -1;
	const unsigned char *signature = (const unsigned char *)sign;
	unsigned char der[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
	if (ctx->ec)
	{
		unsigned char *start = NULL;
		int derlen = _sign_raw2der(ctx, sign, signlen, der, sizeof(der), &start);
		if (derlen < 0)
			return -1;
		signature = start;
		signlen = (size_t)derlen;
	}
	if (mbedtls_pk_verify(&ctx->pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), signature, signlen) != 0)
		return -1;
	return 0;
}

#ifndef LIBB64
static int BASE64_encode1(const char *in, size_t inlen, char *out, size_t outlen)
{
	size_t cnt = 0;
	mbedtls_base64_encode(out, outlen, &cnt, in, inlen);
	return cnt;
}

static int BASE64_encode2(const char *in, size_t inlen, char *out, size_t outlen)
{
	size_t cnt = 0;
	mbedtls_base64_encode(out, outlen, &cnt, in, inlen);
	char *offset = out;
	while (*offset != '\0')
	{
		switch (*offset)
		{
			case '\n':
			case '=':
				*offset = '\0';
				cnt = offset - out;
				offset--;
			break;
			case '/':
				*offset = '_';
			break;
			case '+':
				*offset = '-';
			break;
		}
		offset++;
	}

	return cnt;
}

static int BASE64_decode(const char *in, size_t inlen, char *out, size_t outlen)
{
	size_t cnt = 0;
	mbedtls_base64_decode(out, outlen, &cnt, in, inlen);
	return cnt;
}
#endif
