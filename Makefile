# override e.g. `make install PREFIX=/usr`
PREFIX = /usr/local

CFLAGS=-Wall `pkg-config --cflags jack`
LIBS=`pkg-config --libs jack` -lpthread -lm
#compat w/ NetBSD and GNU Make
LDADD=${LIBS}
LDLIBS=${LIBS}

all: jack-stdout

jack-stdout: jack-stdout.c

install: all
	install -m 755 jack-stdout $(DESTDIR)$(PREFIX)/bin

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/jack-stdout

clean:
	/bin/rm -f jack-stdout

.PHONY: all install uninstall clean
