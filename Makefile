CC      ?= gcc
PKGS     = gtk4 gtk4-layer-shell-0 json-glib-1.0
CFLAGS  += -Wall -Wextra -O2 -std=c11 $(shell pkg-config --cflags $(PKGS))
LDFLAGS += $(shell pkg-config --libs $(PKGS))

SRC = $(wildcard src/*.c)
OBJ = $(SRC:.c=.o)
BIN = ashell

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: run clean
