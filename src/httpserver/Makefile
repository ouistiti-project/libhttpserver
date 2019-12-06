TARGET?=$(LIBHTTPSERVER_NAME)

ifeq ($(STATIC),y)
SLIB_HTTPSERVER:=y
SLIB_URI:=$(LIBURI)
SLIB_WEBSOCKET:=$(LIBWEBSOCKET)
endif

ifeq ($(SHARED),y)
DLIB_HTTPSERVER:=y
DLIB_URI:=$(LIBURI)
DLIB_WEBSOCKET:=$(LIBWEBSOCKET)
endif

lib-$(DLIB_HTTPSERVER)+=$(TARGET)
slib-$(SLIB_HTTPSERVER)+=$(TARGET)
hostslib-y+=$(TARGET)
$(TARGET)_SOURCES+=httpserver.c tcpserver.c
$(TARGET)_CFLAGS+=-I../../include/httpserver
# SERVER_DEFER_ACCEPT may protect the client connection from some exceptions.
# But it still decrease the speed of the answers.
$(TARGET)_CFLAGS-$(IPV6)+=-DIPV6
$(TARGET)_CFLAGS+=-DSERVER_DEFER_ACCEPT
$(TARGET)_CFLAGS+=-DUSE_POLL
$(TARGET)_CFLAGS-$(MBEDTLS)+=-DTLS
$(TARGET)_CFLAGS-$(WOLFSSL)+=-DTLS
$(TARGET)_CFLAGS-$(OPENSSL)+=-DTLS

ifneq ($(MAX_SERVERS),)
$(TARGET)_CFLAGS+=-DMAXSERVERS=$(MAX_SERVERS)
endif

ifneq ($(MAXCHUNKS_HEADER),)
$(TARGET)_CFLAGS+=-DMAXCHUNKS_HEADER=$(MAXCHUNKS_HEADER)
endif
ifneq ($(MAXCHUNKS_SESSION),)
$(TARGET)_CFLAGS+=-DMAXCHUNKS_SESSION=$(MAXCHUNKS_SESSION)
endif
ifneq ($(MAXCHUNKS_URI),)
$(TARGET)_CFLAGS+=-DMAXCHUNKS_URI=$(MAXCHUNKS_URI)
endif
ifneq ($(MAXWEBSOCKETS),)
$(TARGET)_CFLAGS+=-DMAXWEBSOCKETS=$(MAXWEBSOCKETS)
else
$(TARGET)_CFLAGS+=-DMAXWEBSOCKETS=10
endif

ifeq ($(VTHREAD_TYPE),)
VTHREAD_TYPE:=fork
else ifeq ($(VTHREAD_TYPE),pthread)
$(TARGET)_LIBRARY-$(VTHREAD)+=pthread
endif
$(TARGET)_SOURCES-$(VTHREAD)+=vthread_$(VTHREAD_TYPE).c
vthread_pthread_CFLAGS+=-DHAVE_SCHED_YIELD
vthread_fork_CFLAGS+=-DHAVE_SCHED_YIELD

lib-$(DLIB_URI)+=uri
slib-$(SLIB_URI)+=uri
hostslib-y+=uri

uri_SOURCES+=uri.c
uri_CFLAGS+=-I../../include/httpserver

lib-$(DLIB_WEBSOCKET)+=websocket
slib-$(SLIB_WEBSOCKET)+=websocket
hostslib-$(WEBSOCKET)+=websocket
websocket_SOURCES+=websocket.c
websocket_CFLAGS+=-I../../include/httpserver

$(TARGET)_CFLAGS-$(DEBUG)+=-g -DDEBUG
uri_CFLAGS-$(DEBUG)+=-g -DDEBUG
websocket_CFLAGS-$(DEBUG)+=-g -DDEBUG

ifeq ($(WIN32),1)
$(TARGET)_LDFLAGS+=-lws2_32
endif

$(TARGET)_LDFLAGS-$(DEBUG)+=-lrt
