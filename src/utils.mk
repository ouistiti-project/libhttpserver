ifeq ($(STATIC),y)
SLIB_UTILS:=$(LIBUTILS)
endif

ifeq ($(DYNAMIC),y)
DLIB_UTILS:=$(LIBUTILS)
endif

modules-$(DLIB_UTILS)+=utils_mod
slib-$(SLIB_UTILS)+=utils_mod
utils_mod_SOURCES=utils.c
utils_mod_CFLAGS+=-I../include 

utils_mod_CFLAGS-$(DEBUG)+=-g -DDEBUG

