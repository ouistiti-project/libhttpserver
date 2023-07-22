package?=libouistiti
version=3.3

pkgconfig-y+=ouistiti
includedir=$(prefix)/include/$(package)
ifneq ($(wildcard version.h),)
include-y+=config.h
include-y+=version.h
endif

include scripts.mk

ifeq ($(VTHREAD_TYPE),pthread)
USE_PTHREAD=y
endif

LIBHASH=y

LIBHTTPSERVER_NAME?=$(package)
export LIBHTTPSERVER_NAME

ifneq ($(MBEDTLS),)
LIBUTILS=y
endif
ifneq ($(WEBSOCKET),)
LIBUTILS=y
LIBWEBSOCKET=y
endif #WEBSOCKET
export LIBUTILS LIBWEBSOCKET

subdir-y+=src/httpserver
subdir-y+=include/ouistiti
subdir-$(LIBUTILS)+=src/utils.mk
subdir-$(LIBHASH)+=src/hash.mk
subdir-$(TEST)+=src/test.mk

ifeq ($(CC),mingw32-gcc)
WIN32:=1
endif
ifeq ($(CC),cl.exe)
WIN32:=1
endif

ifeq ($(WIN32),1)
SLIBEXT=lib
DLIBEXT=dll
else
SLIBEXT=a
DLIBEXT=so
endif
