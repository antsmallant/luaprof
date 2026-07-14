# luaprof

[中文](README.md)

`luaprof` is a sampling profiler for PUC Lua. CPU and memory recorders are
independent, and the same API works in Linux thread-per-VM hosts using the
pinned Lua 5.4.8 fork and in supported Skynet services using Skynet's pinned,
customized Lua 5.5. V1 intentionally does not include call/return tracing.

## Requirements

- Linux with POSIX per-thread CPU timers and lock-free pointer, integer and
  64-bit atomics
- A C11 compiler, GNU Make, POSIX threads and zlib development files
- The pinned `lua-5.4.8` fork in `3rd/` for the default build; stock Lua does
  not expose the required profiling bridge
- GitHub SSH access for initializing the configured submodule URLs
- Go's `pprof` command for reading the default output; Graphviz is additionally
  required for SVG and graph-based web reports

The profiler core does not depend on Skynet. Skynet support is an explicit host
integration through the pinned fork in `integration/skynet`; that target keeps
Skynet's own Lua, including its seeded state creation, code cache and shared
Proto/table support.

## Quick start

```sh
git clone git@github.com:antsmallant/luaprof.git
cd luaprof
make
make test
make example-thread-vm
```

The example runs CPU and in-use memory recorders together, stops memory first,
continues CPU profiling, and writes:

```text
build/thread-vm-cpu.pb.gz
build/thread-vm-heap.pb.gz
```

Inspect them with standard pprof tooling:

```sh
go tool pprof -top build/thread-vm-cpu.pb.gz
go tool pprof -lines -top build/thread-vm-cpu.pb.gz
go tool pprof -list=calculate_orders build/thread-vm-cpu.pb.gz
go tool pprof -sample_index=alloc_space -top build/thread-vm-heap.pb.gz
go tool pprof -sample_index=inuse_space -top build/thread-vm-heap.pb.gz
```

The default `-top` aggregates by function; the example should show hotspots
such as `calculate_orders`, `calculate_discounts`, and
`tostring [luaB_tostring]`. `-lines -top` splits by the actual executing line,
while `-list` annotates the corresponding source lines. The main chunk is
shown as `main chunk` instead of `profile.lua:0`.

`make` initializes only the required Lua submodule. `make test` runs the core,
thread-per-VM and exporter tests; Skynet and its direct submodule are initialized
only by the explicit Skynet targets.

For integrating `luaprof` into an existing thread-per-VM or Skynet project, see
the [integration guide](docs/integration-en.md). It covers the Lua VM bridge
source changes, build and link requirements, Skynet scheduler hooks, and
post-integration validation.

## Lua API

Start each recorder directly. There is no shared `profile.start()` mode table:

```lua
local profile = require "luaprof"

local cpu = assert(profile.cpu.start {
    sample_hz = 100,
})
local memory = assert(profile.memory.start {
    sample_bytes = 512 * 1024,
    track_free = true,
})

-- Run the workload here.

local memory_result = assert(memory:stop())
-- CPU profiling is still active here.
local cpu_result = assert(cpu:stop())

assert(cpu_result:write("cpu.pb.gz"))
assert(memory_result:write("heap.pb.gz"))
```

`profile.cpu.start([options])` accepts `sample_hz`, an integer from 1 to 10000.
The default is 100Hz. It measures on-thread CPU time, so sleeping does not
produce samples.

`profile.memory.start([options])` accepts a positive integer `sample_bytes` and
a boolean `track_free`. The defaults are 512KiB and `false`. `sample_bytes = 1`
records every successful allocation and realloc.

Only one recorder of each kind may be active in a Lua VM, but CPU and memory may
run and stop independently. A recorder's `__gc` and `__close` methods stop and
discard an active recording; call `stop()` explicitly when the result is
needed. A stopped result owns a frozen profile and supports:

- `result:stats()` to return counters and quality metadata
- `result:write(path[, options])` to export pprof or folded stacks

Unknown options and invalid option types raise Lua argument errors. Host or
lifecycle failures return `nil, error`.

## CPU sampling

The thread-per-VM backend uses `CLOCK_THREAD_CPUTIME_ID`. A timer tick records a
small VM state snapshot without walking Lua or native stacks in the signal
handler. At the next VM safe point, the recorder drains pending ticks and
captures the Lua stack.

Ticks in a long C call retain the `lua_CFunction` pointer and the Lua caller.
At export time, outside all hot paths, the profiler scans CFunction bindings in
`_G` and `package.loaded` and reads local ELF mappings and symbol tables. It
prefers a Lua-visible binding name, then a native symbol, and finally the raw
`lua_CFunction@0x...` address. When distinct Lua and native names are both
available, the result is displayed as `tostring [luaB_tostring]`. Aliases for
the same pointer are resolved by shortest name and then lexicographically.

This work happens only in `result:write()`, never in the signal handler,
allocation callback, or VM instruction fast path. The address fallback remains
when the binary is stripped or moved, is not ELF, or the binding scan does not
cover the function. Profiles include the local mapping path; subsequent native
symbolization on another machine needs the matching original binary. This still
identifies only the active `lua_CFunction`, not a native C stack. Use a native
profiler to find native hot lines. GC and host states are synthetic frames.

Important CPU statistics are:

- `samples`: attributed timer-tick weight, including timer overruns
- `sample_lua`, `sample_c`, `sample_gc`, `sample_host`: tick weight by VM state
- `safe_points`, `pending_weight`: safe-point drain count and requested weight
- `state_lua`, `state_c`, `state_gc`, `state_host`: VM state transition counts
- `dropped_events`: ticks lost because a fixed event ring was full
- `unstable_events`: ticks rejected during an execution-slot publication race
- `profiler_overhead_events`: ticks arriving while profile data was collected
- `stale_events`, `scheduler_workers`: Skynet generation rejects and workers
  observed for the target
- `stack_truncations`, `aggregate_overflows`, `symbol_overflows`: bounded-store
  quality counters

For a healthy profile, compare `samples` with the duration and configured
frequency, inspect the state split, and require drop/overflow counters to be
negligible. Short profiles can contain too few samples to support a conclusion.

## Memory sampling

Allocation intervals follow a geometric distribution whose expected byte
distance is `sample_bytes`. An allocation is selected in proportion to its full
requested new size, produces at most one sample, and is inverse-probability
weighted to estimate allocation bytes and objects. Frees and failed reallocs do
not consume the sampling budget. A successful realloc is treated as ending the
old block and allocating its complete new requested size.

Memory statistics separate observations from estimates:

- `allocation_events`, `reallocation_events`, `free_events` and
  `allocation_failures`: exact allocator event counts while recording
- `samples`, `sampled_alloc_bytes`: raw selected-event count and requested bytes
- `alloc_space`, `alloc_objects`: probability-weighted allocation estimates
- `inuse_space`, `inuse_objects`: weighted sampled blocks still live at stop
- `live_map_overflows`: sampled live blocks omitted from in-use tracking
- `stack_truncations`, `aggregate_overflows`, `symbol_overflows`: bounded-store
  quality counters

With `track_free = false`, no live-pointer map is allocated or queried and the
in-use metrics remain zero. With `track_free = true`, only sampled live blocks
are stored and frees are attributed back to their allocation stacks. Free-site,
lifetime, peak and object-timeline profiles are not collected.

When Lua is reallocating its own VM stack, call-frame pointers are temporarily
unavailable. A selected stack-reallocation event still contributes to the
memory metrics, but is stored with an empty stack and increments
`stack_truncations` instead of traversing invalid VM state.

These are requested allocator sizes, not RSS, physical memory or a VM heap
snapshot. `sample_bytes = 1` gives exact requested alloc-space and in-use values;
larger intervals are statistical estimates. Increasing the interval reduces
stack-capture work but raises variance, especially for short profiles and
in-use object counts.

## Export formats

The default format is gzip-compressed Google `profile.proto`:

```lua
assert(cpu_result:write("cpu.pb.gz"))
assert(memory_result:write("heap.pb.gz", {
    sample = "alloc_space",
}))
```

CPU profiles contain `samples/count` and `cpu/nanoseconds`. Memory profiles
contain `alloc_objects/count`, `alloc_space/bytes`, `inuse_objects/count` and
`inuse_space/bytes`. The default sample is `cpu`, `alloc_space` when free
tracking is disabled, and `inuse_space` when it is enabled.

A Lua frame's function name, definition line, and current execution line are
separate data. The default pprof function view uses the function name, while
`-lines` and `-list` use current execution lines. Lua call names are best-effort
names inferred by the VM at the sample point; anonymous calls fall back to the
source and definition line.

Examples of alternate reports are:

```sh
go tool pprof -text cpu.pb.gz
go tool pprof -sample_index=inuse_space -svg heap.pb.gz > heap.svg
go tool pprof -http=:0 heap.pb.gz
```

Folded root-to-leaf stacks are also available for flame graph tooling:

```lua
assert(memory_result:write("heap.folded", {
    format = "folded",
    sample = "inuse_space",
}))
```

## Skynet integration

Build and run the two-worker example explicitly:

```sh
make example-skynet
```

`make example-skynet` runs the longer diagnostic workload in
`examples/skynet/luaprof_demo.lua` and writes:

```text
build/skynet-cpu.pb.gz
build/skynet-heap.pb.gz
```

Inspect functions, executing lines, and annotated source with:

```sh
go tool pprof -top build/skynet-cpu.pb.gz
go tool pprof -lines -top build/skynet-cpu.pb.gz
go tool pprof -source_path=examples/skynet -list=calculate_orders build/skynet-cpu.pb.gz
go tool pprof -sample_index=inuse_space -top build/skynet-heap.pb.gz
```

Skynet loads `../../examples/...` while running under `integration/skynet`, so
`-list` needs the shown `-source_path` when invoked at the repository root.
`tests/integration/skynet.sh` runs the shorter `luaprof_smoke.lua` by default;
that service is a CI lifecycle/shared-table regression, not a hotspot demo. The
Skynet fork links a small host library into the executable, publishes the target
VM at service dispatch boundaries and maintains one CPU timer per worker.

The two hosts intentionally use different Lua ABI builds:

```text
build/luaprof.so          Lua 5.4.8 thread-per-VM module
build/skynet/luaprof.so   Skynet customized Lua 5.5 module
```

`make test-skynet` builds Skynet against `integration/skynet/3rd/lua`; it does
not pass the parent project's `LUA_INC` or `LUA_LIB`. The target checks the
linked symbols, runs the VM bridge and Lua API suites against Skynet's Lua, and
then runs the real two-worker service with shared-table coverage.

Multiple Skynet services may record CPU concurrently, but all active CPU
recorders currently must use the same `sample_hz`. Service migration, stale
ticks, worker shutdown and concurrent stop are covered by the scheduler tests.

Run the same VM and combined-profiler benchmarks against Skynet's Lua with:

```sh
make bench-skynet-vm
make bench-skynet-combined
```

## Fixed bounds

Hot-path storage is preallocated and does not grow during recording:

| Resource | Bound per recorder or host |
| --- | ---: |
| Captured stack depth | 64 frames |
| CPU symbols / stack aggregates / source bytes | 4096 / 2048 / 256KiB |
| Memory symbols / stack aggregates / source bytes | 4096 / 2048 / 256KiB |
| Inferred call name per sampled Lua function | 255 bytes |
| Sampled live blocks with `track_free` | 16384 |
| Thread or Skynet timer event ring | 4096 entries |
| Thread timers / Skynet targets / Skynet workers | 64 / 128 / 64 |

Source names longer than 1024 bytes and inferred Lua names longer than 255 bytes
are truncated and increment `symbol_overflows`. When a recording bound is
reached, storage remains bounded and the corresponding drop, truncation or
overflow counter increases. Exporting happens after stop and may allocate. The
export-time Lua-visible CFunction scan has separate limits of 4096 functions,
4096 tables, six levels, and 255-byte names; uncovered functions still try
native symbols and ultimately retain the raw address fallback.

## Supported scope

- PUC Lua 5.4.8 at the exact parent-repository gitlink for thread-per-VM hosts
- Linux thread-per-VM hosts where a VM remains on its owner OS thread
- The pinned Skynet fork with its customized Lua 5.5, where a VM may move
  serially between workers
- Lua and coroutine stacks up to the documented fixed depth

Stock Lua, unlisted Lua versions, Windows/macOS, native C stack unwinding,
tracing, allocation timelines and VM object snapshots are outside V1.

## Submodule development

The parent repository pins exact Lua and Skynet commits. The `branch = luaprof`
entries only identify the collaboration branches; normal builds never float to
their latest remote commits.

When changing a fork, commit and push it first, then update the parent gitlink:

```sh
git -C 3rd/lua-5.4.8 switch luaprof
git -C 3rd/lua-5.4.8 add src/lprofile.c
git -C 3rd/lua-5.4.8 commit -m "describe the Lua change"
git -C 3rd/lua-5.4.8 push origin luaprof
git add 3rd/lua-5.4.8
git commit -m "build: update Lua submodule"
```

Use the same sequence for `integration/skynet`. A fresh checkout should use
`git submodule update --init 3rd/lua-5.4.8 integration/skynet` to restore the
exact pinned commits without downloading unused nested dependencies.

Profiling changes for Skynet Lua belong in `integration/skynet/3rd/lua` and are
committed as part of the Skynet fork. Do not replace that tree with the parent
Lua fork or pass parent `LUA_INC`/`LUA_LIB` values into the Skynet build. Both
Lua trees expose profiler bridge ABI version 2 and run the same bridge contract
test, while retaining their own VM ABI and host-specific behavior.
