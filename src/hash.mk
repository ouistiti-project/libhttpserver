lib-$(SHARED)+=hash_mod
slib-$(STATIC)+=hash_mod

hash_mod_SOURCES-y:=hash_default.c hash_libb64.c
LIBMD5_DIR?=../md5-c
LIBSHA1_DIR?=../libsha1
ifneq ($(wildcard $(LIBMD5_DIR)/md5c.c),)
  libmd5_dir:=$(realpath $(LIBMD5_DIR))
  hash_mod_CFLAGS-y+=-I$(libmd5_dir)/../
  hash_mod_CFLAGS-y+=-DMD5_RONRIVEST
  hash_mod_SOURCES-y+=$(LIBMD5_DIR)/md5c.c
else
  hash_mod_SOURCES-y+=md5/md5.c
endif

ifneq ($(wildcard $(LIBSHA1_DIR)/sha1.c),)
  libsha1_dir:=$(realpath $(LIBSHA1_DIR))
  hash_mod_CFLAGS-y+=-I$(libsha1_dir)/
  hash_mod_CFLAGS-y+=-DLIBSHA1
  hash_mod_SOURCES-y+=$(LIBSHA1_DIR)/sha1.c
else
  hash_mod_LIBS-y+=sha1
endif

# reinitialization of VARIABLES if OPENSSL
hash_mod_SOURCES-$(OPENSSL):=hash_openssl.c
hash_mod_CFLAGS-$(OPENSSL):=-DOPENSSL
hash_mod_LIBS-$(OPENSSL):=crypto

# reinitialization of VARIABLES if WOLFSSL
hash_mod_SOURCES-$(WOLFSSL):=hash_wolfssl.c
hash_mod_CFLAGS-$(WOLFSSL):=-DWOLFSSL
hash_mod_LIBS-$(WOLFSSL):=wolfssl

# reinitialization of VARIABLES if MBEDTLS
hash_mod_SOURCES-$(MBEDTLS):=hash_mbedtls.c
hash_mod_CFLAGS-$(MBEDTLS):=-DMBEDTLS
hash_mod_LIBS-$(MBEDTLS):=mbedtls

hash_mod_CFLAGS+=-I../include

LIBB64:=$(MBEDTLS)
LIBB64:=$(OPENSSL)
LIBB64:=$(WOLFSSL)

hash_mod_SOURCES-$(LIBB64)+=hash_libb64.c
hash_mod_CFLAGS-$(LIBB64)+=-DLIBB64

hash_mod_CFLAGS-$(DEBUG)+=-g -DDEBUG

LIBB64_DIR?=$(srcdir)/libb64
ifeq ($(LIBB64),y)
  ifneq ($(wildcard $(LIBB64_DIR)/src/cdecode.c),)
    libb64_dir:=$(realpath $(LIBB64_DIR))
    hash_mod_CFLAGS+=-I$(libb64_dir)/include/
    hash_mod_LDFLAGS+=-L$(libb64_dir)/src -L$(libb64_dir)/src
    hash_mod_SOURCES+=$(LIBB64_DIR)/src/cdecode.c
    hash_mod_SOURCES+=$(LIBB64_DIR)/src/cencode.c
  else
    hash_mod_LIBS-$(LIBB64)+=b64
  endif

  hash_mod_CFLAGS+=-I$(LIBB64_DIR)/include
endif
