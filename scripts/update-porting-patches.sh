#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
generator="$repo_root/scripts/generate-porting-patch.sh"
patch_dir="$repo_root/patches"
temp_dir=$(mktemp -d "${TMPDIR:-/tmp}/luaprof-porting-patches.XXXXXX")

cleanup() {
    rm -rf "$temp_dir"
}
trap cleanup EXIT HUP INT TERM

generate() {
    target=$1
    filename=$2
    output="$temp_dir/$filename"

    "$generator" "$target" >"$output"
    if ! test -s "$output"; then
        echo "generated patch is empty: $target" >&2
        exit 1
    fi
}

generate lua46 lua-5.4.6.patch
generate lua54 lua-5.4.8.patch
generate lua55 lua-5.5.0.patch
generate skynet skynet.patch

mkdir -p "$patch_dir"
for filename in \
    lua-5.4.6.patch \
    lua-5.4.8.patch \
    lua-5.5.0.patch \
    skynet.patch
do
    mv "$temp_dir/$filename" "$patch_dir/$filename"
done

echo "updated porting patches in $patch_dir"
