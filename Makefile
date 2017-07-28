include scripts.mk

ifneq ($(MBEDTLS),)
LIBUTILS=y
endif
ifneq ($(WEBSOCKET),)
LIBUTILS=y
LIBWEBSOCKET=y
endif
export LIBUTILS LIBWEBSOCKET

subdir-y+=src/httpserver
subdir-y+=include
subdir-$(MBEDTLS)+=src/mod_mbedtls.mk
subdir-$(WEBSOCKET)+=src/mod_websocket.mk
subdir-$(FORMPARSER)+=src/mod_formparser.mk
subdir-$(DATE)+=src/mod_date.mk
subdir-$(TEST)+=src/test.mk
subdir-y+=src/utils.mk

ifeq ($(CC),mingw32-gcc)
WIN32:=1
endif
ifeq ($(CC),cl.exe)
WIN32:=1
endif

ifeq ($(WIN32),1)
SLIBEXT=lib
DLIBEXT=dll
else
SLIBEXT=a
DLIBEXT=so
endif
