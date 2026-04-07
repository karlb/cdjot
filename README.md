# cdjot

A single-file C99 [djot](https://djot.net/) to HTML converter. Reads stdin, writes stdout. No dependencies beyond libc.

## Build

    make

## Usage

    printf '# Hello\n\nA *paragraph*.\n' | ./cdjot

## Install

    make install              # installs to /usr/local/bin
    make install PREFIX=/usr  # or a custom prefix

## Test

The test suite uses 256 tests from [jgm/djot.js](https://github.com/jgm/djot.js):

    make test

Show diffs on failure:

    VERBOSE=1 sh test.sh

Run a single category:

    VERBOSE=1 sh test.sh emphasis
