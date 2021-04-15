SPECFILE:=./rpmbuild/libouistiti.spec
package=libouistiti
version=$(shell grep '^version[?,:, ]*=' ../Makefile | awk 'BEGIN{FS="="}{print $$2}')
version=$(shell grep -m 1 '^Version:[ ]*' $(SPECFILE) | awk 'BEGIN{FS=":"}{print $$2}' | sed 's/ //g')

.PHONY: FORCE

debian: FORCE
	@debuild --no-sign

../rpmbuild/SOURCES:
	@mkdir -p $@

../rpmbuild/SOURCES/$(package)-$(version).tar.gz: ../rpmbuild/SOURCES
	@tar -cz --exclude=.git --exclude=.config* --exclude=package --exclude=rpmbuild .. > $@

rpm: ../rpmbuild/SOURCES/$(package)-$(version).tar.gz FORCE
	@rpmbuild -ba $(SPECFILE)
