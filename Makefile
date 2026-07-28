CC = clang
CFLAGS = -std=c99 -Wall -Wextra -O2 $(shell pkg-config --cflags libsystemd x11 imlib2)
LDFLAGS = $(shell pkg-config --libs libsystemd x11 imlib2)

SRC = main.c sni_watcher.c tray_window.c icon_theme.c xembed_tray.c
OBJ = $(SRC:.c=.o)
BIN = stray
PREFIX ?= /usr/local

$(BIN): $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c tray.h config.h
	$(CC) $(CFLAGS) -c $< -o $@

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: clean install uninstall
