modules-$(MODULES)+=mod_cookie
slib-y+=mod_cookie
mod_cookie_SOURCES+=mod_cookie.c
mod_cookie_CFLAGS+=-I../include/

mod_cookie_CFLAGS-$(DEBUG)+=-g -DDEBUG
