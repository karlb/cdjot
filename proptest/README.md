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
`OK=… BAD=… ERRS=…` summary, then exit 0. Some baseline noise is
expected (e.g., `wellformed.js` flags inputs that deliberately emit
unbalanced tags via `{=html}` raw HTML, and `diff_djotjs.js` flags
intentional cdjot-vs-djot.js differences in link-inside-emphasis
parsing and attribute emission order). The signal is *change in the
count after a code change*, not a clean run.

## Quick start

```sh
make fuzz-corpus            # populate fuzz/corpus/ with seeds
node proptest/wellformed.js fuzz/corpus/*
node proptest/attrsafe.js   fuzz/corpus/*
node proptest/idunique.js   fuzz/corpus/*

# diff against reference (one-time setup):
cd proptest && npm init -y && npm install @djot/djot && cd ..
SHOW=10 node proptest/diff_djotjs.js fuzz/corpus/*
```

For broader coverage, grow the corpus first via `make fuzz-run`
(libFuzzer) and re-run the property tests on the expanded set.

## History

Both checks have caught real bugs:

- `wellformed.js` found block parsers (`<section>`, `<h3>`)
  emitting inside an unclosed `<h1>` when heading content
  contained `\n\n` — fixed in commit `4a8bd15`.
- `attrsafe.js` found NUL-byte truncation of attribute values and
  missing HTML escaping of id/class chars — fixed in `4617512`.
