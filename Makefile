include scripts.mk

bin-$(TEST)+=httptest
httptest_CFLAGS+=-DHTTPSERVER
httptest_LDFLAGS+=-DHTTPSERVER
httptest_SOURCES+=test.c
httptest_LIBRARY+=httpserver
httptest_LIBRARY-$(MBEDTLS)+=mod_mbedtls
httptest_CFLAGS-$(MBEDTLS)+=-DMBEDTLS
httptest_LIBRARY-$(STATIC_FILE)+=mod_static_file
httptest_CFLAGS-$(STATIC_FILE)+=-DSTATIC_FILE
#bin-$(TEST)+=uritest
#uritest_SOURCES+=test.c

lib-y+=httpserver
slib-y+=httpserver

lib-$(MBEDTLS)+=mod_mbedtls
mod_mbedtls_SOURCES+=mod_mbedtls.c
mod_mbedtls_LIBRARY+=mbedtls mbedx509 mbedcrypto

lib-$(STATIC_FILE)+=mod_static_file
mod_static_file_SOURCES+=mod_static_file.c

httpserver_SOURCES+=httpserver.c vthread.c

vthread_CFLAGS+=-DHAVE_PTHREAD

lib-y+=uri
slib-y+=uri
uri_SOURCES+=uri.c

ifeq ($(CC),mingw32-gcc)
WIN32:=1
endif
ifeq ($(CC),cl.exe)
WIN32:=1
endif

ifeq ($(WIN32),1)
httpserver_LDFLAGS+=-lws2_32
SLIBEXT=lib
DLIBEXT=dll
else
httpserver_LDFLAGS+=-lpthread
SLIBEXT=a
DLIBEXT=so
endif

httpserver_CFLAGS-$(DEBUG)+=-g -DDEBUG
uri_CFLAGS-$(DEBUG)+=-g -DDEBUG
httptest_CFLAGS-$(DEBUG)+=-g -DDEBUG
mod_mbedtls_CFLAGS-$(DEBUG)+=-g -DDEBUG
mod_static_file_CFLAGS-$(DEBUG)+=-g -DDEBUG
