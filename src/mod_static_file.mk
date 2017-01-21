lib-$(STATIC_FILE)+=mod_static_file
mod_static_file_SOURCES+=mod_static_file.c
mod_static_file_CFLAGS+=-I../include/httpserver 

mod_static_file_CFLAGS-$(DEBUG)+=-g -DDEBUG
