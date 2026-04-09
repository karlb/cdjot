# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

Single-file C99 djot-to-HTML converter. Reads stdin, writes stdout. No dependencies beyond libc.

## Build & Test

```sh
make            # build
make test       # run test suite (268 tests)
VERBOSE=1 sh test.sh          # show diff on failures
VERBOSE=1 sh test.sh emphasis # run one category
printf '# hi\n' | ./cdjot     # manual test
```

**Shell gotcha:** The Bash tool escapes `!` to `\!`, which cdjot sees as a backslash escape. For testing images, write to a file first: `printf '\x21[alt](img.jpg)\n' | ./cdjot`

## Architecture

`cdjot.c` (~3200 lines) follows smu's recursive dispatch pattern:

- **`process(begin, end, newblock)`** — central dispatcher. Tries each parser in order. `newblock=1` enables block parsers; `newblock=0` restricts to inline. Negative return = block consumed (sets newblock for next iteration), positive = inline consumed.
- **Block parsers** (fire when newblock=1): `doattr`, `dorefdef`, `doheading`, `doblockquote`, `docodefence`, `dodiv`, `dothematicbreak`, `dotable`, `dodeflist`, `dolist`, `doparagraph`
- **Inline parsers** (fire when newblock=0): `dolinebreak`, `docode`, `dosurround`, `dolink`, `doautolink`, `doreplace`
- **Pre-scans** (run before `process`): `prescan` collects `[label]: url` definitions, `[^label]:` footnote definitions, and heading auto-refs

Key design decisions:
- Direct stdout output (no intermediate buffers except for list items, blockquotes, divs, and footnotes which need recursive processing)
- `{#id .class key=val}` attributes stored as pending globals, consumed by the next block parser; inline attrs pre-processed in `doparagraph` by transforming `word{attrs}` → `[word]{attrs}` (skips `]{attrs}` and `](url){attrs}` which are handled by `dolink` directly)
- List items collected into a buffer, then `process()` recurses on the buffer
- Tight/loose list detection: `dolist` pre-scans the entire list for blank lines before emitting any items, so all items get consistent `<p>` wrapping when loose
- Smart quotes use look-ahead stack simulation to match openers with closers
- `dosurround` handles emphasis (`_`/`*`), super/subscript (`^`/`~`), and insert/delete/mark (`{+`/`{-`/`{=`) with explicit `{`/`}` delimiters. Uses `inner_openers` counter for closest-opener-wins precedence
- Tables detect separator rows for alignment and headers; captions via `^` after blank line
- Footnotes use sequential numbering, multi-paragraph content, backlink injected in last paragraph

## Goals

- Simplicity: single file, no deps, suckless-influenced style
- Correctness: 268/268 tests passing

## Tests

- `test/*.test` — upstream tests from `jgm/djot.js`. Format: backtick-fenced blocks with `.` separating input from expected HTML
- `test/cdjot.test` — cdjot-specific tests for bugs found via corpus comparison (not upstream)
- Omitted upstream tests: symb.test, filters.test, sourcepos.test (N/A for HTML-only converter); 3 AST-format test blocks removed; 1 test adjusted for attribute emission order (no semantic effect)

## Corpus comparison

460 real-world djot files in `~/code/experiments/djot-corpus/` (6 projects: matklad, treeman, omikhleia, glaze, nhanb, mdbook-djot).
- `compare-api.js` — compares cdjot vs djot.js local API, diffs in `.diffs-api/`
