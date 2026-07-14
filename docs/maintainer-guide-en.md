# luaprof maintainer guide

[中文](maintainer-guide.md) | [README](../READ-en.md) |
[Integration](integration-en.md) | [Sampling model](profiling-model-en.md)

This document is for maintainers of `luaprof`, the Lua fork, and the Skynet
fork. Regular users need only the README and integration guide.

## 1. Repository and build boundaries

The parent repository pins three direct submodules:

| Path | Remote branch | Purpose |
| --- | --- | --- |
| `3rd/lua-5.4.8` | `luaprof` in `lua-5.4.8.git` | Thread-per-VM Lua 5.4.8 |
| `3rd/lua-5.5.0` | `luaprof` in `lua-5.5.0.git` | Thread-per-VM Lua 5.5.0 |
| `integration/skynet` | `luaprof` in `skynet.git` | Skynet host and its customized Lua 5.5 |

Fetch URLs in `.gitmodules` must remain public HTTPS endpoints.
`branch = luaprof` exists only for maintainer use with
`git submodule update --remote`. Normal `make`, CI, and user checkouts use the
exact commit in the parent gitlink and never follow a remote branch implicitly.

The three Lua build boundaries use different modules:

```text
build/luaprof.so          PUC Lua 5.4.8
build/lua55/luaprof.so    PUC Lua 5.5.0
build/skynet/luaprof.so   Skynet customized Lua 5.5
```

They define `LUAPROF_EXPECT_LUA_VERSION=504` and `505`, respectively. PUC Lua
5.5 and Skynet Lua both report 505 but remain different VM ABIs. The modules
are not interchangeable, and no module may statically include another
`liblua.a`.

Current ABI contracts are:

- Lua VM bridge: `LUA_PROFILE_ABI_VERSION == 2`
- Skynet host API: `LP_SKYNET_HOST_ABI_VERSION == 1`

A change to public structures, callback semantics, or symbol contracts requires
reviewing and possibly updating the ABI version, both Lua forks, the module, and
contract tests.

## 2. Public checkouts

Users and CI should clone over HTTPS:

```sh
git clone https://github.com/antsmallant/luaprof.git
cd luaprof
git submodule update --init \
    3rd/lua-5.4.8 3rd/lua-5.5.0 integration/skynet
```

`make` initializes only the Lua submodule by default. Explicit Skynet targets
initialize Skynet and its direct dependencies, preventing a normal build from
downloading unrelated nested submodules.

After changing `.gitmodules`, run:

```sh
git submodule sync -- \
    3rd/lua-5.4.8 3rd/lua-5.5.0 integration/skynet
```

Before committing, clone from the public URL into a clean directory and
initialize both direct submodules. This prevents local `.git/config` or URL
rewrites from hiding a broken public configuration.

## 3. Independent fork development

Do not use the parent's submodule checkouts as long-lived development trees.
Create independent checkouts next to the parent repository:

```sh
git clone --branch luaprof \
    https://github.com/antsmallant/lua-5.4.8.git ../lua-5.4.8
git clone --branch luaprof \
    https://github.com/antsmallant/lua-5.5.0.git ../lua-5.5.0
git clone --branch luaprof \
    https://github.com/antsmallant/skynet.git ../skynet
```

When SSH push is required, change only the independent checkout's push URL and
keep its public fetch URL:

```sh
git -C ../lua-5.4.8 remote set-url --push origin \
    git@github.com:antsmallant/lua-5.4.8.git
git -C ../lua-5.5.0 remote set-url --push origin \
    git@github.com:antsmallant/lua-5.5.0.git
git -C ../skynet remote set-url --push origin \
    git@github.com:antsmallant/skynet.git
```

Standard fork workflow:

1. Change the `luaprof` branch in the independent checkout.
2. Run the fork's relevant build and tests.
3. Commit and push `origin/luaprof`.
4. Verify that the new commit is fetchable from the remote.
5. Return to the parent and update its gitlink.

Pushing the fork before updating the parent is essential. Otherwise, the parent
references a commit external users cannot fetch.

## 4. Update parent gitlinks

Lua fork:

```sh
git submodule update --remote 3rd/lua-5.4.8
git diff --submodule=log -- 3rd/lua-5.4.8
make test
git add 3rd/lua-5.4.8
git commit -m "build: update Lua submodule"
```

Use the same workflow for the Lua 5.5 fork:

```sh
git submodule update --remote 3rd/lua-5.5.0
git diff --submodule=log -- 3rd/lua-5.5.0
make test-lua55
git add 3rd/lua-5.5.0
git commit -m "build: update Lua 5.5 submodule"
```

Skynet fork:

```sh
git submodule update --remote integration/skynet
git diff --submodule=log -- integration/skynet
make test-skynet
git add integration/skynet
git commit -m "build: update Skynet submodule"
```

`git submodule update --remote` should change only the gitlink. If a submodule
contains uncommitted files, identify their owner before proceeding; do not use
reset or checkout to discard another developer's work.

After the update, inspect:

```sh
git submodule status
git status --short
```

After committing and pushing the parent, verify that `HEAD` matches
`origin/master`.

## 5. Modify the Lua bridge

The complete PUC Lua bridge file sets are documented in the function-level
[Lua 5.4](porting/lua-5.4-en.md) and [Lua 5.5](porting/lua-5.5-en.md) guides.
A bridge change typically touches:

- public profiler ABI in `lua.h`;
- `lstate.*` and `lprofile.*`;
- `lvm.c`, `ldo.c`, `lgc.c`, and `lmem.c`;
- `ldebug.*` for stack and call names;
- build dependencies and amalgamated builds.

Do not change only one PUC Lua fork. If the contract applies to Skynet, port
the equivalent change into Skynet's embedded `3rd/lua` and run both bridge test
variants.

### Skynet Lua constraints

Skynet profiling changes belong to the Skynet fork. Do not replace
`integration/skynet/3rd/lua` with the parent Lua 5.4.8 fork or pass parent
`LUA_INC`/`LUA_LIB` values into the Skynet build.

A port must preserve:

- seeded `lua_newstate`;
- `lua_sharefunction`, shared Proto/table, and code cache;
- Skynet's VM ABI and coroutine/GC behavior;
- the matching source inclusion in `onelua.c`.

## 6. Skynet host integration

In addition to the embedded Lua bridge, the Skynet fork maintains:

- worker start/stop hooks in `skynet-src/skynet_start.c`;
- dispatch enter/leave hooks in `skynet-src/skynet_server.c`;
- the root Makefile dependency and link for `libluaprof-skynet-host.a`;
- main-program dynamic symbol export through `-Wl,-E`.

After changing scheduler boundaries, test service migration, concurrent
targets, stale ticks, worker shutdown, and active-runtime destruction. Host and
module symbol-boundary checks live in
`tests/integration/skynet_lua_boundary.sh`.

## 7. Test matrix

Regular Lua:

```sh
make test
make example-thread-vm
make bench-vm
make bench-combined

make test-lua55
make example-lua55
make bench-lua55-vm
make bench-lua55-combined
```

Skynet:

```sh
make test-skynet
make example-skynet
make bench-skynet-vm
make bench-skynet-combined
```

For documentation-only or submodule-URL changes, run at least
`git diff --check`, a public HTTPS clone, and direct-submodule initialization.
For bridge, host, sampling, or exporter changes, run the complete relevant
matrix.

Examples should validate more than file creation. Inspect functions, lines,
CFunction names, and memory sample indexes with pprof, and check drop,
truncation, and overflow counters.

## 8. Release checks

Before a public release or update, verify:

- The README remains focused on first use and contains no local paths or
  internal workflow.
- `.gitmodules` contains only public HTTPS fetch URLs.
- All three gitlink commits are pushed to their respective `luaprof` branches.
- Chinese and English README and documentation links resolve.
- All three modules use the correct Lua ABI and do not contain another static
  Lua.
- `make test`, `make test-lua55`, and `make test-skynet` pass.
- Thread-per-VM and Skynet examples produce profiles readable by standard
  `go tool pprof`.
- The parent and all three forks contain no maintainer-owned uncommitted
  changes.

Local workspace paths, independent checkout locations, and agent collaboration
rules are internal development information. Record them in development
documentation or `AGENTS.md`, not the public README.
