modules-$(MODULES)+=mod_date
slib-y+=mod_date
mod_date_SOURCES+=mod_date.c
mod_date_CFLAGS+=-I../include
mod_date_CFLAGS-$(MODULES)+=-DMODULES

mod_date_CFLAGS-$(DEBUG)+=-g -DDEBUG
