TARGET?=$(LIBHTTPSERVER_NAME:lib%=%)

ifeq ($(STATIC),y)
SLIB_HTTPSERVER:=y
SLIB_URI:=$(LIBURI_DEPRECATED)
SLIB_WEBSOCKET:=$(LIBWEBSOCKET)
endif

ifeq ($(SHARED),y)
DLIB_HTTPSERVER:=y
DLIB_URI:=$(LIBURI_DEPRECATED)
DLIB_WEBSOCKET:=$(LIBWEBSOCKET)
endif

lib-$(DLIB_HTTPSERVER)+=$(TARGET)
slib-$(SLIB_HTTPSERVER)+=$(TARGET)
hostslib-y+=$(TARGET)
$(TARGET)_SOURCES+=buffer.c
$(TARGET)_SOURCES+=httpmessage.c
$(TARGET)_SOURCES+=httpclient.c
$(TARGET)_SOURCES+=httpserver.c
$(TARGET)_SOURCES+=tcpserver.c
#$(TARGET)_CFLAGS+=-DTCPDUMP
$(TARGET)_CFLAGS+=-fvisibility=hidden
$(TARGET)_CFLAGS+=-I../../include
# SERVER_DEFER_ACCEPT may protect the client connection from some exceptions.
# But it still decrease the speed of the answers.
$(TARGET)_CFLAGS+=-DSERVER_DEFER_ACCEPT
$(TARGET)_PKGCONFIG:=ouistiti

ifeq ($(VTHREAD_TYPE),)
VTHREAD_TYPE:=fork
else ifeq ($(VTHREAD_TYPE),pthread)
$(TARGET)_LIBS-$(VTHREAD)+=pthread
else ifeq ($(VTHREAD_TYPE),threadpool)
$(TARGET)_LIBS-$(VTHREAD)+=pthread
endif
$(TARGET)_SOURCES-$(VTHREAD)+=vthread_$(VTHREAD_TYPE).c
vthread_pthread_CFLAGS+=-DHAVE_SCHED_YIELD
vthread_fork_CFLAGS+=-DHAVE_SCHED_YIELD

lib-$(DLIB_URI)+=uri
slib-$(SLIB_URI)+=uri
hostslib-$(LIBURI_DEPRECATED)+=uri

uri_SOURCES+=uri.c
uri_CFLAGS+=-I../../include

lib-$(DLIB_WEBSOCKET)+=ouibsocket
slib-$(SLIB_WEBSOCKET)+=ouibsocket
hostslib-$(WEBSOCKET)+=ouibsocket
ouibsocket_SOURCES+=websocket.c
ouibsocket_CFLAGS+=-I../../include
ouibsocket_PKGCONFIG:=ouistiti

$(TARGET)_CFLAGS-$(DEBUG)+=-g -DDEBUG
uri_CFLAGS-$(DEBUG)+=-g -DDEBUG
ouibsocket_CFLAGS-$(DEBUG)+=-g -DDEBUG

ifeq ($(WIN32),1)
$(TARGET)_LDFLAGS+=-lws2_32
endif

$(TARGET)_LDFLAGS-$(DEBUG)+=-lrt
