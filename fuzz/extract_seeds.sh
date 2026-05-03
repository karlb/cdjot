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

# Optionally pull in real-world djot files. Set CDJOT_CORPUS to a directory
# (defaults to $HOME/code/experiments/djot-corpus); skipped silently if absent.
extra="${CDJOT_CORPUS:-$HOME/code/experiments/djot-corpus}"
if [ -d "$extra" ]; then
	rm -f "$out"/wild_*.dj
	idx=0
	find "$extra" \( -name '*.dj' -o -name '*.djot' \) -type f | while IFS= read -r src; do
		idx=$((idx + 1))
		cp "$src" "$(printf '%s/wild_%05d.dj' "$out" "$idx")"
	done
fi

count=$(find "$out" -name '*.dj' | wc -l | tr -d ' ')
echo "Extracted $count seed inputs to $out"
