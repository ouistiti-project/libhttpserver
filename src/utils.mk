modules-$(DYNAMIC)+=utils_mod
slib-$(STATIC)+=utils_mod
utils_mod_SOURCES=utils.c
utils_mod_CFLAGS+=-I../include 

utils_mod_CFLAGS-$(DEBUG)+=-g -DDEBUG

