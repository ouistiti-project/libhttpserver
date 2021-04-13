include-y+=httpserver/httpserver.h
include-y+=httpserver/log.h
include-$(LIBUTILS)+=httpserver/utils.h
include-$(LIBHASH)+=httpserver/hash.h
include-$(LIBWEBSOCKET)+=httpserver/websocket.h

install:
	install -D ../config.h $(DESTDIR)$(includedir)/httpserver/
	install -D ../version.h $(DESTDIR)$(includedir)/httpserver/
