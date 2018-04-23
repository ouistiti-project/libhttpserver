lib-$(DYNAMIC)+=hash_mod
slib-$(STATIC)+=hash_mod
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
hash_mod_CFLAGS+=-I$(libb64_dir)/include/
hash_mod_LDFLAGS+=-L$(libb64_dir)/src -L$(libb64_dir)/src
endif
endif

LIBMD5_DIR?=../md5-c
ifeq ($(DEFAULT), y)
ifneq ($(wildcard $(LIBMD5_DIR)/md5c.c),)
subdir-y+=$(LIBMD5_DIR)
libmd5_dir:=$(realpath $(LIBMD5_DIR))
hash_mod_CFLAGS+=-I$(libmd5_dir)/../
hash_mod_CFLAGS+=-DMD5_RONRIVEST
hash_mod_SOURCES+=$(libmd5_dir)/md5c.c
else
hash_mod_SOURCES+=md5/md5.c
endif
endif
