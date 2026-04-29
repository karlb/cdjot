#!/bin/sh
# Performance and memory regression tests for cdjot
#
# Requires: hyperfine (perf), valgrind (memory)
# Missing tools are skipped with a message.
#
# Optional: realistic benchmark files (pandoc-manual.dj, tartan-wikipedia.dj)
# from https://github.com/dlc-01/djot-implementations. Set BENCH_FILES to a
# directory containing them and they will be used in addition to the
# synthetic input. Files are not bundled (GPL / CC-BY-SA encumbered).

set -e

TMPDIR="${TMPDIR:-/tmp}"
INPUT_1M="$TMPDIR/cdjot-bench-1m.txt"
INPUT_10M="$TMPDIR/cdjot-bench-10m.txt"
REPS=80

# --- Generate ~1MB input from test files ---
# Extract only the input sections (before the . separator) from test files.
# Skip fenced_divs.test: it contains spec examples of intentionally-malformed
# fences (open with N colons, "close" with fewer) which, when concatenated and
# repeated, cause O(N^2) scan-to-EOF behavior that swamps real measurements.
: > "$INPUT_1M"
for i in $(seq 1 $REPS); do
	for f in test/*.test; do
		case "$f" in *fenced_divs.test) continue ;; esac
		state="outside"
		while IFS= read -r line; do
			case "$state" in
			outside)
				case "$line" in
				\`\`\`*) state="input" ;;
				esac
				;;
			input)
				if [ "$line" = "." ]; then
					state="outside"
				else
					printf '%s\n' "$line"
				fi
				;;
			esac
		done < "$f"
	done
done >> "$INPUT_1M"

# --- Generate ~10MB input by repeating 1MB input ---
: > "$INPUT_10M"
for i in $(seq 1 10); do
	cat "$INPUT_1M"
done >> "$INPUT_10M"

echo "Generated input: $(wc -c < "$INPUT_1M" | tr -d ' ') bytes (1M), $(wc -c < "$INPUT_10M" | tr -d ' ') bytes (10M)"
echo ""

fail=0

# --- Performance benchmark (hyperfine) ---
# Build a list of additional implementations to compare against
# (djot.js, jotdown — both optional, only included if present).
hf_extras=""
if command -v djot >/dev/null 2>&1; then
	hf_extras="$hf_extras djot"
fi
if command -v jotdown >/dev/null 2>&1; then
	hf_extras="$hf_extras jotdown"
fi

if command -v hyperfine >/dev/null 2>&1; then
	# Realistic inputs first, if BENCH_FILES is set and the files exist.
	if [ -n "$BENCH_FILES" ]; then
		for name in pandoc-manual.dj tartan-wikipedia.dj; do
			[ -f "$BENCH_FILES/$name" ] || continue
			echo "=== Performance ($name) ==="
			cmds="./cdjot < $BENCH_FILES/$name > /dev/null"
			for impl in $hf_extras; do
				cmds="$cmds|$impl < $BENCH_FILES/$name > /dev/null"
			done
			IFS='|'; set -- $cmds; unset IFS
			hyperfine --warmup 3 --max-runs 20 "$@"
			echo ""
		done
	fi
	for input in "$INPUT_1M" "$INPUT_10M"; do
		label=$(basename "$input" .txt | sed 's/cdjot-bench-//')
		echo "=== Performance ($label) ==="
		cmds="./cdjot < $input > /dev/null"
		for impl in $hf_extras; do
			cmds="$cmds|$impl < $input > /dev/null"
		done
		IFS='|'; set -- $cmds; unset IFS
		hyperfine --warmup 3 --max-runs 10 "$@"
		echo ""
	done
	echo ""
else
	echo "SKIP: hyperfine not found (install for performance benchmarks)"
	echo ""
fi

# --- Memory leak check (valgrind) ---
if command -v valgrind >/dev/null 2>&1; then
	echo "=== Memory leaks (1M input) ==="
	if valgrind --leak-check=full --errors-for-leak-kinds=all --error-exitcode=99 \
		./cdjot < "$INPUT_1M" > /dev/null 2>"$TMPDIR/cdjot-valgrind.log"; then
		echo "OK: no memory leaks"
	else
		exitcode=$?
		if [ "$exitcode" -eq 99 ]; then
			echo "FAIL: memory leaks detected"
			cat "$TMPDIR/cdjot-valgrind.log"
			fail=1
		else
			echo "FAIL: valgrind error (exit $exitcode)"
			cat "$TMPDIR/cdjot-valgrind.log"
			fail=1
		fi
	fi
	echo ""

	# --- Peak memory check (valgrind massif) ---
	echo "=== Peak memory (1M input) ==="
	valgrind --tool=massif --pages-as-heap=yes --massif-out-file="$TMPDIR/cdjot-massif.out" \
		./cdjot < "$INPUT_1M" > /dev/null 2>/dev/null
	peak=$(grep mem_heap_B "$TMPDIR/cdjot-massif.out" | sort -t= -k2 -n | tail -1 | cut -d= -f2)
	peak_mb=$(echo "$peak / 1048576" | bc)
	echo "Peak memory: ${peak} bytes (~${peak_mb} MB)"
	if [ "$peak_mb" -gt 10 ]; then
		echo "FAIL: peak memory exceeds 10 MB"
		fail=1
	else
		echo "OK: peak memory within 10 MB limit"
	fi
	echo ""

	rm -f "$TMPDIR/cdjot-valgrind.log" "$TMPDIR/cdjot-massif.out"
else
	echo "SKIP: valgrind not found (install for memory checks)"
	echo ""
fi

rm -f "$INPUT_1M" "$INPUT_10M"

if [ "$fail" -ne 0 ]; then
	echo "BENCH: some checks failed"
	exit 1
fi
