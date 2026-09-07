CC ?= cc
AR ?= ar
CPPFLAGS ?=
CFLAGS ?= -O2
LDFLAGS ?=

.PHONY: all check clean install

all: bin/liquid-source-fixture lib/libliquid-source-fixture.a \
	lib/pkgconfig/liquid-source-fixture.pc \
	share/man/man1/liquid-source-fixture.1

build/fixture.o: src/fixture.c include/liquid-source-fixture.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinclude -c src/fixture.c -o $@

build/main.o: src/main.c include/liquid-source-fixture.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -Iinclude -c src/main.c -o $@

lib/libliquid-source-fixture.a: build/fixture.o
	mkdir -p lib
	$(AR) rcs $@ $<

bin/liquid-source-fixture: build/main.o lib/libliquid-source-fixture.a
	mkdir -p bin
	$(CC) $(LDFLAGS) build/main.o lib/libliquid-source-fixture.a -o $@

lib/pkgconfig/liquid-source-fixture.pc: liquid-source-fixture.pc
	mkdir -p lib/pkgconfig
	cp liquid-source-fixture.pc $@

share/man/man1/liquid-source-fixture.1: liquid-source-fixture.1
	mkdir -p share/man/man1
	cp liquid-source-fixture.1 $@

check: all
	./bin/liquid-source-fixture | grep -Fx 'liquid source fixture'

install: all
	install -d $(DESTDIR)/usr/bin
	install -m 0755 bin/liquid-source-fixture $(DESTDIR)/usr/bin/liquid-source-fixture
	install -d $(DESTDIR)/usr/include
	install -m 0644 include/liquid-source-fixture.h \
		$(DESTDIR)/usr/include/liquid-source-fixture.h
	install -d $(DESTDIR)/usr/lib/$(DEB_HOST_MULTIARCH)
	install -m 0644 lib/libliquid-source-fixture.a \
		$(DESTDIR)/usr/lib/$(DEB_HOST_MULTIARCH)/libliquid-source-fixture.a
	install -d $(DESTDIR)/usr/lib/$(DEB_HOST_MULTIARCH)/pkgconfig
	install -m 0644 lib/pkgconfig/liquid-source-fixture.pc \
		$(DESTDIR)/usr/lib/$(DEB_HOST_MULTIARCH)/pkgconfig/liquid-source-fixture.pc
	install -d $(DESTDIR)/usr/share/man/man1
	install -m 0644 share/man/man1/liquid-source-fixture.1 \
		$(DESTDIR)/usr/share/man/man1/liquid-source-fixture.1

clean:
	rm -rf bin build lib share
