include scripts.mk

LIBHASH=y

ifneq ($(MBEDTLS),)
LIBUTILS=y
endif
ifneq ($(WEBSOCKET),)
LIBUTILS=y
LIBWEBSOCKET=y
LIBB64_DIR:=libb64

ifeq ($(MBEDTLS),)
ifneq ($(wildcard $(LIBB64_DIR)/Makefile),)
subdir-y:=$(LIBB64_DIR)

libb64_dir:=$(realpath $(LIBB64_DIR))
export CFLAGS+=-I$(libb64_dir)/include/
export LDFLAGS+=-L$(libb64_dir)/src -L$(libb64_dir)/src
endif #LIBB64
endif #MBEDTLS
endif #WEBSOCKET
export LIBUTILS LIBWEBSOCKET

subdir-y+=src/httpserver
subdir-y+=include
subdir-$(WOLFTLS)+=src/mod_wolftls.mk
subdir-$(MBEDTLS)+=src/mod_mbedtls.mk
subdir-$(WEBSOCKET)+=src/mod_websocket.mk
subdir-$(FORMPARSER)+=src/mod_formparser.mk
subdir-$(DATE)+=src/mod_date.mk
subdir-$(COOKIE)+=src/mod_cookie.mk
subdir-$(TEST)+=src/test.mk
subdir-y+=src/utils.mk
subdir-y+=src/hash.mk

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
