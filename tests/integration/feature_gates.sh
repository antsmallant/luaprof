#!/bin/sh

set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
temp_root=$(mktemp -d "${TMPDIR:-/tmp}/luaprof-feature-gates.XXXXXX")
trap 'rm -rf "$temp_root"' EXIT HUP INT TERM

profile_symbols='lua_(setprofilehooks|profile_request|getprofilestate|profile_capturestack)'

archive_checkout() {
	source_repo=$1
	destination=$2
	mkdir -p "$destination"
	git -C "$source_repo" archive HEAD | tar -xf - -C "$destination"
}

check_lua_disabled() {
	name=$1
	source_repo=$2
	lua_root=$3
	destination="$temp_root/$name"

	archive_checkout "$source_repo" "$destination"
	make -C "$destination/$lua_root" linux >/dev/null
	library="$destination/$lua_root/liblua.a"

	if ar t "$library" | grep -qx 'lprofile.o'; then
		echo "$name: macro-off archive contains lprofile.o" >&2
		exit 1
	fi
	if nm -g --defined-only "$library" | grep -Eq "$profile_symbols"; then
		echo "$name: macro-off archive exports profiling API" >&2
		exit 1
	fi
	if cc -dM -E -x c -I"$destination/$lua_root" \
		-include lua.h /dev/null | grep -q '^#define LUA_PROFILE_ABI_VERSION'; then
		echo "$name: macro-off lua.h exposes profiling ABI" >&2
		exit 1
	fi
}

check_lua_disabled lua46 "$repo_root/3rd/lua-5.4.6" src
check_lua_disabled lua54 "$repo_root/3rd/lua-5.4.8" src
check_lua_disabled lua55 "$repo_root/3rd/lua-5.5.0" src
check_lua_disabled skynet-lua "$repo_root/integration/skynet" 3rd/lua

skynet_root="$temp_root/skynet-lua"
make -C "$skynet_root" linux MALLOC_STATICLIB= \
	SKYNET_DEFINES=-DNOUSE_JEMALLOC >/dev/null
if nm -u "$skynet_root/skynet" | grep -q 'lp_skynet_host_'; then
	echo "skynet: macro-off executable references luaprof host API" >&2
	exit 1
fi

echo "compile-time feature gates: ok"
