# test/

The goal is one true test suite for djot implementations. Every case here
should be either **upstream's** or **cdjot-specific**, with nothing in a
third bucket of "local copy of a case that belongs upstream".

## Layout

- `*.test` except `cdjot.test` — copied byte-verbatim from `jgm/djot.js`.
  **Never edit these.** A local edit silently forks the suite; fix cdjot
  or open a PR upstream instead.
- `cdjot.test` — cases upstream does not have and should not have.

Omitted upstream files: `symb.test` (AST output, no standard HTML
mapping), `filters.test` and `sourcepos.test` (N/A for a stdin-to-stdout
converter).

## Format

Backtick-fenced blocks, `.` on its own line separating input from
expected HTML. A fence carrying an info string (`` ``` a ``) holds an
AST-format block with no HTML to compare, and the runner skips it.

    make test                     # whole suite
    VERBOSE=1 sh test.sh          # show a diff on failure
    VERBOSE=1 sh test.sh emphasis # one file

Don't hardcode the test count in prose — it goes stale on every sync.

## Syncing from upstream

`~/code/github/djot.js` has `upstream` = `jgm/djot.js`, `origin` = the
fork. Fetch, copy the changed files over verbatim, then run the suite:
new upstream cases are how cdjot finds out it is wrong.

## Where a new case goes

Ask what it pins, not what found it. A case belongs in `cdjot.test` only
if upstream would refuse it or already covers the behaviour some other
way. jgm's review of PR #137 gives the criteria:

- **Parsing, not rendering.** "This is more a general point about HTML
  rendering than about parsing. I'd like these cases to center on
  parsing." HTML-escaping cases (`<` and `&` reaching an `href`/`src`)
  are cdjot-local for this reason.
- **Attribute behaviour goes in `attributes.test`,** and only if it says
  something the general rule doesn't. `attributes.test` already opens
  with "an inline attribute attaches to the preceding element"; a case
  showing that same rule on one more element type is "nothing special
  about mark, insert, or delete" and gets omitted.
- **Lean prose.** Two sentences. On being offered "or reading past the
  end of the buffer": "just one more way to fail."
- **Trimmed PRs land.** #137 was cut down to the undisputed cases to get
  it merged; contested ones move to their own PR.

Hold anything whose correct answer is unsettled upstream — pinning it
freezes a behaviour that may be a bug. Open divergences belong in
`cdjot.test` with the issue number in the prose, so they fail loudly if
someone "fixes" them (see the heading-attribute case, jgm/djot.js#144).

## Proving a case is redundant

Before deleting anything as "already covered upstream", **prove it by
mutation**. A case is redundant only if every mutant it kills is also
killed by the upstream files alone.

Two cheaper signals both give wrong answers here, and have:

- **Grepping for the construct.** `){` appears in `attributes.test` and
  `links_and_images.test`, but those cases are `[link](url){}` (empty
  attributes) and `![alt](img.jpg){.photo}` (an image). Neither pins an
  attribute attaching to an `<a>`.
- **Line coverage.** The URL-escaping cases execute no line the upstream
  suite doesn't already execute, yet substituting a non-escaping emitter
  at the reference-link, reference-image, and autolink call sites is
  caught by *none* of the upstream files. The lines run; the outcome
  isn't asserted.

The same rule applies to a case you are about to add: build the mutant
it is supposed to catch and confirm the suite misses it without the
case and catches it with. If nothing distinguishes it, don't add it.
