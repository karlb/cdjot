#!/bin/sh
# Extract seed inputs from test/*.test files into fuzz/corpus/.
# Each test's input section becomes one corpus file; this gives the fuzzer
# a high-quality starting set covering every documented djot construct.

set -eu

here=$(dirname "$0")
out="$here/corpus"
mkdir -p "$out"
rm -f "$out"/seed_*.dj

count=0
for f in "$here"/../test/*.test; do
	fname=$(basename "$f" .test)
	awk -v out="$out" -v base="$fname" '
		BEGIN { state="outside"; idx=0 }
		{
			if (state == "outside") {
				if ($0 ~ /^`+$/) {
					fmarklen = length($0)
					state = "input"
					idx++
					path = sprintf("%s/seed_%s_%03d.dj", out, base, idx)
					next
				}
			} else if (state == "input") {
				if ($0 == ".") { state = "expected"; next }
				printf "%s\n", $0 >> path
				next
			} else if (state == "expected") {
				if ($0 ~ /^`+$/ && length($0) >= fmarklen) {
					close(path); state = "outside"; next
				}
			}
		}
	' "$f"
done

count=$(find "$out" -name 'seed_*.dj' | wc -l | tr -d ' ')
echo "Extracted $count seed inputs to $out"
