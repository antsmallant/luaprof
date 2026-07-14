#!/bin/sh
set -eu

repo_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

usage() {
    echo "usage: $0 lua54|lua55|skynet" >&2
    exit 2
}

test "$#" -eq 1 || usage

# Baselines are deliberate; the target is always the current submodule HEAD.
case "$1" in
lua54)
    source_dir="$repo_root/3rd/lua-5.4.8"
    baseline=46f8c3d
    ;;
lua55)
    source_dir="$repo_root/3rd/lua-5.5.0"
    baseline=1097dbe
    ;;
skynet)
    source_dir="$repo_root/integration/skynet"
    baseline=f19d160
    ;;
*)
    usage
    ;;
esac

if ! git -C "$source_dir" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "submodule is not initialized: $source_dir" >&2
    exit 1
fi

if ! git -C "$source_dir" diff --quiet -- ||
   ! git -C "$source_dir" diff --cached --quiet --; then
    echo "submodule has uncommitted tracked changes: $source_dir" >&2
    echo "commit the fork changes before generating a porting patch" >&2
    exit 1
fi

if ! git -C "$source_dir" cat-file -e "$baseline^{commit}" 2>/dev/null; then
    echo "baseline commit is unavailable in $source_dir: $baseline" >&2
    exit 1
fi

git -C "$source_dir" -c color.ui=false diff \
    --binary --full-index --no-ext-diff --no-textconv \
    "$baseline..HEAD" -- .
