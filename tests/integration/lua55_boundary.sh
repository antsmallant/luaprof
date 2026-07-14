#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
lua="$repo_root/3rd/lua-5.5.0/src/lua"
lua_lib="$repo_root/3rd/lua-5.5.0/src/liblua.a"
module="$repo_root/build/lua55/luaprof.so"

test -x "$lua"
test -f "$lua_lib"
test -f "$module"

nm -g --defined-only "$lua_lib" | awk '
    $3 == "lua_profile_capturestack" { found = 1 }
    END { exit !found }
'

nm -D "$module" | awk '
    $2 == "lua_profile_capturestack" && $1 == "U" { found = 1 }
    END { exit !found }
'

"$lua" -e 'assert(_VERSION == "Lua 5.5")'

echo "PUC Lua 5.5 boundary: ok"
