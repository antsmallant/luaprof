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
interactive_svg="$temp_dir/interactive.svg"
skynet_svg="$temp_dir/skynet.svg"

LP_PPROF_OUTPUT_DIR="$temp_dir" LP_KEEP_PPROF_FILES=1 \
    "$repo_root/build/pprof-exporter-test"
(
    cd "$repo_root/tools/pprof-flamegraph"
    LP_EXPORTED_PROFILE_DIR="$temp_dir" LP_EXAMPLE_PROFILE_DIR="$repo_root/build" \
        go test -run 'TestExportedCPUQuality|TestExampleCPUQuality' -count=1 ./...
)
go tool pprof -comments "$temp_dir/luaprof-pprof-cpu.pb.gz" > "$temp_dir/comments.txt"
grep -Fxq 'luaprof.metadata.version=1' "$temp_dir/comments.txt"
grep -Fxq 'luaprof.cpu.overrun_events=1' "$temp_dir/comments.txt"
grep -Fxq 'luaprof.cpu.overrun_ticks=3' "$temp_dir/comments.txt"
go tool pprof -top "$temp_dir/luaprof-pprof-cpu.pb.gz" > "$temp_dir/top.txt"
grep -Fq '50ms' "$temp_dir/top.txt"
go tool pprof -comments "$temp_dir/luaprof-pprof-empty.pb.gz" > "$temp_dir/empty.txt"
grep -Fxq 'luaprof.cpu.samples=0' "$temp_dir/empty.txt"
grep -Fxq 'luaprof.cpu.overrun_ticks=3' "$temp_dir/empty.txt"

"$flamegraph" --output "$cpu_svg" "$repo_root/build/thread-vm-cpu.pb.gz"
"$flamegraph" --sample=inuse_space --output "$heap_svg" \
    "$repo_root/build/skynet-heap.pb.gz"
"$flamegraph" --interactive --output "$interactive_svg" \
    "$repo_root/build/thread-vm-cpu.pb.gz"
"$flamegraph" --output "$skynet_svg" "$repo_root/build/skynet-cpu.pb.gz"

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
test -s "$interactive_svg"
grep -Fq '<script type="application/ecmascript">' "$interactive_svg"
grep -Fq 'id="search"' "$interactive_svg"
grep -Fq 'id="reset"' "$interactive_svg"
grep -Fq 'data-name="calculate_orders"' "$interactive_svg"
grep -Fq 'Ctrl-F' "$interactive_svg"
for svg in "$cpu_svg" "$interactive_svg" "$skynet_svg"; do
    grep -Fq 'CPU quality: samples=' "$svg"
    grep -Fq 'sample_hz=100 Hz' "$svg"
    grep -Fq 'overrun_events=' "$svg"
    grep -Fq 'overrun_ticks=' "$svg"
done
if grep -Fq 'CPU quality:' "$heap_svg"; then
    echo 'memory flame graph unexpectedly shows CPU quality' >&2
    exit 1
fi

echo "luaprof static and interactive pprof flame graph: ok"
