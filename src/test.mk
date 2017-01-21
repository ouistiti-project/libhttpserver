bin-$(TEST)+=httptest
httptest_CFLAGS+=-I../include/httpserver -DHTTPSERVER
httptest_LDFLAGS+=-DHTTPSERVER -L. -Lhttpserver
httptest_SOURCES+=test.c
httptest_LIBRARY+=httpserver
httptest_LIBRARY-$(MBEDTLS)+=mod_mbedtls
httptest_CFLAGS-$(MBEDTLS)+=-DMBEDTLS
httptest_LIBRARY-$(STATIC_FILE)+=mod_static_file
httptest_CFLAGS-$(STATIC_FILE)+=-DSTATIC_FILE

httptest_CFLAGS-$(DEBUG)+=-g -DDEBUG
