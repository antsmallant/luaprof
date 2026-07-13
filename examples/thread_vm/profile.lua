local profile = require "luaprof"

local cpu_path = assert(arg[1], "missing CPU output path")
local heap_path = assert(arg[2], "missing heap output path")

local cpu_recorder = assert(profile.cpu.start {
	sample_hz = 500,
})
local memory_recorder = assert(profile.memory.start {
	sample_bytes = 64 * 1024,
	track_free = true,
})

local retained = {}
local checksum = 0
for round = 1, 12 do
	retained = {}
	for i = 1, 10000 do
		retained[i] = { i, tostring(i) }
	end
	for i = 1, 1000000 do
		checksum = checksum + i
	end
end
assert(checksum > 0 and #retained == 10000)

local memory_result = assert(memory_recorder:stop())
for i = 1, 5000000 do
	checksum = checksum + i
end
local cpu_result = assert(cpu_recorder:stop())

assert(cpu_result:write(cpu_path))
assert(memory_result:write(heap_path))

local cpu_stats = cpu_result:stats()
local memory_stats = memory_result:stats()
assert(cpu_stats.samples > 0)
assert(memory_stats.samples > 0)
print(string.format(
	"CPU samples=%d Lua/C/GC=%d/%d/%d dropped=%d",
	cpu_stats.samples, cpu_stats.sample_lua, cpu_stats.sample_c,
	cpu_stats.sample_gc, cpu_stats.dropped_events))
print(string.format(
	"memory samples=%d alloc_space=%d inuse_space=%d live_overflows=%d",
	memory_stats.samples, memory_stats.alloc_space,
	memory_stats.inuse_space, memory_stats.live_map_overflows))
print("wrote " .. cpu_path)
print("wrote " .. heap_path)
