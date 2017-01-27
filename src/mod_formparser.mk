lib-$(DYNAMIC)+=mod_form_urlencoded
slib-$(STATIC)+=mod_form_urlencoded
mod_form_urlencoded_SOURCES+=mod_form_urlencoded.c
mod_form_urlencoded_CFLAGS+=-I../include/httpserver 

mod_form_urlencoded_CFLAGS-$(DEBUG)+=-g -DDEBUG
