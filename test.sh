#!/bin/sh
# Test runner for djot-to-HTML converter
# Parses .test files: blocks delimited by backtick fences, . separates input from expected
#
# Tests are from jgm/djot.js. Omitted tests:
# - symb.test: uses AST output format, not HTML (djot symbols have no standard HTML mapping)
# - attributes.test #35 (2 blocks): uses AST output format for multi-line attribute edge cases
# - filters.test, sourcepos.test: not applicable to a simple stdin-to-stdout converter

pass=0
fail=0
total=0
verbose=${VERBOSE:-0}
filter="${1:-}"

is_backtick_fence() {
	case "$1" in
	\`\`\`*)
		# check it's all backticks
		clean=$(printf '%s' "$1" | tr -d '`')
		if [ -z "$clean" ]; then
			return 0
		fi
		;;
	esac
	return 1
}

fence_len() {
	printf '%s' "$1" | wc -c | tr -d ' '
}

for f in test/*.test; do
	fname=$(basename "$f" .test)

	if [ -n "$filter" ] && [ "$fname" != "$filter" ]; then
		continue
	fi

	testnum=0
	state="outside"
	input=""
	expected=""
	fencemark=""
	fmarklen=0

	while IFS= read -r line || [ -n "$line" ]; do
		case "$state" in
		outside)
			if is_backtick_fence "$line"; then
				fencemark="$line"
				fmarklen=$(fence_len "$line")
				state="input"
				input=""
				expected=""
			fi
			;;
		input)
			if [ "$line" = "." ]; then
				state="expected"
			else
				if [ -n "$input" ]; then
					input="$input
$line"
				else
					input="$line"
				fi
			fi
			;;
		expected)
			# check for closing fence (same or longer backtick run)
			if is_backtick_fence "$line"; then
				clen=$(fence_len "$line")
				if [ "$clen" -ge "$fmarklen" ]; then
					testnum=$((testnum + 1))
					total=$((total + 1))
					got=$(printf '%s\n' "$input" | ./djot)
					if [ "$got" = "$expected" ]; then
						pass=$((pass + 1))
					else
						fail=$((fail + 1))
						echo "FAIL: $fname #$testnum"
						if [ "$verbose" = "1" ]; then
							echo "  input:"
							printf '%s\n' "$input" | sed 's/^/    /'
							echo "  expected:"
							printf '%s\n' "$expected" | sed 's/^/    /'
							echo "  got:"
							printf '%s\n' "$got" | sed 's/^/    /'
							echo ""
						fi
					fi
					state="outside"
					continue
				fi
			fi
			if [ -n "$expected" ]; then
				expected="$expected
$line"
			else
				expected="$line"
			fi
			;;
		esac
	done < "$f"
done

echo ""
echo "$pass/$total passed, $fail failed"
[ "$fail" -eq 0 ]
