#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
output=$(mktemp)
trap 'rm -f "$output"' EXIT HUP INT TERM

LUAPROF_MODULE_CPATH=${LUAPROF_MODULE_CPATH:-"$repo_root/build/skynet/?.so"}
export LUAPROF_MODULE_CPATH

cd "$repo_root/integration/skynet"
if ! timeout 10s ./skynet "$repo_root/examples/skynet/config" >"$output" 2>&1; then
    cat "$output"
    exit 1
fi

cat "$output"
grep -Fq "luaprof skynet smoke: ok" "$output"
