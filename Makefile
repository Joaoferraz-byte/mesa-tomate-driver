CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
CPPFLAGS ?=
LDFLAGS ?=
LIBUSB_CFLAGS := $(shell $(PKG_CONFIG) --cflags libusb-1.0)
LIBUSB_LIBS := $(shell $(PKG_CONFIG) --libs libusb-1.0)

.PHONY: all check clean install

all: mtm1106-mode

mtm1106-mode: src/mtm1106-mode.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LIBUSB_CFLAGS) $< $(LDFLAGS) $(LIBUSB_LIBS) -o $@

check: mtm1106-mode
	./mtm1106-mode --self-test
	./mtm1106-mode --profile digimend --dry-run
	./mtm1106-mode --profile mx002 --dry-run

install: mtm1106-mode
	install -Dm755 mtm1106-mode $(DESTDIR)$(PREFIX)/bin/mtm1106-mode

clean:
	rm -f mtm1106-mode
