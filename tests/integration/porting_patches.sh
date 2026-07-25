#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
generator="$repo_root/scripts/generate-porting-patch.sh"
temp_root=$(mktemp -d "${TMPDIR:-/tmp}/luaprof-porting-test.XXXXXX")

cleanup() {
    rm -rf "$temp_root"
}
trap cleanup EXIT HUP INT TERM

check_patch() {
    target=$1
    source_dir=$2
    committed_patch=$3
    metadata_file="$repo_root/patches/README.md"
    filename=$(basename "$committed_patch")
    baseline_commit=$(git -C "$source_dir" rev-parse HEAD^)
    target_commit=$(git -C "$source_dir" rev-parse HEAD)
    patch_file="$temp_root/$target.generated.patch"
    baseline_dir="$temp_root/$target-baseline"

    "$generator" "$target" >"$patch_file"
    if ! test -s "$patch_file"; then
        echo "generated patch is empty: $target" >&2
        exit 1
    fi
    if ! test -s "$committed_patch"; then
        echo "committed patch is missing or empty: $committed_patch" >&2
        exit 1
    fi
    if ! cmp -s "$patch_file" "$committed_patch"; then
        echo "committed patch is stale: $committed_patch" >&2
        echo "run 'make update-porting-patches' and commit the result" >&2
        exit 1
    fi
    metadata_line=$(grep -F "\`$filename\`" "$metadata_file" || :)
    case "$metadata_line" in
    *"$baseline_commit"*"$target_commit"*)
        ;;
    *)
        echo "patch metadata is stale: $filename" >&2
        exit 1
        ;;
    esac
    case "$target" in
    lua46|lua54|lua55)
        if grep -q '^diff --git a/\.gitignore b/\.gitignore$' "$committed_patch"; then
            echo "committed Lua patch contains repository-only .gitignore: $target" >&2
            exit 1
        fi
        ;;
    esac

    mkdir -p "$baseline_dir"
    git -C "$source_dir" archive "$baseline_commit" |
        tar -xf - -C "$baseline_dir"
    git -C "$baseline_dir" init -q
    git -C "$baseline_dir" apply --check "$committed_patch"
    git -C "$source_dir" apply --check --reverse "$committed_patch"

    echo "porting patch $target: ok"
}

check_patch lua46 "$repo_root/3rd/lua-5.4.6" \
    "$repo_root/patches/lua-5.4.6.patch"
check_patch lua54 "$repo_root/3rd/lua-5.4.8" \
    "$repo_root/patches/lua-5.4.8.patch"
check_patch lua55 "$repo_root/3rd/lua-5.5.0" \
    "$repo_root/patches/lua-5.5.0.patch"
check_patch skynet "$repo_root/integration/skynet" \
    "$repo_root/patches/skynet.patch"
