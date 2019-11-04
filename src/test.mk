bin-$(TEST)+=httptest
httptest_CFLAGS+=-I../include/httpserver -DHTTPSERVER
httptest_LDFLAGS+=-DHTTPSERVER -L. -Lhttpserver
httptest_SOURCES+=test.c
httptest_LIBRARY+=httpserver
httptest_LIBRARY-$(MBEDTLS)+=mod_mbedtls
httptest_LIBRARY-$(STATIC_FILE)+=mod_static_file

httptest_CFLAGS-$(DEBUG)+=-g -DDEBUG
