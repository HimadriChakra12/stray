.POSIX:

CC      = cc

# Version derived from `git describe` at build time so the binary reports
# the exact tag/commit it was built from; "dev" without git metadata.
VERSION != git describe --tags --always --dirty 2>/dev/null || echo dev

CFLAGS  = -std=c11 -pedantic -Wall -Wextra -Os -D_POSIX_C_SOURCE=200809L \
          -DSTRAY_VERSION='"$(VERSION)"' \
          -isystem vendor `pkg-config --cflags xft`
LDLIBS  = -lX11 -lXrandr `pkg-config --libs xft`
BINDIR  = $(HOME)/.local/bin

all: stray

stray: stray.c config.h stb_ds.h
	$(CC) $(CFLAGS) -o $@ stray.c $(LDLIBS)

install: stray
	mkdir -p $(BINDIR)
	ln -sf "$$(pwd)/stray" $(BINDIR)/stray

uninstall:
	rm -f $(BINDIR)/stray

clean:
	rm -f stray

.PHONY: all install uninstall clean
