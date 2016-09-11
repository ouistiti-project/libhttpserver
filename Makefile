include scripts.mk

bin-$(TEST)+=httpserver
ifneq ($(TEST),y)
lib-y+=httpserver
slib-y+=httpserver
else
httpserver_CFLAGS+=-DTEST
endif
httpserver_SOURCES+=httpserver.c vthread.c

bin-$(TEST)+=uri
ifneq ($(TEST),y)
lib-y+=uri
slib-y+=uri
else
uri_CFLAGS+=-DTEST
endif
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

