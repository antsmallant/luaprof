#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
flamegraph=${1:?missing pprof-flamegraph path}
temp_dir=$(mktemp -d "${TMPDIR:-/tmp}/luaprof-flamegraph-test.XXXXXX")

cleanup() {
    rm -rf "$temp_dir"
}
trap cleanup EXIT HUP INT TERM

cpu_svg="$temp_dir/cpu.svg"
heap_svg="$temp_dir/heap.svg"

"$flamegraph" --output "$cpu_svg" "$repo_root/build/thread-vm-cpu.pb.gz"
"$flamegraph" --sample=inuse_space --output "$heap_svg" \
    "$repo_root/build/skynet-heap.pb.gz"

for svg in "$cpu_svg" "$heap_svg"; do
    test -s "$svg"
    grep -Fq '<svg ' "$svg"
    if grep -Fq '<script' "$svg"; then
        echo "static flame graph unexpectedly contains script: $svg" >&2
        exit 1
    fi
done

grep -Fq 'sample: cpu;' "$cpu_svg"
grep -Fq 'calculate_orders' "$cpu_svg"
grep -Fq 'tostring [luaB_tostring]' "$cpu_svg"
grep -Fq 'sample: inuse_space;' "$heap_svg"
grep -Fq 'build_retained_cache' "$heap_svg"

echo "luaprof static pprof flame graph: ok"
