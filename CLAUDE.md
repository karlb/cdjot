# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Single-file C99 djot-to-HTML converter. Reads stdin, writes stdout. No dependencies beyond libc.

## Build & Test

```sh
make            # build
make test       # run test suite (258 tests from jgm/djot.js)
VERBOSE=1 sh test.sh          # show diff on failures
VERBOSE=1 sh test.sh emphasis # run one category
printf '# hi\n' | ./djot      # manual test
```

## Architecture

`djot.c` (~2000 lines) follows smu's recursive dispatch pattern:

- **`process(begin, end, newblock)`** — central dispatcher. Tries each parser in order. `newblock=1` enables block parsers; `newblock=0` restricts to inline. Negative return = block consumed (sets newblock for next iteration), positive = inline consumed.
- **Block parsers** (fire when newblock=1): `doattr`, `dorefdef`, `doheading`, `doblockquote`, `docodefence`, `dodiv`, `dothematicbreak`, `dotable`, `dodeflist`, `dolist`, `doparagraph`
- **Inline parsers** (fire when newblock=0): `dolinebreak`, `docode`, `dosurround`, `dolink`, `doautolink`, `doreplace`
- **Pre-scans** (run before `process`): `prescan` collects `[label]: url` definitions, `[^label]:` footnote definitions, and heading auto-refs

Key design decisions:
- Direct stdout output (no intermediate buffers except for list items, blockquotes, divs, and footnotes which need recursive processing)
- `{#id .class key=val}` attributes stored as pending globals, consumed by the next block parser; inline attrs pre-processed in `doparagraph` by transforming `word{attrs}` → `[word]{attrs}`
- List items collected into a buffer, then `process()` recurses on the buffer
- Tight/loose list detection checks blanks between items and within item content
- Smart quotes use look-ahead stack simulation to match openers with closers
- `dosurround` handles emphasis (`_`/`*`), super/subscript (`^`/`~`), and insert/delete/mark (`{+`/`{-`/`{=`) with explicit `{`/`}` delimiters
- Tables detect separator rows for alignment and headers; captions via `^` after blank line
- Footnotes use sequential numbering, multi-paragraph content, backlink injected in last paragraph

## Goals

- Simplicity: single file, no deps, suckless-influenced style
- Correctness: 251/256 official djot.js tests passing (3 AST-format tests removed; symb.test, filters.test, sourcepos.test omitted; 5 remaining failures are attribute edge cases)
- The test suite in `test/` uses files from `jgm/djot.js`. Format: backtick-fenced blocks with `.` separating input from expected HTML
