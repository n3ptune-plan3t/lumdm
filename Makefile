CC      ?= cc
STD     ?= c23              # GCC 13: make STD=c2x
CFLAGS  ?= -O2 -pipe
PREFIX  ?= /usr/local
DESTDIR ?=

lumdm: src/lumdm.c
	$(CC) -std=$(STD) $(CFLAGS) -Wall -Wextra -Wno-format-truncation -s -o $@ $< -lpam

install: lumdm
	install -Dm755 lumdm $(DESTDIR)$(PREFIX)/bin/lumdm
	install -Dm644 data/lumdm.conf $(DESTDIR)/etc/lumdm.conf
	install -Dm644 data/pam.d/lumdm $(DESTDIR)/etc/pam.d/lumdm
	install -Dm644 data/init/lumdm.service $(DESTDIR)/usr/lib/systemd/system/lumdm.service
	install -Dm755 data/init/lumdm/run $(DESTDIR)/etc/sv/lumdm/run

clean:
	rm -f lumdm
.PHONY: install clean
