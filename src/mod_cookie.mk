modules-$(DYNAMIC)+=mod_cookie
slib-$(STATIC)+=mod_cookie
mod_cookie_SOURCES+=mod_cookie.c
mod_cookie_CFLAGS+=-I../include/

mod_cookie_CFLAGS-$(DEBUG)+=-g -DDEBUG
