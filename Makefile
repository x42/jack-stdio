# override e.g. `make install PREFIX=/usr`
PREFIX = /usr/local

CFLAGS=-Wall `pkg-config --cflags jack` -O2
LIBS=`pkg-config --libs jack` -lpthread -lm
#compat w/ NetBSD and GNU Make
LDADD=${LIBS}
LDLIBS=${LIBS}

all: jack-stdout jack-stdin

jack-stdout: jack-stdout.c

jack-stdin: jack-stdin.c

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 jack-stdout $(DESTDIR)$(PREFIX)/bin/
	install -m 755 jack-stdin  $(DESTDIR)$(PREFIX)/bin/
	install -d $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 644 jack-stdout.1 $(DESTDIR)$(PREFIX)/share/man/man1/
	install -m 644 jack-stdin.1 $(DESTDIR)$(PREFIX)/share/man/man1/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/jack-stdout
	rm -f $(DESTDIR)$(PREFIX)/bin/jack-stdin
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/jack-stdout.1
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/jack-stdin.1
	-rmdir $(DESTDIR)$(PREFIX)/bin
	-rmdir $(DESTDIR)$(PREFIX)/share/man/man1

clean:
	/bin/rm -f jack-stdout

.PHONY: all install uninstall clean
