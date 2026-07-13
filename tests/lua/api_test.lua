local profile = require "luaprof"

assert(profile._VERSION == "0.1.0")
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
assert(memory_stats.samples >= 0)
assert(memory_stats.sampled_alloc_bytes >= memory_stats.samples)
assert(memory_stats.alloc_space >= memory_stats.sampled_alloc_bytes)
assert(memory_stats.alloc_objects >= memory_stats.samples)
assert(memory_stats.inuse_space >= 0)
assert(memory_stats.inuse_objects >= 0)
assert(memory_stats.inuse_space <= memory_stats.alloc_space)
assert(memory_stats.inuse_objects <= memory_stats.alloc_objects)
assert(memory_stats.live_map_overflows == 0)

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
assert(default_stats.inuse_space == 0)
assert(default_stats.inuse_objects == 0)

local cpu_first = assert(profile.cpu.start())
local memory_second = assert(profile.memory.start { sample_bytes = 1 })
assert(cpu_first:stop():stats().kind == "cpu")
local function grow_stack(depth)
    if depth == 0 then
        return {}
    end
    local result = grow_stack(depth - 1)
    return result
end
assert(type(grow_stack(400)) == "table")
local exact_stats = assert(memory_second:stop()):stats()
assert(exact_stats.kind == "memory")
assert(exact_stats.samples > 0)
assert(exact_stats.stack_truncations > 0)
assert(exact_stats.alloc_space == exact_stats.sampled_alloc_bytes)
assert(exact_stats.alloc_objects == exact_stats.samples)
assert(exact_stats.inuse_space == 0)
assert(exact_stats.inuse_objects == 0)

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

local cpu_path = "/tmp/luaprof-api-cpu.pb.gz"
local memory_path = "/tmp/luaprof-api-memory.pb.gz"
local folded_path = "/tmp/luaprof-api-memory.folded"
assert(cpu_result:write(cpu_path))
assert(memory_result:write(memory_path, { sample = "alloc_space" }))
assert(memory_result:write(folded_path, {
    format = "folded",
    sample = "inuse_space",
}))
local unwritten, write_error = memory_result:write(folded_path, {
    format = "folded",
    sample = "cpu",
})
assert(unwritten == nil)
assert(write_error:match "not available")
assert(os.remove(cpu_path))
assert(os.remove(memory_path))
assert(os.remove(folded_path))

print("luaprof Lua API lifecycle: ok")
