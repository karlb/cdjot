# Upstream suggestions for djot.js

Findings from comparing cdjot against djot.js. Initial comparison was
against the npm-published v0.3.2 CLI, which revealed several
discrepancies. On investigation, all of these have already been fixed
in the djot.js repo post-0.3.2 (commits 713a417, cc1b4e5, df32bf3,
7892004) but haven't been released yet.

What remains are proposed test cases for spec behaviors that are
described in syntax.md prose but have no corresponding test blocks.
djot.js already handles all of these correctly — these are just
coverage gaps.

## Proposed new test cases

### Verbatim space stripping with multi-backtick delimiters

The spec says: "If the content starts or ends with a backtick character,
a single space is removed between the opening or closing backticks and
the content."

The existing test (`test/verbatim.test` block 6) only tests
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

## Spec clarification suggestion

### Heading auto-ID: punctuation handling for `/` and `'`

The spec says auto-IDs are formed by "removing punctuation (other than
`_` and `-`), replacing spaces with `-`". This is clear, but
implementations may disagree on edge cases:

- `/` in headings: should it be removed (→ `Emphasisstrong`) or
  replaced with `-` (→ `Emphasis-strong`)? The spec says "removed."
- `'` (apostrophe): should it be removed? The spec says yes (it's
  punctuation, not `_` or `-`).

An explicit example in the spec would help:

    ## Emphasis/strong  →  id="Emphasisstrong"
    ## That's all       →  id="Thats-all"
