SLIBHASH:=$(STATIC)
DLIBHASH:=$(DYNAMIC)

lib-$(DLIBHASH)+=hash_mod
slib-$(SLIBHASH)+=hash_mod
hash_mod_SOURCES-$(MBEDTLS)+=hash_mbedtls.c
hash_mod_SOURCES-$(WOLFSSL)+=hash_wolfssl.c
ifeq ($(findstring y, $(MBEDTLS) $(WOLFSSL)),)
DEFAULT:=y
endif
ifeq ($(findstring y, $(MBEDTLS)),)
LIBB64:=y
endif
hash_mod_SOURCES-$(DEFAULT)+=hash_default.c
hash_mod_SOURCES-$(LIBB64)+= hash_libb64.c
hash_mod_CFLAGS+=-I../include 

hash_mod_CFLAGS-$(MBEDTLS)+=-DMBEDTLS
hash_mod_LIBS-$(MBEDTLS)+=mbedtls
hash_mod_CFLAGS-$(WOLFSSL)+=-DWOLFSSL
hash_mod_LIBS-$(WOLFSSL)+=wolfssl

hash_mod_LIBS-$(WOLFSSL)+=b64
hash_mod_LIBS-$(NOMBEDTLS)+=b64

hash_mod_CFLAGS-$(DEBUG)+=-g -DDEBUG

LIBB64_DIR?=../libb64

ifeq ($(LIBB64),y)
ifneq ($(wildcard $(LIBB64_DIR)/Makefile),)
subdir-y+=$(LIBB64_DIR)

libb64_dir:=$(realpath $(LIBB64_DIR))
export CFLAGS+=-I$(libb64_dir)/include/
export LDFLAGS+=-L$(libb64_dir)/src -L$(libb64_dir)/src
endif
endif

