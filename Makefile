CC       ?= cc
PREFIX   ?= /usr/local
SYSCONFDIR ?= /etc
CFLAGS   ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS  ?=

# VERSION is the single place to bump the app version.
VERSION := $(shell cat VERSION)
CFLAGS  += -DVERSION=\"$(VERSION)\"

# Try to detect ncurses via pkg-config (wide-char build preferred).
NCURSES_PKG := $(shell pkg-config --exists ncursesw 2>/dev/null && echo ncursesw || \
                       (pkg-config --exists ncurses 2>/dev/null && echo ncurses))

ifneq ($(NCURSES_PKG),)
    CFLAGS  += -DUSE_NCURSES $(shell pkg-config --cflags $(NCURSES_PKG))
    LDFLAGS += $(shell pkg-config --libs $(NCURSES_PKG))
else
    # Fall back to plain -lncursesw / -lncurses if pkg-config metadata is missing.
    CFLAGS  += -DUSE_NCURSES
    LDFLAGS += -lncursesw
endif

all: cpumon

cpumon: src/cpumon.c VERSION
	$(CC) $(CFLAGS) -o $@ src/cpumon.c $(LDFLAGS)

install: cpumon
	install -Dm755 cpumon $(DESTDIR)$(PREFIX)/bin/cpumon
	install -Dm644 man/cpumon.1 $(DESTDIR)$(PREFIX)/share/man/man1/cpumon.1
	install -Dm644 systemd/cpumon.service $(DESTDIR)/etc/systemd/system/cpumon.service
	install -Dm644 config/cpumon.conf $(DESTDIR)$(SYSCONFDIR)/cpumon/cpumon.conf
	@echo ""
	@echo "Installed. Next steps:"
	@echo "  sudo systemctl daemon-reload"
	@echo "  sudo systemctl enable --now cpumon.service"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/cpumon
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/cpumon.1
	rm -f $(DESTDIR)/etc/systemd/system/cpumon.service

clean:
	rm -f cpumon

.PHONY: all install uninstall clean
