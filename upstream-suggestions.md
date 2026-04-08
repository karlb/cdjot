# Upstream suggestions for djot.js

Findings from comparing cdjot output against djot.js v0.3.2 on 250 test
blocks plus 4 standalone djot documents (syntax.md, cheatsheet.md,
quickstart.md, bench/readme.dj).

## JS djot bugs

These cases produce output that disagrees with both the djot spec and
the expected output in djot.js's own test files.

### 1. Emphasis delimiters inside link URLs close emphasis

Test file: `test/links_and_images.test` blocks 20-23

The test file says "the star inside the destination is protected from
closing emphasis" and expects:

    *[closed](hello*)  →  <p>*<a href="hello*">closed</a></p>

But djot.js v0.3.2 produces:

    <p><strong>[closed](hello</strong>)</p>

Same issue with underscores:

    _[link](http://example.com?foo_bar=1), more text_

Expected (per test file): `<em><a href="...">link</a>, more text</em>`
Actual: emphasis broken by `_` inside URL.

### 2. Attributes applied despite blank line separation

Test file: `test/attributes.test` block 15

The test file says "If there is a blank line between, the attributes
won't be applied" and expects:

    {#id .class}
    ↵
    A paragraph  →  <p>A paragraph</p>

The spec says: "put the attributes on the line immediately before the
block."

But djot.js v0.3.2 applies the attributes through the blank line:

    <p id="id" class="class">A paragraph</p>

### 3. Heading auto-ID includes footnote reference number

Test file: `test/headings.test` block 14

The spec explicitly states: "For example, `# Introduction[^1]`
generates the identifier `Introduction`, not `Introduction1`."

The test file expects `id="Introduction"`. But djot.js produces
`id="Introduction1"`.

### 4. Task lists with different markers merged into one list

Test file: `test/task_lists.test` block 3

The spec says: "changing ordered list style or bullet will stop one list
and start a new one." The test file expects three separate `<ul>` lists
for `- [ ]`, `+ [ ]`, `* [ ]` markers. But djot.js merges them into a
single list.

## Proposed new test cases

These spec behaviors are described in prose (syntax.md) but have no
corresponding test blocks. Adding them would catch regressions.

### Verbatim space stripping with multi-backtick delimiters

The spec says: "If the content starts or ends with a backtick character,
a single space is removed between the opening or closing backticks and
the content."

The existing test for this (`test/verbatim.test` block 6) only tests
single-backtick delimiters wrapping double-backtick content. The reverse
(multi-backtick delimiters wrapping single-backtick content) is untested.

```
`` `foo` ``
.
<p><code>`foo`</code></p>
```

```
`` ` ``
.
<p><code>`</code></p>
```

### Emphasis precedence (closest opener wins)

The spec says: "When there are multiple openers that might be matched
with a given closer, the closest one is used." The spec includes the
example `*not strong *strong*` but there is no test block for it.

```
*not strong *strong*
.
<p>*not strong <strong>strong</strong></p>
```

### Nested emphasis with non-consecutive closers

The spec says "Emphasis can be nested" and shows `__emphasis inside_
emphasis_` as an example. There is no test block for this pattern
(nested emphasis where closers are individual, not a consecutive run).

```
__emphasis inside_ emphasis_
.
<p><em><em>emphasis inside</em> emphasis</em></p>
```

```
*a *b* c*
.
<p><strong>a <strong>b</strong> c</strong></p>
```

### URL line continuation with leading whitespace

The spec says: "The URL may be split over multiple lines; in that case,
the line breaks and any leading and trailing space is ignored, and the
lines are concatenated together."

The existing test (`test/links_and_images.test` block 7) tests basic
line continuation without leading whitespace. Leading whitespace on
continuation lines is untested.

```
[link](http://example.com?n=123
    456)
.
<p><a href="http://example.com?n=123456">link</a></p>
```

## Spec clarification suggestions

### Heading auto-ID: punctuation handling for `/` and `'`

The spec says auto-IDs are formed by "removing punctuation (other than
`_` and `-`), replacing spaces with `-`". This is clear, but
implementations disagree on edge cases:

- `/` in headings: should it be removed (→ `Emphasisstrong`) or
  replaced with `-` (→ `Emphasis-strong`)? The spec says "removed."
- `'` (apostrophe): should it be removed? The spec says yes (it's
  punctuation, not `_` or `-`). JS djot keeps it.

An explicit example in the spec would help:

    ## Emphasis/strong  →  id="Emphasisstrong"
    ## That's all       →  id="Thats-all"
