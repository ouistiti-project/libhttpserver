lib-$(DATE)+=mod_date
mod_date_SOURCES+=mod_date.c
mod_date_CFLAGS+=-I../include/httpserver 

mod_date_CFLAGS-$(DEBUG)+=-g -DDEBUG
