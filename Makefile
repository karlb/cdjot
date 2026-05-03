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
# See fuzz/README.md for the workflow narrative; this block is the target index.
# `make fuzz`         build libFuzzer harness (requires clang + compiler-rt fuzzer)
# `make fuzz-afl`     build AFL++ persistent-mode harness (requires afl-clang-fast)
# `make fuzz-asan`    build standalone batch driver (ASan/UBSan, no fuzzer rt needed)
# `make fuzz-corpus`  extract seed inputs from test/*.test into fuzz/corpus/
# `make fuzz-run`     fuzz/cdjot-fuzz with corpus + dictionary
# `make fuzz-afl-run` minimize corpus into fuzz/corpus-afl/ then run afl-fuzz
# `make fuzz-replay`  replay fuzz/corpus through fuzz/cdjot-asan to catch crashes
# `make fuzz-merge`   minimize fuzz/corpus/ in place; keeps fuzz/corpus.bak
# `make fuzz-baseline` snapshot current proptest BAD filenames into findings/
# `make fuzz-check`   run proptests; report which BAD files are new vs baseline
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

# AFL++ chokes on the libFuzzer-grown fuzz/corpus/ (huge + many redundant
# coverage duplicates), so minimize into a separate dir and run from there.
# AFL_SKIP_CPUFREQ=1 skips the CPU governor warning that otherwise wants
# root to set the scaling_governor to performance. -m none disables AFL's
# memory limit (incompatible with ASan).
fuzz-afl-run: fuzz/cdjot-fuzz-afl fuzz-corpus
	rm -rf fuzz/corpus-afl
	AFL_SKIP_CPUFREQ=1 afl-cmin -T all -i fuzz/corpus -o fuzz/corpus-afl -m none -- ./fuzz/cdjot-fuzz-afl
	AFL_SKIP_CPUFREQ=1 afl-fuzz -i fuzz/corpus-afl -o fuzz/afl-out -m none -- ./fuzz/cdjot-fuzz-afl

# Drop coverage-redundant inputs from fuzz/corpus/ using libFuzzer's own
# minimizer. The previous corpus is preserved as fuzz/corpus.bak so a
# regretted merge can be rolled back. Refuses to clobber an existing .bak.
fuzz-merge: fuzz/cdjot-fuzz
	@if [ -e fuzz/corpus.bak ]; then \
		echo "fuzz/corpus.bak already exists; remove it first to confirm overwrite"; \
		exit 1; \
	fi
	rm -rf fuzz/corpus.min
	mkdir -p fuzz/corpus.min
	./fuzz/cdjot-fuzz -merge=1 fuzz/corpus.min fuzz/corpus
	mv fuzz/corpus fuzz/corpus.bak
	mv fuzz/corpus.min fuzz/corpus
	@echo "merged: $$(ls fuzz/corpus | wc -l) kept (was $$(ls fuzz/corpus.bak | wc -l)). Old corpus preserved at fuzz/corpus.bak"

# Snapshot the current set of proptest BAD filenames as the baseline.
# Run this once on a known-good state (e.g., after fixing a bug or
# accepting that a noise pattern is permanent). The baseline files
# live in findings/baseline-*.txt and are gitignored — each developer
# keeps their own per-corpus snapshot.
fuzz-baseline: cdjot fuzz-corpus
	@mkdir -p findings
	@for s in wellformed attrsafe idunique; do \
		SHOW=999999 node proptest/$$s.js fuzz/corpus/* 2>/dev/null | \
			awk '/^=== BAD/ {print $$3}' | sort -u > findings/baseline-$$s.txt; \
		printf '%-12s baseline: %d failure(s) → findings/baseline-%s.txt\n' \
			"$$s" "$$(wc -l < findings/baseline-$$s.txt)" "$$s"; \
	done

# Run all proptests against the corpus and report inputs that fail now
# but didn't in the baseline (and ones that were failing but no longer
# do). Requires a prior `make fuzz-baseline`.
fuzz-check: cdjot fuzz-corpus
	@for s in wellformed attrsafe idunique; do \
		baseline=findings/baseline-$$s.txt; \
		if [ ! -f $$baseline ]; then \
			echo "$$s: no baseline at $$baseline; run 'make fuzz-baseline' first"; \
			continue; \
		fi; \
		cur=$$(mktemp); \
		SHOW=999999 node proptest/$$s.js fuzz/corpus/* 2>/dev/null | \
			awk '/^=== BAD/ {print $$3}' | sort -u > $$cur; \
		new=$$(grep -vxFf $$baseline $$cur 2>/dev/null || true); \
		gone=$$(grep -vxFf $$cur $$baseline 2>/dev/null || true); \
		rm -f $$cur; \
		if [ -z "$$new" ] && [ -z "$$gone" ]; then \
			echo "$$s: matches baseline"; \
		else \
			[ -n "$$new" ]  && { echo "$$s: NEW failures since baseline:"; echo "$$new"  | sed 's/^/  + /'; }; \
			[ -n "$$gone" ] && { echo "$$s: gone (now passing):";          echo "$$gone" | sed 's/^/  - /'; }; \
		fi; \
	done

fuzz-replay: fuzz/cdjot-asan fuzz-corpus
	@n=0; for f in fuzz/corpus/*; do \
		timeout 5s ./fuzz/cdjot-asan "$$f" >/dev/null 2>&1 || \
			{ rc=$$?; printf 'SLOW/FAIL rc=%d %s\n' "$$rc" "$$f"; n=$$((n+1)); }; \
	done; \
	echo "fuzz-replay: $$n input(s) timed out or failed"

fuzz-clean:
	rm -f fuzz/cdjot-fuzz fuzz/cdjot-fuzz-afl fuzz/cdjot-asan fuzz/cdjot.dict.lf
	rm -rf fuzz/crashes fuzz/corpus-afl fuzz/afl-out

.PHONY: clean test install bench fuzz fuzz-afl fuzz-asan fuzz-corpus \
	fuzz-run fuzz-afl-run fuzz-merge fuzz-baseline fuzz-check \
	fuzz-replay fuzz-clean
