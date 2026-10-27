#!/bin/sh
# tests/run_tests.sh — executes every test case through lvxc and compares
# stdout against the expected output file (cases/<name>.out).
# usage: run_tests.sh <path-to-lvxc>
set -e

LVXC="${1:-./build/lvxc}"
DIR="$(cd "$(dirname "$0")" && pwd)"
CASES="$DIR/cases"
FAILED=0
PASSED=0

for src in "$CASES"/*.lua; do
    name="$(basename "$src" .lua)"
    expected="$CASES/$name.out"
    actual="$(mktemp)"
    # lvxc prints the chunk result ("=> ...") and counters ("tier T0 ...")
    # after guest output; the cases compare guest output only.
    if ! "$LVXC" run "$src" 2>&1 \
            | grep -v '^=> ' | grep -v '^tier T0 ' > "$actual"; then
        echo "FAIL $name (lvxc exited non-zero)"
        cat "$actual"
        rm -f "$actual"
        FAILED=$((FAILED + 1))
        continue
    fi
    if diff -u "$expected" "$actual" > /dev/null 2>&1; then
        echo "PASS $name"
        PASSED=$((PASSED + 1))
    else
        echo "FAIL $name (output mismatch)"
        diff -u "$expected" "$actual" || true
        FAILED=$((FAILED + 1))
    fi
    rm -f "$actual"
done

echo "$PASSED passed, $FAILED failed"
[ "$FAILED" -eq 0 ]
