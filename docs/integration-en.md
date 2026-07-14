# Integrating luaprof into an existing project

[中文](integration.md) | [README](../READ-en.md) |
[Sampling model](profiling-model-en.md) | [Maintainer guide](maintainer-guide-en.md)

This guide explains how to integrate `luaprof` into an existing Linux project:
either a thread-per-VM host where each Lua VM stays on one OS thread, or Skynet,
where a Lua service may migrate between workers. See the root
[README](../READ-en.md) for the Lua API.

## 1. Check the supported boundary first

V1 officially supports these combinations:

| Host | Lua | CPU backend |
| --- | --- | --- |
| thread-per-VM | The project's pinned PUC Lua 5.4.8 fork | `CLOCK_THREAD_CPUTIME_ID` |
| thread-per-VM | The project's pinned PUC Lua 5.5.0 fork | `CLOCK_THREAD_CPUTIME_ID` |
| Skynet | The pinned Skynet fork and its customized embedded Lua 5.5 | Per-worker thread CPU timers |

Stock Lua does not contain the VM bridge required by `luaprof`. Building only
`luaprof.so` without modifying the Lua that actually runs in the host will not
work. Porting the bridge to another Lua version is technically possible, but
requires a fresh review against that version's VM structures; it is not ABI
compatible by default.

An integration must guarantee the following:

- `luaprof.so` is built with headers from the exact Lua implementation and ABI
  used at runtime.
- The profiler does not introduce a second statically linked copy of Lua into
  the process.
- Linux realtime signal `SIGRTMAX - 2` is reserved for the thread-per-VM
  backend, and `SIGRTMAX - 3` for the Skynet backend. Both must still have their
  default disposition before profiler timers are installed.
- Recorders stop before `lua_close`.
- The VM bridge reports `LUA_PROFILE_ABI_VERSION == 2`.

## 2. Integration components

`luaprof` has three layers:

1. **Lua VM bridge**: publishes VM state, safe points, allocation events, and
   profiler-safe stack capture from inside Lua.
2. **Lua module**: `luaprof.so`, implementing the Lua API, sample aggregation,
   symbol collection, and pprof export.
3. **Host backend**: thread-per-VM uses the thread timer inside the module.
   Skynet additionally links `libluaprof-skynet-host.a` into the `skynet`
   executable and installs scheduler hooks.

Memory events originate in Lua's internal `lmem.c`; they do not replace the
public `lua_Alloc`. Its signature and `ud` semantics stay unchanged, while the
allocation callback receives an accurate `lua_State *`, old/new pointers,
old/new requested sizes, and realloc success.

## 3. Modify or replace the project's Lua

### 3.1 Lowest-risk option: use the pinned fork

For a regular PUC Lua 5.4 or 5.5 project, use the exact matching fork pinned by
this repository:

```sh
git submodule update --init 3rd/lua-5.4.8
make lua

git submodule update --init 3rd/lua-5.5.0
make lua55
```

The host must include and link this tree's `src/lua.h` and `src/liblua.a`. Do
not replace only the headers, and do not let `luaprof.so` and the host use two
different Lua copies.

Skynet must not use either parent PUC Lua fork. Keep Skynet's own
`3rd/lua` and port the same bridge into that directory. The pinned
`integration/skynet` tree already does this while retaining seeded
`lua_newstate`, code cache, and shared Proto/table behavior.

### 3.2 Port the bridge into a customized Lua

First select the function-level guide matching the target major/minor version:

- [Lua 5.4 bridge port](porting/lua-5.4-en.md)
- [Lua 5.5 bridge port](porting/lua-5.5-en.md)
- [Skynet embedded Lua and scheduler port](porting/skynet-en.md)

The complete PUC Lua reference diffs are:

```sh
git -C 3rd/lua-5.4.8 diff 46f8c3d..02c8f57 -- src
git -C 3rd/lua-5.5.0 diff 1097dbe..074659c -- src
```

For an identical base, generate a patch and first run `git apply --check`. If
the target Lua already has internal modifications, port and review each
function separately instead of applying the patch blindly. The complete file
set and responsibilities are:

| File | Required change |
| --- | --- |
| `lua.h` | Bridge ABI, hook/event/frame types, and four public bridge functions |
| `lstate.h`, `lstate.c` | Hook, pending, state, and capture-guard storage in `global_State`/`lua_State`, plus initialization and teardown |
| `lprofile.h`, `lprofile.c` | State publication, pending requests, safe points, allocation events, and stack capture |
| `lvm.c` | Instruction-dispatch safe point and Lua execution state boundaries |
| `ldo.c` | C function/continuation states, yield/resume, exception unwind, and protected-call recovery |
| `lgc.c` | GC state boundaries around incremental and full collection entry points |
| `lmem.c` | Complete events for successful alloc/free/realloc and failed realloc |
| `ldebug.c`, `ldebug.h` | Best-effort sampled-frame call names inside the VM |
| `Makefile` | Compile and link `lprofile.c` |

An amalgamated Lua build must also include `lprofile.c`; the Skynet reference
changes `3rd/lua/onelua.c`.

These changes form one contract. Copying only `lprofile.c` and `lprofile.h`
omits safe points, C/GC states, and allocator events. Preserve these invariants:

- Profiler callbacks must not call Lua, allocate through Lua, or raise errors.
- `lua_profile_request` may add pending weight from a timer signal handler, but
  captures the stack only at the next VM safe point.
- The allocation callback receives `lua_State *` from `lmem.c`; it does not
  alter the `lua_Alloc` ABI or repurpose allocator `ud`.
- Successful realloc ends the old block and creates one complete new
  allocation. Failed realloc leaves the old block alive.
- While the Lua stack is being relocated, capture returns an empty stack and
  reports truncation instead of walking temporarily invalid `CallInfo` pointers.
- State publication after errors, yield, resume, and coroutine switches agrees
  with the current `CallInfo`.

The current Skynet reference diff is:

```sh
git -C integration/skynet diff f19d160..0af0699 -- \
    3rd/lua Makefile skynet-src/skynet_server.c skynet-src/skynet_start.c
```

It includes both the Lua bridge and scheduler integration. When the target
Skynet fork has diverged, port those two parts separately as described below.

## 4. Thread-per-VM integration

### 4.1 Build the module

Select the target matching the pinned Lua ABI:

```sh
make module          # build/luaprof.so, PUC Lua 5.4.8
make module-lua55    # build/lua55/luaprof.so, PUC Lua 5.5.0
```

For a customized PUC Lua with the standard source layout, build against that
tree into a distinct output directory. Lua 5.4:

```sh
make module \
    LUA_DIR=/absolute/path/to/lua-5.4.8 \
    LUA_SRC=/absolute/path/to/lua-5.4.8/src \
    LUA_LIB=/absolute/path/to/lua-5.4.8/src/liblua.a \
    BUILD_DIR=/absolute/path/to/project-build/luaprof-lua54
```

Lua 5.5:

```sh
make module-lua55 \
    LUA55_DIR=/absolute/path/to/lua-5.5.0 \
    LUA55_SRC=/absolute/path/to/lua-5.5.0/src \
    LUA55_LIB=/absolute/path/to/lua-5.5.0/src/liblua.a \
    LUA55_BUILD_DIR=/absolute/path/to/project-build/luaprof-lua55
```

This invokes `make linux` in the target Lua directory. If the customized Lua
uses another build system, move the root Makefile's `$(LUA_MODULE)` source list
and flags into the project build. The essential requirements are:

- Compile every module object as C11 with `-fPIC` and the target Lua include
  path.
- Define `LUAPROF_EXPECT_LUA_VERSION=504` or `505` for PUC Lua 5.4.8 or 5.5.0.
- Link the shared module with `-lm -lz -ldl -pthread -lrt`.
- Keep `lua_*` and `lua_profile_*` references undefined in the module so they
  resolve to the Lua already running in the host. Do not statically put another
  `liblua.a` into the module.

If the host statically links Lua into its executable, export Lua symbols from
the Linux executable:

```make
my_host: $(HOST_OBJECTS) /path/to/liblua.a
	$(CC) -Wl,-E -o $@ $^ -lm -ldl -pthread
```

For dynamically linked Lua, likewise verify that `lua_profile_*` is visible in
the process-wide dynamic symbol scope.

### 4.2 Deploy and manage the lifecycle

Put the module on the application's C module search path:

```sh
export LUA_CPATH=/absolute/path/to/modules/?.so\;\;
```

The host may instead set `package.cpath` after creating the VM. Each VM to be
profiled loads the module directly:

```lua
local profile = require "luaprof"

local cpu = assert(profile.cpu.start { sample_hz = 100 })
local memory = assert(profile.memory.start {
    sample_bytes = 512 * 1024,
    track_free = true,
})

-- workload

local memory_result = assert(memory:stop())
local cpu_result = assert(cpu:stop())
assert(memory_result:write("heap.pb.gz"))
assert(cpu_result:write("cpu.pb.gz"))
```

CPU recorder `start()`, VM execution, and `stop()` must all occur on the VM's
own OS thread. The VM must not migrate while the recorder is active. Multiple
thread-per-VM instances can profile independently, with at most 64 registered
thread timers per process. One thread cannot run CPU recorders for two VMs at
the same time. Coroutines in one VM share the bridge and are supported.

## 5. Skynet integration

A Skynet service can migrate between workers, so a thread-per-VM timer cannot
represent service CPU time. A complete integration requires the embedded Lua
bridge, host library, worker/dispatch hooks, and a Skynet-ABI module.

### 5.1 Modify Skynet's embedded Lua

Port the section 3 bridge into the project's `skynet/3rd/lua`; do not replace
the entire directory with the parent Lua fork. Preserve the project's existing:

- `lua_newstate` parameters and seed propagation;
- `lua_sharefunction`, shared Proto/table, and code-cache behavior;
- other Skynet changes to coroutine, GC, and state creation;
- `onelua.c` or the amalgamated build entry used by the project.

After the build, both bridge and Skynet extension symbols must be defined by
the same `liblua.a` and final `skynet` executable.

### 5.2 Build and link the host library

Compile `src/skynet_host.c` with the embedded Lua headers and archive it as
`libluaprof-skynet-host.a`:

```sh
cc -std=c11 -O2 -fPIC \
    -I/path/to/luaprof/include \
    -I/path/to/skynet/3rd/lua \
    -c /path/to/luaprof/src/skynet_host.c -o skynet_host.o
ar rcs libluaprof-skynet-host.a skynet_host.o
```

Link this archive into the `skynet` executable, not a Lua service module, and
export main-program symbols with `-Wl,-E` or equivalent. The link environment
must also provide C11, pthreads, realtime timers, and `dlopen`. A representative
Makefile relationship is:

```make
SKYNET_DEFINES += -DSKYNET_LUAPROF -I/path/to/luaprof/include

skynet: $(SKYNET_OBJECTS) libluaprof-skynet-host.a $(LUA_LIB)
	$(CC) -Wl,-E -o $@ $^ $(SKYNET_LIBS) -pthread -lrt -ldl
```

`luaprof.so` discovers this backend with
`dlsym(RTLD_DEFAULT, "lp_skynet_host_get_api")`. Without the host library, the
module falls back to the thread-per-VM backend. That configuration is invalid
for a migrating Skynet service and must not be deployed.

### 5.3 Install scheduler hooks

When `SKYNET_LUAPROF` is defined, add these calls around each worker lifecycle:

```c
lp_skynet_host_worker_start(worker_id);  /* after init, before dispatch loop */
lp_skynet_host_worker_stop();            /* after leaving dispatch loop */
```

Wrap every service message callback with:

```c
lp_skynet_host_dispatch_enter(ctx->handle);
reserve_msg = ctx->cb(/* existing arguments */);
lp_skynet_host_dispatch_leave();
```

See `skynet-src/skynet_start.c` and `skynet-src/skynet_server.c` in the pinned
fork for exact locations. Every callback branch must pass through enter/leave,
and leave must complete before the next service dispatch. The handle associates
the current worker's timer sample with the target service; a `lua_State *` is
not a replacement for this scheduler boundary.

### 5.4 Build the Skynet-ABI module

Compile the module with headers from `skynet/3rd/lua` and define this for the
current customized Lua 5.5:

```text
LUAPROF_EXPECT_LUA_VERSION=505
```

Keep the artifact separate from regular Lua modules, for example:

```text
modules/lua54/luaprof.so
modules/skynet-lua55/luaprof.so
```

The reference artifact is `build/skynet/luaprof.so`. Do not copy
`build/luaprof.so` into Skynet. Add the correct directory to Skynet's
`lua_cpath`:

```lua
lua_cpath = "/absolute/path/to/skynet-lua55/?.so;" .. root .. "luaclib/?.so"
```

Start a CPU recorder inside the target service dispatch, such as a
`skynet.start` callback. A current service handle must exist or the scheduler
backend returns a host error. Multiple services may profile concurrently, but
all active CPU recorders currently must use the same `sample_hz`.

## 6. Validate the integration

Do not treat creation of a `.pb.gz` file by itself as proof of a correct
integration. Run at least these checks.

### 6.1 VM bridge contract

For the pinned Lua:

```sh
make test-vm-bridge
```

For a customized Lua, compile `tests/integration/vm_bridge_test.c` against its
headers and library. A Skynet Lua whose `lua_newstate` accepts an explicit seed
also needs `LUAPROF_LUA_EXPLICIT_SEED`. This test covers Lua/C/GC/host states,
pending safe points, allocation/reallocation/free, coroutines, and stack
capture.

### 6.2 Symbol and ABI boundaries

The Lua library must define the bridge:

```sh
nm -g --defined-only /path/to/liblua.a | grep lua_profile_capturestack
```

The module must reference the host bridge instead of defining its own copy:

```sh
nm -D /path/to/luaprof.so | grep lua_profile_capturestack
```

The expected symbol type is `U`. The Skynet executable must also export the
host API and define its embedded Lua bridge:

```sh
nm -D /path/to/skynet | grep lp_skynet_host_get_api
nm -g --defined-only /path/to/skynet | grep lua_profile_capturestack
```

### 6.3 Runtime smoke test

- thread-per-VM: run a sufficiently long Lua compute and allocation workload,
  stop CPU and memory independently, and require CPU `samples > 0` and memory
  `samples > 0`.
- Skynet: use at least two workers, explicitly yield or migrate the target
  service, and require `scheduler_workers > 0`. Zero means the module did not
  select the Skynet backend.
- With `track_free = true`, require `inuse_space > 0` and
  `live_map_overflows == 0`.
- Inspect `dropped_events`, `unstable_events`, `stack_truncations`, and all
  overflow counters. They should not be significant for a normal workload.

Finally inspect function, line, and source attribution with pprof:

```sh
go tool pprof -top cpu.pb.gz
go tool pprof -lines -top cpu.pb.gz
go tool pprof -list=your_hot_function cpu.pb.gz
go tool pprof -sample_index=inuse_space -top heap.pb.gz
```

## 7. Common integration failures

| Symptom | Check first |
| --- | --- |
| `undefined symbol: lua_profile_*` | The host Lua lacks the bridge, or statically linked Lua symbols were not exported with `-Wl,-E` |
| Build-time `unexpected Lua ABI` | The module used the wrong Lua headers or `LUAPROF_EXPECT_LUA_VERSION` |
| Skynet CPU profile has `scheduler_workers == 0` | Host library/hooks are inactive and the module incorrectly fell back to the thread backend |
| Skynet `cpu.start` returns host error | The call is outside service dispatch, handle is zero, worker is unregistered, or concurrent recorders use different `sample_hz` |
| Profile shows only address-form CFunctions | The executable is stripped, symbols are not exported, or Lua binding scan cannot reach the function; the Lua caller stack is still valid |
| Memory `inuse_*` is always zero | `track_free = true` was not enabled |
| Starting a timer returns host error | Realtime signal is occupied, the thread already has a recorder, or a fixed timer/worker bound was reached |

The repository's `examples/thread_vm`, `examples/skynet`, and
`tests/integration` directories are minimal post-integration references. A
production project should add regression coverage for its own VM creation,
thread exit, service migration, and shutdown paths.
