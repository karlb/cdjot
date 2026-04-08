# cdjot

A single-file C99 [djot](https://djot.net/) to HTML converter. ~3000 lines, no dependencies beyond libc. Reads stdin, writes stdout.

Djot is a markup language designed as a principled replacement for Markdown. It eliminates Markdown's ambiguities and corner cases while adding features like definition lists, footnotes, attributes, and more — all backed by a complete specification.

cdjot converts djot to HTML. That's all it does.

Similar in spirit to [smu](https://github.com/karlb/smu), but backed by a real spec.

## Build

    make

## Usage

    printf '# Hello\n\nA *paragraph*.\n' | ./cdjot

    cat input.dj | cdjot > output.html

## Install

    make install              # installs to /usr/local/bin
    make install PREFIX=/usr  # or a custom prefix

## Test

Passes 256 of 258 tests from the [djot.js](https://github.com/jgm/djot.js) suite (the remaining 2 test source-position and filter features that are outside the scope of a pure converter):

    make test
