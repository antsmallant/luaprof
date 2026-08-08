#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
output=$(mktemp)
trap 'rm -f "$output"' EXIT HUP INT TERM

LUAPROF_MODULE_CPATH=${LUAPROF_MODULE_CPATH:-"$repo_root/build/skynet/?.so"}
LUAPROF_SKYNET_SERVICE=${LUAPROF_SKYNET_SERVICE:-luaprof_smoke}
LUAPROF_EXPECT_OUTPUT=${LUAPROF_EXPECT_OUTPUT:-"luaprof skynet smoke: ok"}
export LUAPROF_MODULE_CPATH
export LUAPROF_SKYNET_SERVICE

cd "$repo_root/integration/skynet"
if ! timeout 10s ./skynet "$repo_root/examples/skynet/config" >"$output" 2>&1; then
    cat "$output"
    exit 1
fi

cat "$output"
grep -Fq "$LUAPROF_EXPECT_OUTPUT" "$output"
grep -Eq 'overrun_events/ticks=[0-9]+/[0-9]+' "$output"
