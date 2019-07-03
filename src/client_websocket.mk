bin-$(HTTPCLIENT_FEATURES)=client_websocket

client_websocket_SOURCES+=client_websocket.c
client_websocket_CFLAGS+=-I../include
client_websocket_CFLAGS-$(MBEDTLS)+=-DMBEDTLS
client_websocket_LDFLAGS+=-L$(obj)httpserver/
client_websocket_LIBRARY+=pthread
client_websocket_LIBS+=httpserver
client_websocket_LIBS+=websocket hash_mod
client_websocket_LIBS-$(MBEDTLS)+=mbedtls mbedx509 mbedcrypto mod_mbedtls

client_websocket_CFLAGS-$(DEBUG)+=-DDEBUG -g
