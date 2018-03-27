modules-$(DYNAMIC)+=mod_wolftls
slib-$(STATIC)+=mod_wolftls
mod_wolftls_SOURCES+=mod_wolftls.c
mod_wolftls_CFLAGS+=-I../include/httpserver 

mod_wolftls_CFLAGS-$(DEBUG)+=-g -DDEBUG
