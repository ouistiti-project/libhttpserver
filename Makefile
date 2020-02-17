include scripts.mk

package?=httpserver
version=2.5

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
subdir-$(WOLFSSL)+=src/mod_wolfssl.mk
subdir-$(MBEDTLS)+=src/mod_mbedtls.mk
subdir-$(OPENSSL)+=src/mod_openssl.mk
subdir-$(WEBSOCKET)+=src/mod_websocket.mk
subdir-$(FORMPARSER)+=src/mod_formparser.mk
subdir-$(DATE)+=src/mod_date.mk
subdir-$(COOKIE)+=src/mod_cookie.mk
subdir-$(TEST)+=src/test.mk

subdir-$(WEBSOCKET)+=src/client_websocket.mk

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
