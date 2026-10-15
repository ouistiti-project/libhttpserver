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

#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ecdsa.h>

#include "ouistiti/hash.h"
#include "ouistiti/log.h"

#if OPENSSL_VERSION_NUMBER < 0x30000000
#define DEPRECATED_OPENSSL
#endif

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

static void *SIGN_initkey(const char *key, size_t keylen);
static const char *SIGN_algo(void *ctx);
static int SIGN_sign(void *ctx, const char *in, size_t inlen, char *out, size_t outlen);
static int SIGN_verify(void *ctx, const char *in, size_t inlen, const char *sign, size_t signlen);
static void SIGN_freekey(void *ctx);

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
	.name = "hmac-sha256",
	.nameid = '5',
	.initkey = HMACSHA256_initkey,
	.update = HMAC_update,
	.finish = HMAC_finish,
	.length = HMAC_length,
};

const hash_t *hash_macsha1 = &(const hash_t)
{
	.size = 20,
	.name = "hmac-sha1",
	.nameid = '2',
	.initkey = HMACSHA1_initkey,
	.update = HMAC_update,
	.finish = HMAC_finish,
	.length = HMAC_length,
};

const sign_t *sign_sha256 = &(const sign_t)
{
	.name = "sign-sha256",
	.initkey = SIGN_initkey,
	.algo = SIGN_algo,
	.sign = SIGN_sign,
	.verify = SIGN_verify,
	.freekey = SIGN_freekey,
};

#ifndef DEPRECATED_OPENSSL
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
#ifndef DEPRECATED_OPENSSL
	return EVP_MD_CTX_get_size(ctx);
#else
	return EVP_MD_CTX_size(ctx);
#endif
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
#ifndef DEPRECATED_OPENSSL
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
#ifndef DEPRECATED_OPENSSL
	EVP_MAC_update(ctx, (const unsigned char *)input, len);
#else
	HMAC_Update(ctx, input, len);
#endif
}

static int HMAC_finish(void *ctx, char *output)
{
	size_t len = HASH_MAX_SIZE;
#ifndef DEPRECATED_OPENSSL
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
#ifndef DEPRECATED_OPENSSL
	return EVP_MAC_CTX_get_mac_size(ctx);
#else
	return HMAC_size(ctx);
#endif
}

typedef struct _sign_ctx_s _sign_ctx_t;
struct _sign_ctx_s
{
	EVP_PKEY *pkey;
	int type;
	int private;
	/// size of r and s into raw ECDSA signature
	size_t ecsize;
};

static void *SIGN_initkey(const char *key, size_t keylen)
{
	BIO *bio = BIO_new_mem_buf(key, (int)keylen);
	if (bio == NULL)
		return NULL;
	int private = 1;
	EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
	if (pkey == NULL)
	{
		ERR_clear_error();
		BIO_reset(bio);
		pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
		private = 0;
	}
	BIO_free(bio);
	if (pkey == NULL)
	{
		err("sign: key format not supported");
		ERR_print_errors_fp(stderr);
		return NULL;
	}
#ifndef DEPRECATED_OPENSSL
	int type = EVP_PKEY_get_base_id(pkey);
	int bits = EVP_PKEY_get_bits(pkey);
#else
	int type = EVP_PKEY_base_id(pkey);
	int bits = EVP_PKEY_bits(pkey);
#endif
	if (type != EVP_PKEY_RSA && type != EVP_PKEY_EC)
	{
		err("sign: key type not supported (only RSA and EC)");
		EVP_PKEY_free(pkey);
		return NULL;
	}
	_sign_ctx_t *ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL)
	{
		EVP_PKEY_free(pkey);
		return NULL;
	}
	ctx->pkey = pkey;
	ctx->type = type;
	ctx->private = private;
	ctx->ecsize = ((size_t)bits + 7) / 8;
	return ctx;
}

static const char *SIGN_algo(void *arg)
{
	const _sign_ctx_t *ctx = (const _sign_ctx_t *)arg;
	if (ctx->type == EVP_PKEY_EC)
		return "ES256";
	return "RS256";
}

static int _sign_der2raw(const _sign_ctx_t *ctx, const unsigned char *der, size_t derlen, char *out, size_t outlen)
{
	if (outlen < 2 * ctx->ecsize)
		return -1;
	ECDSA_SIG *ecsig = d2i_ECDSA_SIG(NULL, &der, (long)derlen);
	if (ecsig == NULL)
		return -1;
	const BIGNUM *r = NULL;
	const BIGNUM *s = NULL;
	ECDSA_SIG_get0(ecsig, &r, &s);
	int ret = 2 * (int)ctx->ecsize;
	if (BN_bn2binpad(r, (unsigned char *)out, (int)ctx->ecsize) < 0 ||
		BN_bn2binpad(s, (unsigned char *)out + ctx->ecsize, (int)ctx->ecsize) < 0)
		ret = -1;
	ECDSA_SIG_free(ecsig);
	return ret;
}

static int _sign_raw2der(const _sign_ctx_t *ctx, const char *raw, size_t rawlen, unsigned char **der)
{
	if (rawlen != 2 * ctx->ecsize)
		return -1;
	ECDSA_SIG *ecsig = ECDSA_SIG_new();
	BIGNUM *r = BN_bin2bn((const unsigned char *)raw, (int)ctx->ecsize, NULL);
	BIGNUM *s = BN_bin2bn((const unsigned char *)raw + ctx->ecsize, (int)ctx->ecsize, NULL);
	if (ecsig == NULL || r == NULL || s == NULL || !ECDSA_SIG_set0(ecsig, r, s))
	{
		BN_free(r);
		BN_free(s);
		ECDSA_SIG_free(ecsig);
		return -1;
	}
	/// r and s are now owned by ecsig
	*der = NULL;
	int ret = i2d_ECDSA_SIG(ecsig, der);
	ECDSA_SIG_free(ecsig);
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
	EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
	if (mdctx == NULL)
		return -1;
	int ret = -1;
	unsigned char sign[SIGN_MAX_SIZE];
	size_t signlen = sizeof(sign);
	if (EVP_DigestSignInit(mdctx, NULL, EVP_sha256(), NULL, ctx->pkey) == 1 &&
		EVP_DigestSign(mdctx, sign, &signlen, (const unsigned char *)in, inlen) == 1)
	{
		if (ctx->type == EVP_PKEY_EC)
			ret = _sign_der2raw(ctx, sign, signlen, out, outlen);
		else if (signlen <= outlen)
		{
			memcpy(out, sign, signlen);
			ret = (int)signlen;
		}
	}
	else
		ERR_print_errors_fp(stderr);
	EVP_MD_CTX_free(mdctx);
	return ret;
}

static int SIGN_verify(void *arg, const char *in, size_t inlen, const char *sign, size_t signlen)
{
	_sign_ctx_t *ctx = (_sign_ctx_t *)arg;
	unsigned char *der = NULL;
	const unsigned char *signature = (const unsigned char *)sign;
	if (ctx->type == EVP_PKEY_EC)
	{
		int derlen = _sign_raw2der(ctx, sign, signlen, &der);
		if (derlen < 0)
			return -1;
		signature = der;
		signlen = (size_t)derlen;
	}
	EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
	int ret = -1;
	if (mdctx != NULL &&
		EVP_DigestVerifyInit(mdctx, NULL, EVP_sha256(), NULL, ctx->pkey) == 1 &&
		EVP_DigestVerify(mdctx, signature, signlen, (const unsigned char *)in, inlen) == 1)
		ret = 0;
	ERR_clear_error();
	EVP_MD_CTX_free(mdctx);
	OPENSSL_free(der);
	return ret;
}

static void SIGN_freekey(void *arg)
{
	_sign_ctx_t *ctx = (_sign_ctx_t *)arg;
	EVP_PKEY_free(ctx->pkey);
	free(ctx);
}

static void __attribute__ ((constructor))_init(void)
{
#ifndef DEPRECATED_OPENSSL
	g_mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
#endif
}

static void __attribute__ ((destructor))_finit(void)
{
#ifndef DEPRECATED_OPENSSL
	EVP_MAC_free(g_mac);
#endif
}
