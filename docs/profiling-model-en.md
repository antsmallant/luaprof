# Sampling model and profile interpretation

[中文](profiling-model.md) | [README](../READ-en.md) |
[Integration](integration-en.md) | [Maintainer guide](maintainer-guide-en.md)

This document describes the `luaprof` Lua API, CPU and memory sampling
semantics, output formats, quality statistics, and fixed limits.

## 1. Recorder lifecycle

CPU and memory recorders are independent. There is no shared
`profile.start()` mode table:

```lua
local profile = require "luaprof"

local cpu = assert(profile.cpu.start {
    sample_hz = 100,
})
local memory = assert(profile.memory.start {
    sample_bytes = 512 * 1024,
    track_free = true,
})

-- workload

local memory_result = assert(memory:stop())
-- CPU profiling is still active.
local cpu_result = assert(cpu:stop())

assert(cpu_result:write("cpu.pb.gz"))
assert(memory_result:write("heap.pb.gz"))
```

`profile.cpu.start([options])`:

- `sample_hz` is an integer from 1 to 10000 and defaults to 100Hz.
- The thread-per-VM backend measures thread CPU time, so sleep produces no
  samples.
- The Skynet backend measures thread CPU time while the target service executes
  on a worker.

`profile.memory.start([options])`:

- `sample_bytes` is a positive integer and defaults to 512KiB. It is the
  expected byte interval between samples.
- `track_free` is a boolean and defaults to `false`. Enabling it produces
  sampled in-use metrics at stop time.
- `sample_bytes = 1` selects every successful allocation and realloc.

One Lua VM may have at most one active recorder of each kind, while CPU and
memory can run and stop independently. Recorder `__gc` and `__close` stop and
discard an active recording. Call `stop()` explicitly when a result is needed.

A stopped result owns a frozen profile:

- `result:stats()` returns counters and quality metadata.
- `result:write(path[, options])` exports pprof or folded stacks.

Unknown options and invalid option types raise Lua argument errors. Host and
lifecycle failures return `nil, error`.

## 2. CPU sampling model

### 2.1 Thread-per-VM

The thread-per-VM backend uses `CLOCK_THREAD_CPUTIME_ID`. A timer tick records
only a small VM-state snapshot; it does not walk Lua or native stacks in the
signal handler. The recorder consumes the pending tick and captures the Lua
stack at the next VM safe point.

Sample weight therefore corresponds to real timer ticks, while the stack is
the first safe Lua stack after the tick. This avoids accessing unstable VM data
asynchronously. The VM must remain on the OS thread that started the recorder.

### 2.2 Skynet service

The Skynet backend is process-wide infrastructure, but each profile target is
one service:

- `profile.cpu.start()` reads the service handle in the current dispatch and
  registers it as a target.
- Every worker uses its own thread CPU timer.
- Dispatch hooks publish samples only when the current handle matches an active
  target.
- If the service migrates, its handle continues the profile on the new worker.
- Other services, queueing time, and sleep are excluded from that result.

Multiple services may record independent profiles. All concurrently active CPU
recorders currently must use the same `sample_hz`. The Lua API has no combined
whole-process Skynet profile and does not let one service select an arbitrary
other handle.

### 2.3 Lua, C, GC, and host states

Samples are attributed to Lua, CFunction, GC, or host VM state. A tick during a
long C call preserves the current `lua_CFunction` pointer and its Lua caller;
the hot path does not unwind a native C stack.

At export time, the profiler scans CFunction bindings reachable from `_G` and
`package.loaded`, then reads native ELF mappings and symbol tables. Names are
selected in this order:

1. Lua-visible binding name;
2. native symbol;
3. raw `lua_CFunction@0x...` address.

When visible and native names both exist and differ, the result looks like
`tostring [luaB_tostring]`. For multiple Lua aliases of one pointer, the
shortest name wins, with lexical order as a tie-breaker.

Symbol scanning occurs only in `result:write()`. It is not in the signal
handler, allocation callback, or VM instruction fast path. A stripped, moved,
or non-ELF binary, or an uncovered binding, retains the address fallback. The
profile stores native mapping paths; symbolizing on another machine requires
matching original binaries.

This identifies the current CFunction but does not reconstruct its native C
stack. Use a native profiler to find hot C/C++ lines. GC and host states use
synthetic frames.

### 2.4 CPU statistics

Important fields are:

- `samples`: attributed timer-tick weight, including timer overruns.
- `sample_lua`, `sample_c`, `sample_gc`, `sample_host`: weight by VM state.
- `safe_points`, `pending_weight`: safe-point consumption and request weight.
- `state_lua`, `state_c`, `state_gc`, `state_host`: VM state transitions.
- `dropped_events`: ticks lost because the fixed event ring was full.
- `unstable_events`: ticks rejected during execution-slot publication races.
- `profiler_overhead_events`: ticks arriving while profile data was collected.
- `stale_events`: old ticks rejected at Skynet generation or migration
  boundaries.
- `scheduler_workers`: workers actually used by a Skynet target.
- `stack_truncations`, `aggregate_overflows`, `symbol_overflows`: bounded-store
  quality counters.

To judge profile health, compare `samples`, recording duration, and configured
frequency; inspect VM-state proportions; and ensure drops and overflows are
negligible. A very short profile cannot provide enough samples for a reliable
conclusion.

## 3. Memory sampling model

### 3.1 Random sampling

Allocation intervals follow a geometric distribution whose expected byte
interval is `sample_bytes`. An allocation is selected in proportion to its
complete requested new size, produces at most one sample, and uses inverse
probability weighting to estimate allocation bytes and objects.

Free and failed realloc do not consume the sampling budget. Successful realloc
ends the old block and performs one allocation using the complete new requested
size. Events come from Lua's internal allocator wrapper and include the exact
`lua_State *`, old/new pointers, old/new requested sizes, and success state.

A memory profile covers allocator events from the target Lua VM only. It is not
process RSS, physical memory, Skynet C-layer allocation, or a VM-object heap
snapshot.

### 3.2 Alloc-space and in-use

With `track_free = false`, no live-pointer map is allocated or queried. Only
alloc-space metrics are produced, and in-use metrics stay zero.

With `track_free = true`, only sampled live blocks are stored. Free uses the
pointer to find its sampled allocation and removes it from the stop-time in-use
result. Unsampled pointers are not stored. The profiler does not capture free
sites, lifetimes, peaks, or per-object timelines.

`sample_bytes = 1` provides exact allocator-requested alloc-space and in-use
values. Larger intervals are statistical estimates. Increasing the interval
reduces stack-capture work but raises variance, especially for short profiles
or a small number of live objects.

While Lua relocates its VM stack, call-frame pointers are temporarily invalid.
A selected stack-reallocation event still contributes to memory metrics, but
is stored with an empty stack and increments `stack_truncations`.

### 3.3 Memory statistics

- `allocation_events`, `reallocation_events`, `free_events`: exact allocator
  event counts during the recording.
- `allocation_failures`: failed allocation/realloc count.
- `samples`, `sampled_alloc_bytes`: raw selected event count and requested
  bytes.
- `alloc_space`, `alloc_objects`: probability-weighted allocation estimates.
- `inuse_space`, `inuse_objects`: weighted sampled blocks still live at stop.
- `live_map_overflows`: sampled live blocks that could not enter in-use
  tracking.
- `stack_truncations`, `aggregate_overflows`, `symbol_overflows`: quality
  counters.

Before using in-use conclusions, require `track_free = true`, check that
`live_map_overflows == 0`, and decide whether the raw `samples` count is large
enough. A few large objects or a short recording can have substantial sampling
variance.

## 4. Export and pprof

The default format is gzip-compressed Google `profile.proto`:

```lua
assert(cpu_result:write("cpu.pb.gz"))
assert(memory_result:write("heap.pb.gz", {
    sample = "alloc_space",
}))
```

CPU profiles contain:

- `samples/count`
- `cpu/nanoseconds`

Memory profiles contain:

- `alloc_objects/count`
- `alloc_space/bytes`
- `inuse_objects/count`
- `inuse_space/bytes`

The default sample is `cpu` for CPU, `alloc_space` for memory without free
tracking, and `inuse_space` with free tracking.

Common commands:

```sh
go tool pprof -top cpu.pb.gz
go tool pprof -lines -top cpu.pb.gz
go tool pprof -list=calculate_orders cpu.pb.gz
go tool pprof -sample_index=alloc_space -top heap.pb.gz
go tool pprof -sample_index=inuse_space -top heap.pb.gz
go tool pprof -sample_index=inuse_objects -top heap.pb.gz
go tool pprof -sample_index=inuse_space -svg heap.pb.gz > heap.svg
go tool pprof -http=:0 heap.pb.gz
```

Default `-top` aggregates by function, `-lines -top` splits by executing line,
and `-list` annotates source lines. A Lua frame's function name, definition
line, and current line are independent data. The call name is a best-effort VM
inference at the sample point; anonymous calls fall back to source and
definition line.

Skynet loads the outer examples while running under `integration/skynet`. From
the repository root, use this source path for listings:

```sh
go tool pprof -source_path=examples/skynet \
    -list=calculate_orders build/skynet-cpu.pb.gz
```

Folded root-to-leaf stacks are also available for flame-graph tools:

```lua
assert(memory_result:write("heap.folded", {
    format = "folded",
    sample = "inuse_space",
}))
```

## 5. Fixed bounds

Hot-path storage is preallocated and does not grow while recording:

| Resource | Limit per recorder or host |
| --- | ---: |
| Captured stack depth | 64 frames |
| CPU symbols / stack aggregates / source bytes | 4096 / 2048 / 256KiB |
| Memory symbols / stack aggregates / source bytes | 4096 / 2048 / 256KiB |
| Call name per sampled Lua function | 255 bytes |
| Sampled live blocks with `track_free` | 16384 |
| Thread or Skynet timer event ring | 4096 entries |
| Thread timers / Skynet targets / Skynet workers | 64 / 128 / 64 |

Source names over 1024 bytes and Lua call names over 255 bytes are truncated
and increment `symbol_overflows`. Reaching another recording bound keeps memory
bounded and increments the relevant drop, truncation, or overflow counter.

Export occurs after stop and may allocate. Its Lua-visible CFunction scan has
separate limits of 4096 functions, 4096 tables, six levels, and 255-byte names.
Uncovered functions still try native symbols and finally retain raw addresses.

## 6. Known limitations

V1 does not provide:

- native C stack unwinding;
- call/return tracing;
- allocation timelines, free sites, lifetimes, or peak profiles;
- VM object graphs or heap snapshots;
- RSS, physical memory, or process memory outside the Lua allocator;
- Windows or macOS backends;
- ABI compatibility guarantees for unlisted Lua versions.

For an existing host, continue with the [integration guide](integration-en.md).
For fork, submodule, or release work, read the
[maintainer guide](maintainer-guide-en.md).
