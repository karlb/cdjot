#!/bin/sh
# Performance and memory regression tests for cdjot
#
# Requires: hyperfine (perf), valgrind (memory)
# Missing tools are skipped with a message.

set -e

TMPDIR="${TMPDIR:-/tmp}"
INPUT="$TMPDIR/cdjot-bench-input.txt"
REPS=80

# --- Generate ~1MB input from test files ---
# Extract only the input sections (before the . separator) from test files
: > "$INPUT"
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
done >> "$INPUT"

inputsize=$(wc -c < "$INPUT" | tr -d ' ')
echo "Generated input: ${inputsize} bytes"
echo ""

fail=0

# --- Performance benchmark (hyperfine) ---
if command -v hyperfine >/dev/null 2>&1; then
	echo "=== Performance ==="
	if command -v npx >/dev/null 2>&1; then
		hyperfine --warmup 3 --max-runs 10 \
			"./cdjot < $INPUT > /dev/null" \
			"npx --yes @djot/djot < $INPUT > /dev/null"
	else
		hyperfine --warmup 3 --max-runs 10 "./cdjot < $INPUT > /dev/null"
	fi
	echo ""
else
	echo "SKIP: hyperfine not found (install for performance benchmarks)"
	echo ""
fi

# --- Memory leak check (valgrind) ---
if command -v valgrind >/dev/null 2>&1; then
	echo "=== Memory leaks ==="
	if valgrind --leak-check=full --errors-for-leak-kinds=all --error-exitcode=99 \
		./cdjot < "$INPUT" > /dev/null 2>"$TMPDIR/cdjot-valgrind.log"; then
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
	echo "=== Peak memory ==="
	valgrind --tool=massif --pages-as-heap=yes --massif-out-file="$TMPDIR/cdjot-massif.out" \
		./cdjot < "$INPUT" > /dev/null 2>/dev/null
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

rm -f "$INPUT"

if [ "$fail" -ne 0 ]; then
	echo "BENCH: some checks failed"
	exit 1
fi
