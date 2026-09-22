CC         ?= cc
PREFIX     ?= /usr/local
BINDIR     ?= $(PREFIX)/bin
MANDIR     ?= $(PREFIX)/share/man
SYSCONFDIR ?= /etc
UNITDIR    ?= /etc/systemd/system
CFLAGS     ?= -O2 -Wall -Wextra -std=gnu11
LDFLAGS    ?=

# VERSION is the single place to bump the app version.
VERSION := $(shell cat VERSION)

# Try to detect ncurses via pkg-config (wide-char build preferred).
NCURSES_PKG := $(shell pkg-config --exists ncursesw 2>/dev/null && echo ncursesw || \
                       (pkg-config --exists ncurses 2>/dev/null && echo ncurses))

ifneq ($(NCURSES_PKG),)
    NCURSES_CFLAGS := $(shell pkg-config --cflags $(NCURSES_PKG))
    NCURSES_LIBS   := $(shell pkg-config --libs $(NCURSES_PKG))
else
    # Fall back to plain -lncursesw if pkg-config metadata is missing.
    NCURSES_LIBS   := -lncursesw
endif

ZLIB_CFLAGS := $(shell pkg-config --cflags zlib 2>/dev/null)
ZLIB_LIBS   := $(shell pkg-config --libs zlib 2>/dev/null || echo -lz)

# Kept out of CFLAGS so `make CFLAGS=...` (as CI and packagers do) can't
# drop the version, ncurses or zlib flags.
CPUMON_CPPFLAGS := -DVERSION=\"$(VERSION)\" -DSYSCONFDIR=\"$(SYSCONFDIR)\" \
                   -DUSE_NCURSES $(NCURSES_CFLAGS) $(ZLIB_CFLAGS)
CPUMON_LIBS     := $(NCURSES_LIBS) $(ZLIB_LIBS)

all: cpumon

cpumon: src/cpumon.c VERSION
	$(CC) $(CPPFLAGS) $(CPUMON_CPPFLAGS) $(CFLAGS) -o $@ src/cpumon.c $(LDFLAGS) $(CPUMON_LIBS)

# Unit tests compile the program's source directly (minus main) so they
# can exercise its static helpers.
tests/test_cpumon: tests/test_cpumon.c src/cpumon.c VERSION
	$(CC) $(CPPFLAGS) $(CPUMON_CPPFLAGS) $(CFLAGS) -Wno-unused-function -o $@ tests/test_cpumon.c $(LDFLAGS) $(CPUMON_LIBS)

test: tests/test_cpumon
	./tests/test_cpumon

smoke: cpumon
	sh tests/smoke.sh ./cpumon

check: test smoke

# The unit hardcodes /usr/bin and /etc (the packaged layout); rewrite
# both for this install's BINDIR/SYSCONFDIR so the service can start.
install: cpumon
	install -Dm755 cpumon $(DESTDIR)$(BINDIR)/cpumon
	install -Dm644 man/cpumon.1 $(DESTDIR)$(MANDIR)/man1/cpumon.1
	install -d $(DESTDIR)$(UNITDIR)
	sed -e 's|/usr/bin/cpumon|$(BINDIR)/cpumon|g' \
	    -e 's|/etc/cpumon/|$(SYSCONFDIR)/cpumon/|g' \
	    systemd/cpumon.service > $(DESTDIR)$(UNITDIR)/cpumon.service
	chmod 644 $(DESTDIR)$(UNITDIR)/cpumon.service
	@if [ -e $(DESTDIR)$(SYSCONFDIR)/cpumon/cpumon.conf ]; then \
	    echo "Keeping existing $(DESTDIR)$(SYSCONFDIR)/cpumon/cpumon.conf (new default: config/cpumon.conf)"; \
	else \
	    install -Dm644 config/cpumon.conf $(DESTDIR)$(SYSCONFDIR)/cpumon/cpumon.conf; \
	fi
	@echo ""
	@echo "Installed. Next steps:"
	@echo "  sudo systemctl daemon-reload"
	@echo "  sudo systemctl enable --now cpumon.service"

uninstall:
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null 2>&1 && \
	    [ -e $(UNITDIR)/cpumon.service ]; then \
	    echo "Stopping and disabling cpumon.service"; \
	    systemctl disable --now cpumon.service || true; \
	fi
	rm -f $(DESTDIR)$(BINDIR)/cpumon
	rm -f $(DESTDIR)$(MANDIR)/man1/cpumon.1
	rm -f $(DESTDIR)$(UNITDIR)/cpumon.service
	rm -f $(DESTDIR)$(SYSCONFDIR)/cpumon/cpumon.conf
	-rmdir $(DESTDIR)$(SYSCONFDIR)/cpumon 2>/dev/null
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null 2>&1; then \
	    systemctl daemon-reload || true; \
	fi
	@echo "Log files (default /var/log/cpumon) were left in place."

clean:
	rm -f cpumon tests/test_cpumon

.PHONY: all test smoke check install uninstall clean
