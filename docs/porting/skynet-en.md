# Porting luaprof to Skynet

[中文](skynet.md) | [Integration](../integration-en.md) |
[Lua 5.4](lua-5.4-en.md) | [Lua 5.5](lua-5.5-en.md)

This guide identifies function-level changes for Skynet's embedded Lua bridge
and scheduler hooks. The validated target is the pinned Skynet fork. A profile
belongs to the service that calls `profile.cpu.start()` and follows that
service across worker migration; it is not a combined profile of the complete
Skynet process.

## 1. Reference diff

```text
baseline: f19d160
bridge:   0af0699
Lua ABI:  customized Lua 5.5, LUA_PROFILE_ABI_VERSION == 2
```

Inspect the complete change:

```sh
git -C integration/skynet diff f19d160..0af0699 -- \
    3rd/lua Makefile skynet-src/skynet_server.c skynet-src/skynet_start.c
```

This diff contains two independent parts: the embedded Lua VM bridge and the
Skynet host/scheduler integration. Port each part by function when the target
fork has diverged. Do not replace `3rd/lua` with the parent's PUC Lua 5.5 tree.

## 2. Embedded Lua bridge

Port the contract and guard semantics from the [Lua 5.5 bridge](lua-5.5-en.md)
into `3rd/lua`, while retaining Skynet's existing differences:

- explicit-seed `lua_newstate`;
- `lua_sharetable`, code cache, coroutine, and GC changes;
- the `onelua.c` amalgamated build;
- Skynet's makefile and dependency layout.

The exact changed files are:

```text
ldebug.c/h, ldo.c, lgc.c, lmem.c, lstate.c/h, lua.h, lvm.c,
new lprofile.c/h, makefile, onelua.c
```

Add `lprofile.o` to `CORE_O` and dependencies in `3rd/lua/makefile`. Include
`lprofile.c` in the VM core section of `onelua.c`. Do not store profiler state
in `lua_Alloc.ud`.

## 3. Link the host library

Compile `src/skynet_host.c` with the embedded Lua headers and archive it as
`libluaprof-skynet-host.a`. In Skynet's root `Makefile`:

1. Add an overridable `LUAPROF_HOST_LIB ?=` variable.
2. Add the archive to the `skynet` executable prerequisites and link inputs.
3. Define `SKYNET_LUAPROF` and add the luaprof include path for scheduler hooks.
4. Link the executable with `-Wl,-E`/`--export-dynamic` so the module can find
   `lp_skynet_host_get_api` through `dlsym`.

The host library belongs in the main executable, not a Lua service module.
`luaprof.so` selects the Skynet backend with
`dlsym(RTLD_DEFAULT, "lp_skynet_host_get_api")`.

The reference diff also adds `$(LUA_STATICLIB)` as a build prerequisite of C
and Lua modules while filtering link inputs to `%.c`. This orders a fresh
parallel build without accidentally linking the archive into every `.so`.

## 4. Worker lifecycle hooks

### `skynet-src/skynet_start.c`: `thread_worker`

After `skynet_initthread(THREAD_WORKER)` and
`skynet_handle_register_thread()`, before entering the dispatch loop, call:

```c
lp_skynet_host_worker_start((unsigned int)id);
```

After leaving the loop and before returning, call:

```c
lp_skynet_host_worker_stop();
```

Guard both calls with `#ifdef SKYNET_LUAPROF`. Register each worker once; do
not create one timer per service.

## 5. Service dispatch hooks

### `skynet-src/skynet_server.c`: `dispatch_message`

After incrementing `message_count` and before either `ctx->cb` branch, enter
the current service:

```c
lp_skynet_host_dispatch_enter(ctx->handle);
```

Leave immediately after all callback branches:

```c
lp_skynet_host_dispatch_leave();
```

Guard both calls with `SKYNET_LUAPROF`. `leave` must cover both values of
`ctx->profile` and finish before freeing the message or starting another
dispatch. The handle assigns CPU samples to a service; a `lua_State *` cannot
replace this scheduler boundary.

## 6. Build the matching ABI module

Compile `luaprof.so` with headers from the same `skynet/3rd/lua` and define:

```text
LUAPROF_EXPECT_LUA_VERSION=505
LUAPROF_LUA_EXPLICIT_SEED
```

The reference artifact is `build/skynet/luaprof.so`. It and
`build/lua55/luaprof.so` both report Lua 5.5 but have different ABIs and are
not interchangeable. Start a CPU recorder from within the target service's
dispatch; otherwise the current handle is zero and the Skynet backend rejects
the start.

## 7. Validation

```sh
make test-skynet
make example-skynet
```

At minimum, verify:

- embedded Lua and the final `skynet` define `lua_profile_capturestack`;
- the executable dynamically exports `lp_skynet_host_get_api`;
- the module leaves `lua_profile_capturestack` undefined as type `U`;
- the target service yields or migrates across at least two workers;
- profile metadata has `scheduler_workers > 0` and contains CPU only from the
  target service's dispatch.

See the [integration guide](../integration-en.md) for exact build commands,
deployment layout, and troubleshooting.
