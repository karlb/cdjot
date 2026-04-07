PREFIX = /usr/local

CFLAGS = -std=c99 -Wall -Wextra -pedantic -Os
LDFLAGS =

djot: djot.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ djot.c

clean:
	rm -f djot

test: djot
	sh test.sh

install: djot
	install -Dm755 djot $(DESTDIR)$(PREFIX)/bin/djot

bench: djot
	sh bench.sh

.PHONY: clean test install bench
