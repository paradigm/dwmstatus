CC=cc
CFLAGS=-Wall -Wextra -Werror -Os -std=c99
PREFIX=/usr

all: dwmstatus

clean:
	- rm -f dwmstatus

config.h:
	cp config.def.h config.h

dwmstatus: dwmstatus.c config.h
	$(CC) $(CFLAGS) dwmstatus.c -o dwmstatus -lX11

install: dwmstatus
	mkdir -p $(DESTDIR)$(PREFIX)/bin/
	install dwmstatus $(DESTDIR)$(PREFIX)/bin/
	chmod 755 $(DESTDIR)$(PREFIX)/bin/dwmstatus
