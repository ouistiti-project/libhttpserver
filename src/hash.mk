ifeq ($(MBEDTLS),y)
LIBB64:=y
endif
ifeq ($(OPENSSL),y)
LIBB64?=y
endif
ifeq ($(WOLFSSL),y)
LIBB64?=y
endif

LIBB64_DIR?=libb64

download-$(LIBB64_DL)+=libb64
libb64_SOURCE=libb64
#libb64_SITE=https://github.com/ouistiti-project/libb64.git
libb64_SITE=/home/mch/Projects/libb64
libb64_SITE_METHOD=git

subdir-$(LIBB64_DL)+=src/$(LIBB64_DIR)
libb64_CONFIGURE=echo configure

ifeq ($(LIBMD5_RONRIVEST), )
LIBMD5_DIR?=md5-c
endif

LIBSHA1_DIR?=libsha1

lib-$(SHARED)+=hash_mod
slib-$(STATIC)+=hash_mod
hostslib-y+=hash_mod

ifeq ($(findstring y,$(OPENSSL)$(WOLFSSL)$(MBEDTLS)), )
hash_mod_SOURCES:=hash_default.c
endif

hash_mod_CFLAGS-$(LIBMD5_RONRIVEST)+=-I$(dirname $(LIBMD5_DIR))
hash_mod_CFLAGS-$(LIBMD5_RONRIVEST)+=-DMD5_RONRIVEST
hash_mod_SOURCES-$(LIBMD5_RONRIVEST)+=$(LIBMD5_DIR)/md5c.c

hash_mod_SOURCES-$(LIBMD5)+=md5/md5.c

ifneq ($(wildcard $(LIBSHA1_DIR)/sha1.c),)
	hash_mod_CFLAGS-$(LIBSHA1)+=-I$(LIBSHA1_DIR)/
	hash_mod_CFLAGS-$(LIBSHA1)+=-DLIBSHA1
	hash_mod_SOURCES-$(LIBSHA1)+=$(LIBSHA1_DIR)/sha1.c
else
	hash_mod_LIBS-$(LIBSHA1)+=sha1
endif

# reinitialization of VARIABLES if OPENSSL
hash_mod_SOURCES-$(OPENSSL):=hash_openssl.c
hash_mod_LIBS-$(OPENSSL):=crypto

# reinitialization of VARIABLES if WOLFSSL
hash_mod_SOURCES-$(WOLFSSL):=hash_wolfssl.c
hash_mod_LIBS-$(WOLFSSL):=wolfssl

# reinitialization of VARIABLES if MBEDTLS
hash_mod_SOURCES-$(MBEDTLS):=hash_mbedtls.c
hash_mod_LIBS-$(MBEDTLS):=mbedcrypto

hash_mod_CFLAGS+=-I../include

hash_mod_SOURCES-$(LIBB64)+=hash_libb64.c

hash_mod_CFLAGS-$(DEBUG)+=-g -DDEBUG

ifeq ($(LIBB64),y)
  ifneq ($(wildcard $(LIBB64_DIR)/src/cdecode.c),)
    hash_mod_CFLAGS+=-I$(LIBB64_DIR)/include/
    hash_mod_LDFLAGS+=-L$(LIBB64_DIR)/src -L$(LIBB64_DIR)/src
    hash_mod_SOURCES+=$(LIBB64_DIR)/src/cdecode.c
    hash_mod_SOURCES+=$(LIBB64_DIR)/src/cencode.c
  else
    hash_mod_LIBS-$(LIBB64)+=b64
  endif

  hash_mod_CFLAGS+=-I$(LIBB64_DIR)/include
endif
