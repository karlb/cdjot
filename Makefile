PREFIX = /usr/local

CFLAGS = -std=c99 -Wall -Wextra -pedantic -Os
LDFLAGS =

SRC = djot.c block.c inline.c buf.c
OBJ = $(SRC:.c=.o)

djot: $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $(OBJ)

.c.o:
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f djot $(OBJ)

test: djot
	sh test.sh

install: djot
	install -Dm755 djot $(DESTDIR)$(PREFIX)/bin/djot

.PHONY: clean test install
