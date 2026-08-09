CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
CPPFLAGS ?=
LDFLAGS ?=
LIBUSB_CFLAGS := $(shell $(PKG_CONFIG) --cflags libusb-1.0)
LIBUSB_LIBS := $(shell $(PKG_CONFIG) --libs libusb-1.0)

.PHONY: all check clean install

# Rebuild everything from source even if binaries already exist in the
# source tree: the packaged source may carry stale or host-incompatible
# binaries from a previous build (which would make the Nix checkPhase
# fail with "cannot execute" on the generic-linux ELF).
# Binaries are PHONY: always recompile, so the packaged source tree's
# (possibly stale) build artifacts never satisfy the dependency.
.PHONY: mtm1106-mode mtm1106-daemon

all: mtm1106-mode mtm1106-daemon

mtm1106-mode: src/mtm1106-mode.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LIBUSB_CFLAGS) $< $(LDFLAGS) $(LIBUSB_LIBS) -o $@

check: mtm1106-mode mtm1106-daemon
	./mtm1106-mode --self-test
	./mtm1106-mode --profile digimend --dry-run
	./mtm1106-mode --profile mx002 --dry-run

install: mtm1106-mode mtm1106-daemon
	install -Dm755 mtm1106-mode $(DESTDIR)$(PREFIX)/bin/mtm1106-mode
	install -Dm755 mtm1106-daemon $(DESTDIR)$(PREFIX)/bin/mtm1106-daemon

clean:
	rm -f mtm1106-mode mtm1106-daemon

mtm1106-daemon: src/mtm1106-daemon.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LIBUSB_CFLAGS) $< $(LDFLAGS) $(LIBUSB_LIBS) -o $@
