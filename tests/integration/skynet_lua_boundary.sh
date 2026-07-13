#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
skynet="$repo_root/integration/skynet/skynet"
lua_lib="$repo_root/integration/skynet/3rd/lua/liblua.a"
module="$repo_root/build/skynet/luaprof.so"

test -x "$skynet"
test -f "$lua_lib"
test -f "$module"

nm -g --defined-only "$lua_lib" | awk '
    $3 == "lua_profile_capturestack" { profile = 1 }
    $3 == "lua_sharefunction" { shared = 1 }
    END { exit !(profile && shared) }
'

nm -g --defined-only "$skynet" | awk '
    $3 == "lua_profile_capturestack" { profile = 1 }
    $3 == "lua_sharefunction" { shared = 1 }
    END { exit !(profile && shared) }
'

nm -D "$module" | awk '
    $2 == "lua_profile_capturestack" && $1 == "U" { found = 1 }
    END { exit !found }
'

grep -Fq 'lua_newstate(lalloc, l, global_seed())' \
    "$repo_root/integration/skynet/service-src/service_snlua.c"
grep -Fq '$LUAPROF_MODULE_CPATH' "$repo_root/examples/skynet/config"
grep -Fq 'build/skynet/?.so' "$repo_root/tests/integration/skynet.sh"

echo "Skynet embedded Lua boundary: ok"
