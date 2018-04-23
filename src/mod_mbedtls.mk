modules-$(MODULES)+=mod_mbedtls
slib-y+=mod_mbedtls
mod_mbedtls_SOURCES+=mod_mbedtls.c
#mod_mbedtls_LIBRARY+=mbedtls mbedx509 mbedcrypto
mod_mbedtls_CFLAGS+=-I../include

mod_mbedtls_CFLAGS-$(DEBUG)+=-g -DDEBUG
