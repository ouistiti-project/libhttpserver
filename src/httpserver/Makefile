lib-y+=httpserver
slib-y+=httpserver
httpserver_SOURCES+=httpserver.c
httpserver_CFLAGS+=-I../../include/httpserver

httpserver_SOURCES-$(VTHREAD)+=vthread.c
httpserver_CFLAGS-$(VTHREAD)+=-DVTHREAD
vthread_CFLAGS+=-DHAVE_PTHREAD -DHAVE_SCHED_YIELD

lib-y+=uri
slib-y+=uri
uri_SOURCES+=uri.c
uri_CFLAGS+=-I../../include/httpserver

httpserver_CFLAGS-$(DEBUG)+=-g -DDEBUG
uri_CFLAGS-$(DEBUG)+=-g -DDEBUG

ifeq ($(WIN32),1)
httpserver_LDFLAGS+=-lws2_32
else
httpserver_LDFLAGS-$(VTHREAD)+=-lpthread
endif
