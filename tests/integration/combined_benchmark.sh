#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 COMBINED_BENCHMARK" >&2
    exit 2
fi

output=$(mktemp)
trap 'rm -f "$output"' EXIT HUP INT TERM

if ! "$1" >"$output" 2>&1; then
    cat "$output"
    exit 1
fi

cat "$output"
grep -Eq '^cpu[[:space:]]+.*overrun_events/ticks=[0-9]+/[0-9]+' "$output"
grep -Eq '^cpu[+]in-use[[:space:]]+.*overrun_events/ticks=[0-9]+/[0-9]+' "$output"
