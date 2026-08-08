#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: $0 LUA_BIN MODULE_DIR OUTPUT_DIR" >&2
    exit 2
fi

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
lua_bin=$1
module_dir=$2
output_dir=$3
output=$(mktemp)
trap 'rm -f "$output"' EXIT HUP INT TERM

if ! LUA_CPATH="$module_dir/?.so;;" "$lua_bin" \
    "$repo_root/examples/thread_vm/profile.lua" \
    "$output_dir/thread-vm-cpu.pb.gz" \
    "$output_dir/thread-vm-heap.pb.gz" >"$output" 2>&1; then
    cat "$output"
    exit 1
fi

cat "$output"
grep -Eq '^CPU samples=[0-9]+ Lua/C/GC=[0-9]+/[0-9]+/[0-9]+ overrun_events/ticks=[0-9]+/[0-9]+ dropped=[0-9]+$' "$output"
