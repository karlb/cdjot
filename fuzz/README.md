# Fuzzing

cdjot has a libFuzzer + AFL++ + standalone-ASan setup. All targets live
in the top-level `Makefile` (search for `--- Fuzzing ---`). This file
describes the typical workflow.

## Components

| file | role |
|---|---|
| `fuzz_cdjot.c` | one source file, three entry points (libFuzzer / AFL persistent / standalone batch driver) selected by `-D` flags |
| `cdjot.dict` | hand-curated libFuzzer/AFL dictionary in natural C-string form (`\n`, `\t`, …) |
| `cdjot.dict.lf` | build artifact — `cdjot.dict` with escapes converted to libFuzzer's `\xHH` form |
| `extract_seeds.sh` | populates `corpus/` from `test/*.test` and (if present) `~/code/experiments/djot-corpus/` |
| `corpus/` | seed inputs + libFuzzer's coverage-guided additions |
| `corpus-afl/` | minimized seed set for AFL (cmin output) |
| `afl-out/` | AFL fuzz output |
| `corpus.bak/` | previous corpus preserved by `make fuzz-merge` |
| `crash-*` | inputs libFuzzer flagged as ASan/UBSan crashes |
| `slow-unit-*` | inputs libFuzzer flagged as slow (>20s by default) |

## First-time setup

- libFuzzer needs `clang` with `-fsanitize=fuzzer` (compiler-rt fuzzer
  runtime). Standard on most distros.
- AFL++ is optional: `sudo apt install afl++-clang` on Debian.

## Typical workflow

```sh
make fuzz-corpus      # one-shot: ~770 seeds (test/* + real-world djot)
make fuzz-baseline    # snapshot proptest BAD filenames into findings/
make fuzz-run         # libFuzzer; runs forever, Ctrl-C to stop
                      # grows corpus/ as it discovers new edges
make fuzz-replay      # ASan + UBSan + 5s timeout over the whole corpus
make fuzz-check       # diff proptest BAD set vs baseline; reports new failures
make fuzz-merge       # optional: drop coverage-redundant inputs from corpus/
                      # (keeps a corpus.bak in case you regret it)
```

`make fuzz-check` is the post-fuzz triage command: it runs all three
proptest scripts (`wellformed.js`, `attrsafe.js`, `idunique.js`) over
the full corpus and prints any inputs that fail now but didn't in the
baseline (and any that were failing but no longer do). See
`../proptest/README.md` for what each check enforces.

To re-snapshot the baseline (e.g., after fixing a bug or accepting a
new noise pattern as permanent), re-run `make fuzz-baseline`.

## Triaging a crash

```sh
ls fuzz/crash-* fuzz/slow-unit-*       # any findings?
./fuzz/cdjot-asan fuzz/crash-<sha1>    # see the sanitizer trace

# minimize for a small reproducer:
./fuzz/cdjot-fuzz -minimize_crash=1 -runs=100000 fuzz/crash-<sha1>
```

The minimized input usually fits in a regression test in
`test/cdjot.test`.

## AFL++ alongside libFuzzer

AFL uses different mutators and sometimes finds bugs libFuzzer misses,
but it chokes on libFuzzer's grown 25k-input corpus. `make fuzz-afl-run`
handles this:

1. `afl-cmin` minimizes `corpus/` → `corpus-afl/`
2. `afl-fuzz` runs against the minimized set, output to `afl-out/`

Both invocations set `AFL_SKIP_CPUFREQ=1` (skip the governor warning)
and `-m none` (ASan needs no memory cap).

## Tuning levers

If libFuzzer plateaus and you want to push further:

- `-max_len=N` in the Makefile recipe (default 8192) — larger inputs
  reach deeper grammar states but slow execution
- Add multi-token compound entries to `cdjot.dict` for constructs the
  fuzzer struggles to assemble from individual tokens (deep nested
  divs/lists, complete tables, etc.)
- `-jobs=N -workers=N` to parallelise libFuzzer across cores
- `make fuzz-merge` then re-run, so libFuzzer starts from a leaner
  base
- Try AFL++ as a complementary mutator
