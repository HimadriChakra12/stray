.POSIX:

CC      = cc
VERSION != git describe --tags --always --dirty 2>/dev/null || echo dev

CFLAGS  = -std=c11 -pedantic -Wall -Wextra -Os -D_POSIX_C_SOURCE=200809L \
          -DSTRAY_VERSION='"$(VERSION)"' \
          -isystem vendor \
          `pkg-config --cflags xrandr`
LDLIBS  = -lX11 -lXrandr

BINDIR  = $(HOME)/.local/bin

all: stray

stray: stray.c config.h vendor/stb_ds.h
	$(CC) $(CFLAGS) -o $@ stray.c $(LDLIBS)

install: stray
	mkdir -p $(BINDIR)
	ln -sf "$$(pwd)/stray" $(BINDIR)/stray

uninstall:
	rm -f $(BINDIR)/stray

clean:
	rm -f stray

.PHONY: all install uninstall clean
