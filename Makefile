package?=libouistiti
version=3.1

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
subdir-y+=include
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
