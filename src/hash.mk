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
libb64_SITE=https://github.com/ouistiti-project/libb64.git
libb64_SITE_METHOD=git

subdir-$(LIBB64_DL)+=src/$(LIBB64_DIR)
libb64_CONFIGURE=echo configure

ifneq ($(findstring y,$(LIBMD5_RONRIVEST)),)
LIBMD5_DIR?=md5-c
endif

LIBSHA1_DIR?=libsha1

download-$(LIBSHA1_DL)+=libsha1
libsha1_SOURCE=libsha1
libsha1_SITE=https://github.com/dottedmag/libsha1.git
libsha1_SITE_METHOD=git

lib-$(SHARED)+=ouihash
slib-$(STATIC)+=ouihash
hostslib-y+=ouihash

ifeq ($(findstring y,$(OPENSSL)$(WOLFSSL)$(MBEDTLS)), )
ouihash_SOURCES:=hash_default.c
endif

ouihash_CFLAGS-$(LIBMD5_RONRIVEST)+=-I$(dirname $(LIBMD5_DIR))
ouihash_CFLAGS-$(LIBMD5_RONRIVEST)+=-DMD5_RONRIVEST
ouihash_SOURCES-$(LIBMD5_RONRIVEST)+=$(LIBMD5_DIR)/md5c.c

ouihash_SOURCES-$(LIBMD5)+=md5/md5.c

ifneq ($(wildcard $(LIBSHA1_DIR)/sha1.c),)
	ouihash_CFLAGS-$(LIBSHA1)+=-I$(LIBSHA1_DIR)/
	ouihash_CFLAGS-$(LIBSHA1)+=-DLIBSHA1
	ouihash_SOURCES-$(LIBSHA1)+=$(LIBSHA1_DIR)/sha1.c
else
	ouihash_LIBS-$(LIBSHA1)+=sha1
endif

# reinitialization of VARIABLES if OPENSSL
ouihash_SOURCES-$(OPENSSL):=hash_openssl.c
ouihash_LIBS-$(OPENSSL):=crypto

# reinitialization of VARIABLES if WOLFSSL
ouihash_SOURCES-$(WOLFSSL):=hash_wolfssl.c
ouihash_LIBS-$(WOLFSSL):=wolfssl

# reinitialization of VARIABLES if MBEDTLS
ouihash_SOURCES-$(MBEDTLS):=hash_mbedtls.c
ouihash_LIBS-$(MBEDTLS):=mbedcrypto

ouihash_CFLAGS+=-I../include/ouistiti

ouihash_SOURCES-$(LIBB64)+=hash_libb64.c

ouihash_CFLAGS-$(DEBUG)+=-g -DDEBUG

ifeq ($(LIBB64),y)
  ifneq ($(wildcard $(LIBB64_DIR)/src/cdecode.c),)
    ouihash_CFLAGS+=-I$(LIBB64_DIR)/include/
    ouihash_LDFLAGS+=-L$(LIBB64_DIR)/src -L$(LIBB64_DIR)/src
    ouihash_SOURCES+=$(LIBB64_DIR)/src/cdecode.c
    ouihash_SOURCES+=$(LIBB64_DIR)/src/cencode.c
  else
    ouihash_LIBS-$(LIBB64)+=b64
  endif

  ouihash_CFLAGS+=-I$(LIBB64_DIR)/include
endif
