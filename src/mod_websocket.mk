LIBB64_DIR=../libb64

modules-$(DYNAMIC)+=mod_websocket
slib-$(STATIC)+=mod_websocket
mod_websocket_LDFLAGS+=-L../utils
mod_websocket_SOURCES-$(WEBSOCKET)+=mod_websocket.c
mod_websocket_CFLAGS+=-I../include
mod_websocket_CFLAGS+=-DWEBSOCKET
mod_websocket_CFLAGS-$(MBEDTLS)+=-DMBEDTLS
ifneq ($(MBEDTLS),y)
mod_websocket_LIBS+=b64
mod_websocket_CFLAGS+=-I$(LIBB64_DIR)/include
endif

mod_websocket_CFLAGS-$(DEBUG)+=-g -DDEBUG

