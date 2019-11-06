lib-$(SHARED)+=ouiutils
slib-$(STATIC)+=ouiutils
ouiutils_SOURCES=utils.c
ouiutils_CFLAGS+=-I../include

ouiutils_CFLAGS-$(DEBUG)+=-g -DDEBUG

