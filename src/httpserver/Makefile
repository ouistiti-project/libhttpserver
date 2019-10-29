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

lib-$(DLIB_HTTPSERVER)+=httpserver
slib-$(SLIB_HTTPSERVER)+=httpserver
httpserver_SOURCES+=httpserver.c tcpserver.c
httpserver_CFLAGS+=-I../../include/httpserver
# SERVER_DEFER_ACCEPT may protect the client connection from some exceptions.
# But it still decrease the speed of the answers.
httpserver_CFLAGS-$(IPV6)+=-DIPV6
httpserver_CFLAGS+=-DSERVER_DEFER_ACCEPT
httpserver_CFLAGS+=-DUSE_POLL
httpserver_CFLAGS-$(MBEDTLS)+=-DTLS
httpserver_CFLAGS-$(WOLFSSL)+=-DTLS
httpserver_CFLAGS-$(OPENSSL)+=-DTLS

ifneq ($(MAX_SERVERS),)
httpserver_CFLAGS+=-DMAXSERVERS=$(MAX_SERVERS)
endif

ifneq ($(MAXCHUNKS_HEADER),)
httpserver_CFLAGS+=-DMAXCHUNKS_HEADER=$(MAXCHUNKS_HEADER)
endif
ifneq ($(MAXCHUNKS_SESSION),)
httpserver_CFLAGS+=-DMAXCHUNKS_SESSION=$(MAXCHUNKS_SESSION)
endif
ifneq ($(MAXCHUNKS_URI),)
httpserver_CFLAGS+=-DMAXCHUNKS_URI=$(MAXCHUNKS_URI)
endif
ifneq ($(MAXWEBSOCKETS),)
httpserver_CFLAGS+=-DMAXWEBSOCKETS=$(MAXWEBSOCKETS)
else
httpserver_CFLAGS+=-DMAXWEBSOCKETS=10
endif

ifeq ($(VTHREAD_TYPE),)
VTHREAD_TYPE:=fork
else ifeq ($(VTHREAD_TYPE),pthread)
httpserver_LIBRARY-$(VTHREAD)+=pthread
endif
httpserver_SOURCES-$(VTHREAD)+=vthread_$(VTHREAD_TYPE).c
httpserver_CFLAGS-$(VTHREAD)+=-DVTHREAD
vthread_pthread_CFLAGS+=-DHAVE_SCHED_YIELD
vthread_fork_CFLAGS+=-DHAVE_SCHED_YIELD

lib-$(DLIB_URI)+=uri
slib-$(SLIB_URI)+=uri
uri_SOURCES+=uri.c
uri_CFLAGS+=-I../../include/httpserver

lib-$(DLIB_WEBSOCKET)+=websocket
slib-$(SLIB_WEBSOCKET)+=websocket
websocket_SOURCES+=websocket.c
websocket_CFLAGS+=-I../../include/httpserver

httpserver_CFLAGS-$(DEBUG)+=-g -DDEBUG
uri_CFLAGS-$(DEBUG)+=-g -DDEBUG
websocket_CFLAGS-$(DEBUG)+=-g -DDEBUG

ifeq ($(WIN32),1)
httpserver_LDFLAGS+=-lws2_32
endif

httpserver_LDFLAGS-$(DEBUG)+=-lrt
