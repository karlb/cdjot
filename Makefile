PREFIX = /usr/local

CFLAGS = -std=c99 -Wall -Wextra -pedantic -Os
LDFLAGS =

cdjot: cdjot.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ cdjot.c

clean:
	rm -f cdjot

test: cdjot
	sh test.sh

install: cdjot
	install -Dm755 cdjot $(DESTDIR)$(PREFIX)/bin/cdjot

bench: cdjot
	sh bench.sh

.PHONY: clean test install bench
