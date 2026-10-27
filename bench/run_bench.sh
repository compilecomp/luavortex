#!/bin/sh
# bench/run_bench.sh — wall-clock timing of the compiled Lua workloads.
# usage: run_bench.sh <path-to-lvxc> [repetitions]
set -e
LVXC="${1:-./build/bin/lvxc}"
REPS="${2:-3}"
DIR="$(cd "$(dirname "$0")" && pwd)"

for src in "$DIR"/*.lua; do
    name="$(basename "$src" .lua)"
    best=""
    i=0
    while [ "$i" -lt "$REPS" ]; do
        start=$(date +%s%N)
        "$LVXC" run "$src" > /dev/null
        end=$(date +%s%N)
        ms=$(( (end - start) / 1000000 ))
        if [ -z "$best" ] || [ "$ms" -lt "$best" ]; then best=$ms; fi
        i=$((i + 1))
    done
    echo "$name: ${best} ms (best of $REPS)"
done
