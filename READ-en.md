# luaprof

[中文](README.md)

`luaprof` is a Linux sampling profiler for PUC Lua. It provides independent CPU
and memory recorders and writes standard pprof profiles. It currently supports
thread-per-VM hosts using pinned Lua 5.4.8 and Lua 5.5.0 forks, plus the pinned
Skynet fork with its customized Lua 5.5. V1 does not include call/return
tracing.

## Requirements

- Linux, a C11 compiler, GNU Make, and POSIX threads
- zlib development files
- Go `pprof`; Graphviz is also required for SVG or graph-based reports
- HTTPS access to GitHub; public submodules do not require an SSH key

Stock Lua does not provide the VM bridge required by `luaprof`. The default
build uses the pinned Lua 5.4.8 fork. Explicit Lua 5.5 and Skynet targets use
the pinned Lua 5.5.0 fork and Skynet's own customized Lua, respectively.

## Build and test

```sh
git clone https://github.com/antsmallant/luaprof.git
cd luaprof
make
make test
```

Lua 5.5 has a separate build artifact and test entry point:

```sh
make test-lua55
make example-lua55
```

Run the thread-per-VM example:

```sh
make example-thread-vm
```

The example writes:

```text
build/thread-vm-cpu.pb.gz
build/thread-vm-heap.pb.gz
```

Inspect the results:

```sh
go tool pprof -top build/thread-vm-cpu.pb.gz
go tool pprof -lines -top build/thread-vm-cpu.pb.gz
go tool pprof -sample_index=inuse_space -top build/thread-vm-heap.pb.gz
```

## Basic usage

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
local cpu_result = assert(cpu:stop())

assert(memory_result:write("heap.pb.gz"))
assert(cpu_result:write("cpu.pb.gz"))
```

CPU and memory recorders for one VM can start and stop independently. With
`track_free = true`, the memory profile estimates sampled allocations that are
still live when the recorder stops.

## Skynet

Build, test, and run the pinned Skynet example:

```sh
make test-skynet
make example-skynet
```

The example writes:

```text
build/skynet-cpu.pb.gz
build/skynet-heap.pb.gz
```

A Skynet CPU profile targets the service that calls `profile.cpu.start()` and
continues following that service when it migrates between workers. It is not a
combined profile of the whole Skynet process. See the integration guide below.

## Documentation

- [Integration guide](docs/integration-en.md): modify a custom Lua and integrate
  a thread-per-VM or Skynet host.
- [Sampling model and profile interpretation](docs/profiling-model-en.md): API,
  CPU/memory semantics, pprof, statistics, and limits.
- [Maintainer guide](docs/maintainer-guide-en.md): forks, submodules, dual Lua
  ABIs, and release workflow.

## Supported scope

- Linux thread-per-VM hosts using the pinned PUC Lua 5.4.8 or Lua 5.5.0 fork
- The pinned Skynet fork and its customized Lua 5.5
- Lua/CFunction/GC attribution and Lua stacks
- pprof and folded-stack export

Windows, macOS, native C stack unwinding, tracing, allocation timelines, and VM
object snapshots are outside V1.
