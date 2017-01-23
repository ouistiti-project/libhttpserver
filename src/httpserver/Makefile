ifeq ($(STATIC),y)
SLIB_HTTPSERVER:=y
SLIB_URI:=y
endif

ifeq ($(DYNAMIC),y)
DLIB_HTTPSERVER:=y
DLIB_URI:=y
endif

lib-$(DLIB_HTTPSERVER)+=httpserver
slib-$(SLIB_HTTPSERVER)+=httpserver
httpserver_SOURCES+=httpserver.c
httpserver_CFLAGS+=-I../../include/httpserver

httpserver_SOURCES-$(VTHREAD)+=vthread.c
httpserver_CFLAGS-$(VTHREAD)+=-DVTHREAD
vthread_CFLAGS+=-DHAVE_PTHREAD -DHAVE_SCHED_YIELD

lib-$(DLIB_URI)+=uri
slib-$(SLIB_URI)+=uri
uri_SOURCES+=uri.c
uri_CFLAGS+=-I../../include/httpserver

httpserver_CFLAGS-$(DEBUG)+=-g -DDEBUG
uri_CFLAGS-$(DEBUG)+=-g -DDEBUG

ifeq ($(WIN32),1)
httpserver_LDFLAGS+=-lws2_32
else
httpserver_LDFLAGS-$(VTHREAD)+=-lpthread
endif
