modules-$(MODULES)+=mod_form_urlencoded
slib-y+=mod_form_urlencoded
mod_form_urlencoded_SOURCES+=mod_form_urlencoded.c
mod_form_urlencoded_CFLAGS+=-I../include
mod_form_urlencoded_CFLAGS-$(MODULES)+=-DMODULES

mod_form_urlencoded_CFLAGS-$(DEBUG)+=-g -DDEBUG
