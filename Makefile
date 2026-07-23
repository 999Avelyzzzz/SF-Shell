CC      ?= gcc
PKGS     = gtk4 gtk4-layer-shell-0 json-glib-1.0 fontconfig gio-unix-2.0

# Percorsi di installazione. PREFIX e DESTDIR sono sovrascrivibili:
#   make install                 -> /usr/bin/sfshell, /usr/share/sfshell/fonts
#   make PREFIX=/usr/local install
#   make DESTDIR=/pkg install    (staging per packaging)
PREFIX  ?= /usr
BINDIR   = $(DESTDIR)$(PREFIX)/bin
# DATADIR: percorso usato a runtime dal binario (senza DESTDIR).
DATADIR  = $(PREFIX)/share/sfshell
# DATADEST: destinazione di copia in fase di install (con DESTDIR per staging).
DATADEST = $(DESTDIR)$(DATADIR)

# SFSHELL_DATADIR indica al binario dove trovare i font una volta installato.
CFLAGS  += -Wall -Wextra -O2 -std=c11 -DSFSHELL_DATADIR='"$(DATADIR)"' \
           $(shell pkg-config --cflags $(PKGS))
LDFLAGS += $(shell pkg-config --libs $(PKGS)) -lm

SRC = $(wildcard src/*.c)
OBJ = $(SRC:.c=.o)
BIN = sfshell

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN)

# Compila e installa binario + font. Richiede i privilegi per scrivere in
# $(PREFIX) (es. sudo make install). 'make clean install' ricompila da zero.
install: $(BIN)
	install -Dm755 $(BIN) $(BINDIR)/$(BIN)
	install -d $(DATADEST)/fonts
	install -m644 src/fonts/* $(DATADEST)/fonts/

uninstall:
	rm -f $(BINDIR)/$(BIN)
	rm -rf $(DATADEST)

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: run install uninstall clean
