lib-$(SHARED)+=utils_mod
slib-$(STATIC)+=utils_mod
utils_mod_SOURCES=utils.c
utils_mod_CFLAGS+=-I../include 
utils_mod_CFLAGS-$(COOKIE)+=-DCOOKIE

utils_mod_CFLAGS-$(DEBUG)+=-g -DDEBUG

