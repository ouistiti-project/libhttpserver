SPECFILE:=./rpmbuild/libouistiti.spec
package=libouistiti
version=$(shell grep '^version[?,:, ]*=' ../Makefile | awk 'BEGIN{FS="="}{print $$2}')
version=$(shell grep -m 1 '^Version:[ ]*' $(SPECFILE) | awk 'BEGIN{FS=":"}{print $$2}' | sed 's/ //g')

RPMDIR?=$(HOME)

.PHONY: FORCE

debian: FORCE
	@debuild --no-sign

$(RPMDIR)/rpmbuild/SOURCES:
	@mkdir -p $@

$(RPMDIR)/rpmbuild/SOURCES/$(package)-$(version).tar.gz: $(RPMDIR)/rpmbuild/SOURCES
	@tar -cz --exclude=.git --exclude=.config* --exclude=package --exclude=rpmbuild .. > $@

rpm: $(RPMDIR)/rpmbuild/SOURCES/$(package)-$(version).tar.gz FORCE
	@rpmbuild -ba $(SPECFILE)
