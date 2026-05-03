# Property tests

Oracle-based tests that check semantic invariants of cdjot's output
on arbitrary input. Designed to be paired with the libFuzzer corpus
(`fuzz/corpus/`) — the fuzzer drives input variety, the property
tests provide an oracle that catches malformed output the fuzzer
itself can't see.

## Tests

| script | invariant | runtime deps |
|---|---|---|
| `wellformed.js` | every paired HTML tag (`<p>`, `<section>`, `<a>`, …) is closed in LIFO order | node |
| `attrsafe.js` | no attribute value contains a literal `<` | node |
| `idunique.js` | every `id="X"` is unique and every `href="#X"` resolves to an `id="X"` | node |
| `diff_djotjs.js` | output equals `@djot/djot` reference output | node + `npm install @djot/djot` |

All scripts are informational — they print failures and an
`OK=… BAD=… ERRS=…` summary, then exit 0. A clean run is not
expected: `wellformed.js` flags inputs that deliberately emit
unbalanced tags via `{=html}` raw HTML, and `diff_djotjs.js` flags
intentional cdjot-vs-djot.js differences in link-inside-emphasis
parsing and attribute emission order. The right signal is *which
specific inputs fail*, not the count — use `make fuzz-baseline` to
snapshot the current set and `make fuzz-check` to diff against it.

## Quick start

```sh
make fuzz-corpus            # populate fuzz/corpus/ with seeds
make fuzz-baseline          # snapshot the current BAD filenames
make fuzz-check             # later: diff vs baseline (clean = no new failures)
```

To run an individual script directly (e.g., to inspect failure
output rather than just filenames):

```sh
SHOW=20 node proptest/wellformed.js fuzz/corpus/*
SHOW=20 node proptest/attrsafe.js   fuzz/corpus/*
SHOW=20 node proptest/idunique.js   fuzz/corpus/*
```

The differential test against the reference implementation needs a
one-time `npm install` and isn't wired into `fuzz-check` (its output
shape differs):

```sh
cd proptest && npm init -y && npm install @djot/djot && cd ..
SHOW=10 node proptest/diff_djotjs.js fuzz/corpus/*
```

The CDJOT path defaults to `<repo>/cdjot` regardless of CWD, so any
of the above can be invoked from `proptest/`, `fuzz/`, or repo root.

## History

Property checks have caught real bugs:

- `wellformed.js` found block parsers (`<section>`, `<h3>`)
  emitting inside an unclosed `<h1>` when heading content
  contained `\n\n` — fixed in commit `4a8bd15`.
- `attrsafe.js` found NUL-byte truncation of attribute values and
  missing HTML escaping of id/class chars — fixed in `4617512`.
- `idunique.js` surfaced duplicate `id="fnrefN"` when the same
  footnote is referenced more than once — see
  `../findings/duplicate-fnref-on-repeat-reference.md`. Reference
  djot.js shares the behavior, so this is a spec-level issue.
