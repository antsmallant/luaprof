# luaprof

Sampling profiler for PUC Lua 5.4.8. CPU and memory recorders are independent;
Skynet is a supported host integration, not a core dependency.

The profiler implementation is under development. The current repository
bootstrap verifies both supported host shapes before the sampling core is added.

## Build the default host

The default build initializes only the required Lua submodule and builds a
minimal thread-per-VM program:

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

local cpu = assert(profile.cpu.start())
local memory = assert(profile.memory.start {
    sample_bytes = 512 * 1024,
    track_free = true,
})

local cpu_result = assert(cpu:stop())
local memory_result = assert(memory:stop())
```

Only one recorder of each kind may be active in a Lua VM. Stopping either
recorder does not affect the other. Sampling collection and pprof export are
implemented in later milestones; the current result handles expose lifecycle
metadata through `result:stats()`.
