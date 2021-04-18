include-y+=ouistiti/httpserver.h
include-y+=ouistiti/log.h
include-$(LIBUTILS)+=ouistiti/utils.h
include-$(LIBHASH)+=ouistiti/hash.h
include-$(LIBWEBSOCKET)+=ouistiti/websocket.h

hook-install-$(DEVINSTALL)+=install-config

install-config:
	@echo INSTALL config.h version.h
	@install -m 0644 -D ../config.h $(DESTDIR)$(includedir)/ouistiti/
	@install -m 0644 -D ../version.h $(DESTDIR)$(includedir)/ouistiti/
