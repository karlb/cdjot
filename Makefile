PREFIX = /usr/local

CFLAGS = -std=c99 -Wall -Wextra -pedantic -O2
LDFLAGS =

# Fuzzing config (overridable: `make fuzz FUZZ_CC=clang-15`)
FUZZ_CC ?= clang
FUZZ_CFLAGS = -std=c99 -g -O1 -DCDJOT_NO_MAIN
FUZZ_SAN = -fsanitize=address,undefined -fno-sanitize-recover=undefined

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

# --- Fuzzing -----------------------------------------------------------------
# `make fuzz`         build libFuzzer harness (requires clang + compiler-rt fuzzer)
# `make fuzz-afl`     build AFL++ persistent-mode harness (requires afl-clang-fast)
# `make fuzz-asan`    build standalone batch driver (ASan/UBSan, no fuzzer rt needed)
# `make fuzz-corpus`  extract seed inputs from test/*.test into fuzz/corpus/
# `make fuzz-run`     fuzz/cdjot-fuzz with corpus + dictionary
# `make fuzz-replay`  replay fuzz/corpus through fuzz/cdjot-asan to catch crashes
# `make fuzz-clean`   remove fuzz build artifacts (keeps corpus dir)

fuzz: fuzz/cdjot-fuzz

fuzz/cdjot-fuzz: fuzz/fuzz_cdjot.c cdjot.c cdjot.h
	$(FUZZ_CC) $(FUZZ_CFLAGS) -fsanitize=fuzzer,address,undefined \
		-fno-sanitize-recover=undefined \
		-o $@ fuzz/fuzz_cdjot.c cdjot.c

fuzz-afl: fuzz/cdjot-fuzz-afl

fuzz/cdjot-fuzz-afl: fuzz/fuzz_cdjot.c cdjot.c cdjot.h
	afl-clang-fast $(FUZZ_CFLAGS) $(FUZZ_SAN) \
		-o $@ fuzz/fuzz_cdjot.c cdjot.c

fuzz-asan: fuzz/cdjot-asan

fuzz/cdjot-asan: fuzz/fuzz_cdjot.c cdjot.c cdjot.h
	$(FUZZ_CC) $(FUZZ_CFLAGS) -DFUZZ_STANDALONE $(FUZZ_SAN) \
		-o $@ fuzz/fuzz_cdjot.c cdjot.c

fuzz-corpus:
	sh fuzz/extract_seeds.sh

fuzz-run: fuzz/cdjot-fuzz fuzz-corpus fuzz/cdjot.dict.lf
	cd fuzz && ./cdjot-fuzz -dict=cdjot.dict.lf -max_len=8192 corpus/

# libFuzzer's dict parser only honours \\, \", and \xHH escapes (not
# \n/\t/\r), so convert the natural-escape source dict into a libFuzzer-
# compatible one at build time. Keeps cdjot.dict editable as plain C
# strings.
fuzz/cdjot.dict.lf: fuzz/cdjot.dict
	sed 's/\\n/\\x0a/g; s/\\r/\\x0d/g; s/\\t/\\x09/g' $< > $@

fuzz-replay: fuzz/cdjot-asan fuzz-corpus
	@n=0; for f in fuzz/corpus/*.dj; do \
		timeout 5s ./fuzz/cdjot-asan "$$f" >/dev/null 2>&1 || \
			{ rc=$$?; printf 'SLOW/FAIL rc=%d %s\n' "$$rc" "$$f"; n=$$((n+1)); }; \
	done; \
	echo "fuzz-replay: $$n input(s) timed out or failed"

fuzz-clean:
	rm -f fuzz/cdjot-fuzz fuzz/cdjot-fuzz-afl fuzz/cdjot-asan fuzz/cdjot.dict.lf
	rm -rf fuzz/crashes

.PHONY: clean test install bench fuzz fuzz-afl fuzz-asan fuzz-corpus \
	fuzz-run fuzz-replay fuzz-clean
