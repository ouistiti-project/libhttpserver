modules-$(MODULES)+=mod_wolfssl
slib-y+=mod_wolfssl
mod_wolfssl_SOURCES+=mod_wolfssl.c
mod_wolfssl_CFLAGS+=-I../include/httpserver 
mod_wolfssl_CFLAGS-$(MODULES)+=-DMODULES
mod_wolfssl_ALIAS-$(MODULES)+=mod_tls.so

mod_wolfssl_CFLAGS-$(DEBUG)+=-g -DDEBUG
