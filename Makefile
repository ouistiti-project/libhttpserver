include scripts.mk

subdir-y+=src/httpserver
subdir-$(MBEDTLS)+=src/mod_mbedtls.mk
subdir-$(STATIC_FILE)+=src/mod_static_file.mk
subdir-$(DATE)+=src/mod_date.mk
subdir-$(TEST)+=src/test.mk

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
