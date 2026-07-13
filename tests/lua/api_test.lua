local profile = require "luaprof"

assert(type(profile.cpu.start) == "function")
assert(type(profile.memory.start) == "function")
assert(profile.start == nil)

local cpu = assert(profile.cpu.start { sample_hz = 200 })
local duplicate, duplicate_error = profile.cpu.start()
assert(duplicate == nil)
assert(duplicate_error:match "already running")

local memory = assert(profile.memory.start {
    sample_bytes = 4096,
    track_free = true,
})

local bridge_allocations = {}
for i = 1, 100 do
    bridge_allocations[i] = { value = i }
end

local memory_result = assert(memory:stop())
local memory_stats = memory_result:stats()
assert(memory_stats.kind == "memory")
assert(memory_stats.sample_bytes == 4096)
assert(memory_stats.track_free == true)
assert(memory_stats.active == false)
assert(memory_stats.allocation_events > 0)
assert(memory_stats.reallocation_events > 0)

local cpu_result = assert(cpu:stop())
local cpu_stats = cpu_result:stats()
assert(cpu_stats.kind == "cpu")
assert(cpu_stats.generation ~= memory_stats.generation)
assert(cpu_stats.sample_hz == 200)
assert(cpu_stats.state_lua > 0)
assert(cpu_stats.state_c > 0)

local stopped, stopped_error = cpu:stop()
assert(stopped == nil)
assert(stopped_error:match "already stopped")

local memory_default = assert(profile.memory.start())
local default_stats = assert(memory_default:stop()):stats()
assert(default_stats.sample_bytes > 1)
assert(default_stats.track_free == false)

local cpu_first = assert(profile.cpu.start())
local memory_second = assert(profile.memory.start { sample_bytes = 1 })
assert(cpu_first:stop():stats().kind == "cpu")
assert(memory_second:stop():stats().kind == "memory")

local ok = pcall(profile.memory.start, { sample_bytes = 0 })
assert(not ok)
ok = pcall(profile.memory.start, { track_free = 1 })
assert(not ok)
ok = pcall(profile.memory.start, { typo = true })
assert(not ok)
ok = pcall(profile.cpu.start, { typo = true })
assert(not ok)
ok = pcall(profile.cpu.start, { sample_hz = 0 })
assert(not ok)
ok = pcall(profile.cpu.start, { sample_hz = 10001 })
assert(not ok)

local abandoned = assert(profile.cpu.start())
abandoned = nil
collectgarbage "collect"
assert(profile.cpu.start():stop())

local unwritten, write_error = cpu_result:write("unused.pb.gz")
assert(unwritten == nil)
assert(write_error:match "not implemented")

print("luaprof Lua API lifecycle: ok")
