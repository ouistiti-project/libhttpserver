include scripts.mk

httpserver_CFLAGS-$(DEBUG)+=-g -DDEBUG
uri_CFLAGS-$(DEBUG)+=-g -DDEBUG
httptest_CFLAGS-$(DEBUG)+=-g -DDEBUG

bin-$(TEST)+=httptest
httptest_CFLAGS+=-DHTTPSERVER
httptest_LDFLAGS+=-DHTTPSERVER
httptest_SOURCES+=test.c
httptest_LIBRARY+=httpserver
#bin-$(TEST)+=uritest
#uritest_SOURCES+=test.c

lib-y+=httpserver
slib-y+=httpserver

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

