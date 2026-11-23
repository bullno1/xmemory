CC ?= cc
CFLAGS ?= -O2 -g
PREFIX ?= /usr/local
PKGS := xcb xcb-ewmh xcb-res xcb-randr

XMEMORY_CFLAGS := -std=c23 -Wall -Wextra -Wpedantic $(shell pkg-config --cflags $(PKGS))
XMEMORY_LIBS := $(shell pkg-config --libs $(PKGS))

xmemory: xmemory.c
	$(CC) $(XMEMORY_CFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS) $(XMEMORY_LIBS)

UNIT_DIR := $(PREFIX)/lib/systemd/user

# The unit is generated at install time since it depends on PREFIX
install: xmemory xmemory.service.in
	install -Dm755 xmemory $(DESTDIR)$(PREFIX)/bin/xmemory
	install -dm755 $(DESTDIR)$(UNIT_DIR)
	sed 's|@BINDIR@|$(PREFIX)/bin|' xmemory.service.in > $(DESTDIR)$(UNIT_DIR)/xmemory.service
	chmod 644 $(DESTDIR)$(UNIT_DIR)/xmemory.service

clean:
	rm -f xmemory

.PHONY: install clean
