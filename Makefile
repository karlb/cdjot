PREFIX = /usr/local

CFLAGS = -std=c99 -Wall -Wextra -pedantic -O2
LDFLAGS =

# Fuzzing config (overridable: `make fuzz FUZZ_CC=clang-15`)
FUZZ_CC ?= clang
FUZZ_CFLAGS = -std=c99 -g -O1 -DCDJOT_NO_MAIN
FUZZ_SAN = -fsanitize=address,undefined -fno-sanitize-recover=undefined

# Coverage tools fall back to the versioned binaries (Debian ships
# only `llvm-profdata-19` / `llvm-cov-19` on PATH; the unversioned
# names live in /usr/lib/llvm-19/bin which is not on PATH by default).
LLVM_PROFDATA ?= $(shell command -v llvm-profdata 2>/dev/null || command -v llvm-profdata-19 2>/dev/null || echo llvm-profdata)
LLVM_COV ?= $(shell command -v llvm-cov 2>/dev/null || command -v llvm-cov-19 2>/dev/null || echo llvm-cov)

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
# `make fuzz-corpus`  extract seed inputs from test/*.test into fuzz/corpus/
# `make fuzz-run`     fuzz/cdjot-fuzz with corpus + dictionary
# `make fuzz-afl-run` minimize corpus into fuzz/corpus-afl/ then run afl-fuzz
# `make fuzz-sync`    fold AFL queue findings into fuzz/corpus/ and re-merge
# `make fuzz-replay`  replay fuzz/corpus through fuzz/cdjot-fuzz to catch crashes
# `make fuzz-baseline` snapshot current proptest BAD filenames into findings/
# `make fuzz-check`   run proptests; report which BAD files are new vs baseline
# `make fuzz-cov`     LLVM source-based coverage of fuzz/corpus on cdjot.c
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
# afl-cmin uses fuzz/corpus-afl/.traces/ as a working dir and leaves it
# behind on completion; clean it explicitly so the dir holds only the
# minimized inputs that afl-fuzz consumes.
# AFL_SKIP_CPUFREQ=1 skips the CPU governor warning that otherwise wants
# root to set the scaling_governor to performance. -m none disables AFL's
# memory limit (incompatible with ASan).
fuzz-afl-run: fuzz/cdjot-fuzz-afl fuzz-corpus
	rm -rf fuzz/corpus-afl
	AFL_SKIP_CPUFREQ=1 afl-cmin -T all -i fuzz/corpus -o fuzz/corpus-afl -m none -- ./fuzz/cdjot-fuzz-afl
	rm -rf fuzz/corpus-afl/.traces
	AFL_SKIP_CPUFREQ=1 afl-fuzz -i fuzz/corpus-afl -o fuzz/afl-out -m none -- ./fuzz/cdjot-fuzz-afl

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

# Fold AFL's queue findings into libFuzzer's corpus so both fuzzers benefit
# from each other's coverage. Rotates corpus → corpus.bak before swapping
# in the merged set so a bad merge is recoverable.
fuzz-sync: fuzz/cdjot-fuzz
	@test -d fuzz/afl-out/default/queue || { echo "no AFL queue at fuzz/afl-out/default/queue"; exit 1; }
	@before=$$(ls fuzz/corpus | wc -l); \
	 find fuzz/afl-out/default/queue -name 'id:*' -exec cp -n {} fuzz/corpus/ \; ; \
	 added=$$(($$(ls fuzz/corpus | wc -l) - $$before)); \
	 echo "copied $$added AFL inputs into fuzz/corpus/"
	rm -rf fuzz/corpus.min && mkdir fuzz/corpus.min
	./fuzz/cdjot-fuzz -merge=1 fuzz/corpus.min fuzz/corpus
	rm -rf fuzz/corpus.bak
	mv fuzz/corpus fuzz/corpus.bak
	mv fuzz/corpus.min fuzz/corpus
	@echo "corpus: $$(ls fuzz/corpus | wc -l) files (was $$(ls fuzz/corpus.bak | wc -l))"

# Replay every corpus input through the libFuzzer harness (-runs=0 means
# no mutation; libFuzzer just feeds each file through LLVMFuzzerTestOneInput
# under ASan/UBSan). -timeout=5 caps each input at 5s wall-clock; libFuzzer
# aborts on the first timeout/crash and prints the offending input path.
fuzz-replay: fuzz/cdjot-fuzz fuzz-corpus
	./fuzz/cdjot-fuzz -timeout=5 -runs=0 fuzz/corpus

# Coverage analysis (one-shot). Build cdjot with LLVM source-based
# coverage, replay the libFuzzer corpus through it, and emit per-line
# branch counts so structurally-uncovered constructs can be seeded
# manually (corpus inputs or fuzz/cdjot.dict entries) instead of
# waiting on compute. Counter $$i (not %p) names each profraw because
# PIDs can recycle inside a tight shell loop and clobber files.
fuzz-cov: cdjot.c cdjot.h fuzz-corpus
	$(FUZZ_CC) -std=c99 -O0 -g -fprofile-instr-generate -fcoverage-mapping \
		-o fuzz/cdjot-cov cdjot.c
	rm -rf fuzz/cov && mkdir fuzz/cov
	@i=0; for f in fuzz/corpus/*; do \
		i=$$((i+1)); \
		LLVM_PROFILE_FILE=fuzz/cov/$$i.profraw \
			./fuzz/cdjot-cov < "$$f" > /dev/null; \
	done
	$(LLVM_PROFDATA) merge -sparse fuzz/cov/*.profraw -o fuzz/cov/cdjot.profdata
	@echo "--- summary ---"
	$(LLVM_COV) report ./fuzz/cdjot-cov -instr-profile=fuzz/cov/cdjot.profdata
	$(LLVM_COV) show ./fuzz/cdjot-cov -instr-profile=fuzz/cov/cdjot.profdata \
		-show-branches=count -show-line-counts-or-regions cdjot.c \
		> fuzz/cov/coverage.txt
	@echo "per-line:        fuzz/cov/coverage.txt"
	@echo "uncovered lines: awk '/^ *0\\|/' fuzz/cov/coverage.txt"

fuzz-clean:
	rm -f fuzz/cdjot-fuzz fuzz/cdjot-fuzz-afl fuzz/cdjot.dict.lf fuzz/cdjot-cov
	rm -rf fuzz/crashes fuzz/corpus-afl fuzz/afl-out fuzz/cov

.PHONY: clean test install bench fuzz fuzz-afl fuzz-corpus \
	fuzz-run fuzz-afl-run fuzz-sync fuzz-baseline fuzz-check \
	fuzz-replay fuzz-cov fuzz-clean
