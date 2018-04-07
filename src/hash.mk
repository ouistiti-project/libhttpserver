SLIBHASH:=$(STATIC)
DLIBHASH:=$(DYNAMIC)

lib-$(DLIBHASH)+=hash_mod
slib-$(SLIBHASH)+=hash_mod
hash_mod_SOURCES-$(MBEDTLS)=hash_mbedtls.c
ifneq ($(MBEDTLS),y)
NOMBEDTLS=y
endif
hash_mod_SOURCES-$(NOMBEDTLS)=hash_default.c
hash_mod_CFLAGS+=-I../include 
hash_mod_CFLAGS-$(MBEDTLS)+=-DMBEDTLS
hash_mod_LIBS-$(MBEDTLS)+=mbedtls

hash_mod_CFLAGS-$(DEBUG)+=-g -DDEBUG

