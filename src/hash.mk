SLIBHASH:=$(STATIC)
DLIBHASH:=$(DYNAMIC)

lib-$(DLIBHASH)+=hash_mod
slib-$(SLIBHASH)+=hash_mod
hash_mod_SOURCES-$(MBEDTLS)=hash_mbedtls.c
hash_mod_SOURCES-$(WOLFSSL)=hash_wolfssl.c hash_libb64.c
ifneq ($(MBEDTLS),y)
ifneq ($(WOLFSSL),y)
NOMBEDTLS=y
endif
endif
hash_mod_SOURCES-$(NOMBEDTLS)=hash_default.c hash_libb64.c
hash_mod_CFLAGS+=-I../include 

hash_mod_CFLAGS-$(MBEDTLS)+=-DMBEDTLS
hash_mod_LIBS-$(MBEDTLS)+=mbedtls
hash_mod_CFLAGS-$(WOLFSSL)+=-DWOLFSSL
hash_mod_LIBS-$(WOLFSSL)+=wolfssl

hash_mod_CFLAGS-$(DEBUG)+=-g -DDEBUG

