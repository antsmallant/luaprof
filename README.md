# luaprof

Sampling profiler for PUC Lua 5.4.8. CPU and memory recorders are independent;
Skynet is a supported host integration, not a core dependency.

The profiler implementation is under development. Linux thread-per-VM hosts and
Skynet services can collect CPU samples through thread CPU-time timers. The
Skynet backend tracks the target VM across worker dispatches. Memory recorders
collect requested alloc-space with allocation-proportional sampling.

## Build the default host

The default build initializes only the required Lua submodule and builds a
minimal thread-per-VM program. A C11 toolchain, POSIX threads and zlib development
files are required:

```sh
make
make test
```

## Build the Skynet integration

The explicit integration target initializes Skynet and links it against the
parent project's Lua submodule:

```sh
make skynet
make test-skynet
```

Both submodules use their `luaprof` branches. The parent repository records
exact gitlinks; the configured branch names are for intentional remote updates,
not floating builds.

## Recorder lifecycle

The V1 API uses independent recorder handles:

```lua
local profile = require "luaprof"

local cpu = assert(profile.cpu.start {
    sample_hz = 100,
})
local memory = assert(profile.memory.start {
    sample_bytes = 512 * 1024,
    track_free = true,
})

local cpu_result = assert(cpu:stop())
local memory_result = assert(memory:stop())
```

Only one recorder of each kind may be active in a Lua VM. Stopping either
recorder does not affect the other. The Linux thread-per-VM backend also permits
only one active CPU timer on an OS thread. It uses thread CPU time, so sleeping
does not produce samples.

Skynet owns one timer per worker and publishes a target only while its service
callback is running. Multiple Skynet CPU recorders may run concurrently, but
they currently must use the same `sample_hz`.

CPU results currently expose aggregate and quality counters through the C API,
with lifecycle and scheduler quality metadata available through Lua
`result:stats()`.

Memory sampling uses geometrically distributed byte intervals whose mean is
`sample_bytes`. A successful allocation or realloc consumes its full requested
new size; frees and failed reallocs do not consume the sampling budget. Each
event produces at most one sample. Samples are probability-weighted to estimate
`alloc_space` and `alloc_objects`, while `sampled_alloc_bytes` and `samples`
report raw observations. Setting `sample_bytes = 1` records every successful
allocation and realloc.

With `track_free = false`, the recorder does not allocate or query a live-pointer
map and reports alloc-space only. With `track_free = true`, sampled live blocks
are tracked until a matching free or successful realloc and are reported as
`inuse_space` and `inuse_objects` on their original allocation stacks. Failed
reallocs leave the old block live; successful reallocs end the old sampled block
and treat the complete new block as a new allocation sample. Free-site,
lifetime, peak and object-timeline profiling are intentionally not collected.

The live map is preallocated for 16384 sampled blocks. Additional sampled live
blocks still contribute alloc-space but not in-use values, and are reported by
`live_map_overflows`.

## Export results

`result:write()` writes a gzip-compressed `profile.proto` file by default:

```lua
assert(cpu_result:write("cpu.pb.gz"))
assert(memory_result:write("heap.pb.gz", {
    sample = "alloc_space",
}))
```

CPU profiles contain `samples/count` and `cpu/nanoseconds`. Memory profiles
contain `alloc_objects/count`, `alloc_space/bytes`, `inuse_objects/count` and
`inuse_space/bytes`. `sample` selects the default metric; it defaults to `cpu`,
`alloc_space` when free tracking is disabled, and `inuse_space` when enabled.
Lua frames are named from source and definition line. Native C symbol lookup is
not required: C frames are emitted as `lua_CFunction@0x...`.

The files can be read by Google pprof, for example:

```sh
go tool pprof -top cpu.pb.gz
go tool pprof -sample_index=alloc_space -top heap.pb.gz
go tool pprof -sample_index=inuse_space -svg heap.pb.gz > heap.svg
go tool pprof -http=:0 heap.pb.gz
```

SVG output requires Graphviz (`dot`); the interactive web view may also invoke
Graphviz for graph reports.

Folded stacks are available for flame graph tooling:

```lua
assert(memory_result:write("heap.folded", {
    format = "folded",
    sample = "inuse_space",
}))
```
