#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
generator="$repo_root/scripts/generate-porting-patch.sh"
patch_file=

cleanup() {
    if test -n "$patch_file"; then
        rm -f "$patch_file"
    fi
}
trap cleanup EXIT HUP INT TERM

check_patch() {
    target=$1
    source_dir=$2
    patch_file=$(mktemp)

    "$generator" "$target" >"$patch_file"
    if ! test -s "$patch_file"; then
        echo "generated patch is empty: $target" >&2
        exit 1
    fi
    case "$target" in
    lua46|lua54|lua55)
        if grep -q '^diff --git a/\.gitignore b/\.gitignore$' "$patch_file"; then
            echo "generated Lua patch contains repository-only .gitignore: $target" >&2
            exit 1
        fi
        ;;
    esac
    git -C "$source_dir" apply --check --reverse "$patch_file"

    rm -f "$patch_file"
    patch_file=
    echo "porting patch $target: ok"
}

check_patch lua46 "$repo_root/3rd/lua-5.4.6"
check_patch lua54 "$repo_root/3rd/lua-5.4.8"
check_patch lua55 "$repo_root/3rd/lua-5.5.0"
check_patch skynet "$repo_root/integration/skynet"
