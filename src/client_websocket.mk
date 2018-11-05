bin-$(HTTPCLIENT_FEATURES)=client_websocket

client_websocket_SOURCES+=client_websocket.c
client_websocket_LDFLAGS+=-Lhttpserver/
client_websocket_LIBRARY+=pthread
client_websocket_LIBS+=httpserver
client_websocket_LIBS+=websocket

client_websocket_CFLAGS-$(DEBUG)+=-DDEBUG -g
