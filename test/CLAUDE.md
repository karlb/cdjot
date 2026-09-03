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

That is for divergences upstream has not settled. Where the spec does
settle it and cdjot is the wrong one, do the opposite: no case. Pinning
our output would assert a known bug and fail the day someone repairs
it. Record it at the code instead (see the cross-kind note on
`dosurround`'s scan, for `*a _b *c_ d*`).

## Deciding a case is redundant

Open the upstream case and check that it asserts the same behaviour on
the same construct. That is the bar. It is a judgment call, and the cost
of getting it wrong is a regression that corpus comparison and the
proptests are likely to catch anyway — so make the call and move on.

Read the case, though. Two shortcuts look like evidence and aren't:

- **Grepping for the construct.** `){` appears in `attributes.test` and
  `links_and_images.test`, but those cases are `[link](url){}` (empty
  attributes) and `![alt](img.jpg){.photo}` (an image). Neither pins an
  attribute attaching to an `<a>`.
- **Line coverage.** The URL-escaping cases execute no line the upstream
  suite doesn't already execute, yet substituting a non-escaping emitter
  at the reference-link, reference-image, and autolink call sites is
  caught by *none* of the upstream files. The lines run; the outcome
  isn't asserted.

Reading the case is necessary and still not sufficient. `smart.test:71`
and `:199` both assert that an unmatched `"` is a left quote — the same
sentence as the `x"y` case here — and pre-fix cdjot passes both, because
both sit at a word boundary where the old rule already opened correctly.
Asserting the same sentence is not discriminating the same failure.

So when a case exists because of a cdjot bug that got fixed, check the
fix. `git show 1028f9e^:cdjot.c > /tmp/old.c`, build it, run it against
the upstream files: if it passes them, they do not cover this case and
it stays. Build the old version, never reconstruct it — a hand-written
mutant of that bug broke eight upstream cases the real one never
touched, and that strawman is what made deleting `x"y` look safe.

This only works where history has the buggy version. A case that never
had a corresponding bug still rests on reading.

Mutation testing is the tie-breaker for a close call, not a gate — and
it only cuts one way. A mutant that this case alone kills proves the
case is **needed**. Finding no such mutant proves nothing: a sweep of
420 operator mutants rated the autolink escaping case redundant, while a
mutant it alone catches was already known. Reach for it to justify
keeping something, not to license deleting it.

Same standard for a case you're about to add: if you can't say what it
would catch that the rest of the suite wouldn't, don't add it.
