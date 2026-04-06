# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Single-file C99 djot-to-HTML converter. Reads stdin, writes stdout. No dependencies beyond libc.

## Build & Test

```sh
make            # build
make test       # run test suite (168 tests from jgm/djot.js)
VERBOSE=1 sh test.sh          # show diff on failures
VERBOSE=1 sh test.sh emphasis # run one category
printf '# hi\n' | ./djot      # manual test
```

## Architecture

`djot.c` (~2000 lines) follows smu's recursive dispatch pattern:

- **`process(begin, end, newblock)`** — central dispatcher. Tries each parser in order. `newblock=1` enables block parsers; `newblock=0` restricts to inline. Negative return = block consumed (sets newblock for next iteration), positive = inline consumed.
- **Block parsers** (fire when newblock=1): `doattr`, `dorefdef`, `doheading`, `doblockquote`, `docodefence`, `dothematicbreak`, `dolist`, `doparagraph`
- **Inline parsers** (fire when newblock=0): `dolinebreak`, `docode`, `dosurround`, `dolink`, `doautolink`, `doreplace`
- **Pre-scans** (run before `process`): `scan_refs` collects `[label]: url` definitions, `scan_footnotes` collects `[^label]:` definitions, `scan_heading_refs` creates implicit refs from headings

Key design decisions:
- Direct stdout output (no intermediate buffers except for list items and blockquotes which need recursive processing)
- `{#id .class key=val}` attributes stored as pending globals, consumed by the next block parser
- List items collected into a buffer, then `process()` recurses on the buffer
- Tight/loose list detection checks blanks between items and within item content
- Smart quotes use look-ahead stack simulation to match openers with closers
- `dosurround` handles emphasis runs, `{_`/`_}` explicit delimiters, and skips `[](` link syntax

## Goals

- Simplicity: single file, no deps, suckless-influenced style
- Correctness: 168/168 official djot.js tests passing
- The test suite in `test/` uses files from `jgm/djot.js`. Format: backtick-fenced blocks with `.` separating input from expected HTML
