# Fuzzing

cdjot has a libFuzzer + AFL++ setup. All targets live in the top-level
`Makefile` (search for `--- Fuzzing ---`). This file describes the
typical workflow.

## Components

| file | role |
|---|---|
| `fuzz_cdjot.c` | one source file, two entry points (libFuzzer / AFL persistent) selected automatically by the AFL macros |
| `cdjot.dict` | hand-curated libFuzzer/AFL dictionary in natural C-string form (`\n`, `\t`, …) |
| `cdjot.dict.lf` | build artifact — `cdjot.dict` with escapes converted to libFuzzer's `\xHH` form |
| `extract_seeds.sh` | populates `corpus/` from `test/*.test` and (if present) `~/code/experiments/djot-corpus/` |
| `corpus/` | seed inputs + libFuzzer's coverage-guided additions |
| `corpus-afl/` | minimized seed set for AFL (cmin output) |
| `afl-out/` | AFL fuzz output |
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
make fuzz-replay      # libFuzzer -runs=0 over corpus/ under ASan + UBSan
                      # with a 5s per-input cap; aborts on first failure
make fuzz-check       # diff proptest BAD set vs baseline; reports new failures
```

If `corpus/` ever grows large enough to slow startup, dedupe by edge
with `./fuzz/cdjot-fuzz -merge=1 corpus.min corpus && rm -rf corpus &&
mv corpus.min corpus`. libFuzzer's coverage-guided growth makes this
rarely necessary.

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
./fuzz/cdjot-fuzz fuzz/crash-<sha1>    # see the sanitizer trace

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

After a meaningful AFL session, run `make fuzz-sync` to fold AFL's queue
findings (`afl-out/default/queue/id:*`) into `corpus/` and re-merge with
`-merge=1` so libFuzzer keeps only inputs that add new edges. The
previous corpus is rotated to `corpus.bak` in case the merge goes
sideways. This is the only place the two fuzzers share coverage —
without it, AFL's discoveries never reach libFuzzer's runs.

## Coverage analysis (one-shot)

When the fuzzer plateaus, `make fuzz-cov` builds cdjot with LLVM
source-based coverage, replays `corpus/` through it, and writes
`fuzz/cov/coverage.txt` with per-line and per-branch counts. To see
what the corpus never reached:

```sh
awk '/^ *0\|/' fuzz/cov/coverage.txt | less
```

Group hits by `do<construct>` parser to spot which djot grammar
shapes are still dark. Faster to seed those by hand into `corpus/`
or as compound entries in `cdjot.dict` than to wait for mutators to
assemble them.

## Tuning levers

If libFuzzer plateaus and you want to push further:

- `-max_len=N` in the Makefile recipe (default 8192) — larger inputs
  reach deeper grammar states but slow execution
- Add multi-token compound entries to `cdjot.dict` for constructs the
  fuzzer struggles to assemble from individual tokens (deep nested
  divs/lists, complete tables, etc.)
- `-jobs=N -workers=N` to parallelise libFuzzer across cores
- Manually merge with `./fuzz/cdjot-fuzz -merge=1 corpus.min corpus`
  (then swap dirs) so libFuzzer starts from a leaner base
- Try AFL++ as a complementary mutator
