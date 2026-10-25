CC      ?= cc
STD     ?= c23# GCC 13: make STD=c2x
CFLAGS  ?= -O2 -pipe
LDLIBS  ?= -lpam -lm
PREFIX  ?= /usr/local
DESTDIR ?=

lumdm: src/lumdm.c src/vtfont.h src/scenes.h src/stb_truetype.h
	$(CC) -std=$(STD) $(CFLAGS) $(CPPFLAGS) -Wall -Wextra -Wno-format-truncation $(LDFLAGS) -s -o $@ $< $(LDLIBS)

install: lumdm
	install -Dm755 lumdm $(DESTDIR)$(PREFIX)/bin/lumdm
	@# never overwrite an edited /etc/lumdm.conf: new defaults land beside it as lumdm.conf.new
	@if [ -e $(DESTDIR)/etc/lumdm.conf ]; then \
	    install -Dm644 data/lumdm.conf $(DESTDIR)/etc/lumdm.conf.new; \
	    echo "kept existing /etc/lumdm.conf (new defaults: /etc/lumdm.conf.new)"; \
	else install -Dm644 data/lumdm.conf $(DESTDIR)/etc/lumdm.conf; fi
	install -Dm644 data/pam.d/lumdm $(DESTDIR)/etc/pam.d/lumdm
	install -Dm644 data/init/lumdm.service $(DESTDIR)/usr/lib/systemd/system/lumdm.service
	install -Dm755 data/init/lumdm/run $(DESTDIR)/etc/sv/lumdm/run

clean:
	rm -f lumdm
.PHONY: install clean
