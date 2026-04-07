#!/bin/sh
# Performance and memory regression tests for cdjot
#
# Requires: hyperfine (perf), valgrind (memory)
# Missing tools are skipped with a message.

set -e

TMPDIR="${TMPDIR:-/tmp}"
INPUT_1M="$TMPDIR/cdjot-bench-1m.txt"
INPUT_10M="$TMPDIR/cdjot-bench-10m.txt"
REPS=80

# --- Generate ~1MB input from test files ---
# Extract only the input sections (before the . separator) from test files
: > "$INPUT_1M"
for i in $(seq 1 $REPS); do
	for f in test/*.test; do
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
if command -v hyperfine >/dev/null 2>&1; then
	for input in "$INPUT_1M" "$INPUT_10M"; do
		label=$(basename "$input" .txt | sed 's/cdjot-bench-//')
		echo "=== Performance ($label) ==="
		if command -v djot >/dev/null 2>&1; then
			hyperfine --warmup 3 --max-runs 10 \
				"./cdjot < $input > /dev/null" \
				"djot < $input > /dev/null"
		else
			echo "(install @djot/djot globally for comparison benchmark)"
			hyperfine --warmup 3 --max-runs 10 "./cdjot < $input > /dev/null"
		fi
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
