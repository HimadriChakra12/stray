.POSIX:

CC      = cc
VERSION != git describe --tags --always --dirty 2>/dev/null || echo dev

CFLAGS  = -std=c11 -pedantic -Wall -Wextra -Os -D_POSIX_C_SOURCE=200809L \
          -DSTRAY_VERSION='"$(VERSION)"' \
          -isystem vendor \
          `pkg-config --cflags xrandr`
LDLIBS  = -lX11 -lXrandr

PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin
BINARY  = stray

all: stray

stray: stray.c config.h vendor/stb_ds.h
	$(CC) $(CFLAGS) -o $@ stray.c $(LDLIBS)

install: $(BINARY)
	strip $(BINARY)
	install -Dm755 $(BINARY) $(BINDIR)/$(BINARY)

uninstall:
	rm -f $(BINDIR)/stray

clean:
	rm -f stray

.PHONY: all install uninstall clean
