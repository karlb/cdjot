# cdjot

A single-file C99 [djot](https://djot.net/) to HTML converter. ~3000 lines, no dependencies beyond libc.

Djot is a markup language designed as a principled replacement for Markdown. It eliminates Markdown's ambiguities and corner cases while adding features like definition lists, footnotes, attributes, and more.

Similar in spirit to [smu](https://github.com/karlb/smu), but backed by a real spec. No features beyond the conversion itself (no AST, no filters, etc).

## Build

    make

## Usage

    printf '# Hello\n\nA *paragraph*.\n' | ./cdjot

    cat input.dj | cdjot > output.html

## Install

    make install              # installs to /usr/local/bin
    make install PREFIX=/usr  # or a custom prefix

## Test

Passes the [djot.js](https://github.com/jgm/djot.js) test suite, minus the files covering source positions, filters, and AST symbols — features outside the scope of a pure converter — plus cdjot's own regression tests:

    make test
