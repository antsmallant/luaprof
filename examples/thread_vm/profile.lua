local profile = require "luaprof"

local cpu_path = assert(arg[1], "missing CPU output path")
local heap_path = assert(arg[2], "missing heap output path")

local function calculate_orders(iterations)
	local checksum = 0
	for i = 1, iterations do
		checksum = (checksum + i % 97) % 1000000007
	end
	return checksum
end

local function calculate_discounts(iterations)
	local checksum = 7
	for i = 1, iterations do
		checksum = (checksum * 33 + i % 31) % 1000000007
	end
	return checksum
end

local function format_event_labels(count)
	local bytes = 0
	for i = 1, count do
		bytes = bytes + #tostring(i)
	end
	return bytes
end

local function build_temporary_batches(rounds, width)
	local checksum = 0
	for round = 1, rounds do
		local batch = {}
		for i = 1, width do
			batch[i] = { id = i, label = tostring(i), round = round }
		end
		checksum = checksum + #batch
	end
	return checksum
end

local function build_retained_cache(count)
	local cache = {}
	for i = 1, count do
		cache[i] = {
			id = i,
			label = "retained-" .. tostring(i),
			metadata = { bucket = i % 64 },
		}
	end
	return cache
end

local function run_profiled_workload()
	local retained = build_retained_cache(30000)
	local temporary = build_temporary_batches(50, 6000)
	local primary = calculate_orders(60000000)
	local secondary = calculate_discounts(24000000)
	local formatted = format_event_labels(1500000)
	return retained, temporary + primary + secondary + formatted
end

local function post_memory_cpu_work()
	return calculate_orders(20000000)
end

local cpu_recorder = assert(profile.cpu.start {
	sample_hz = 250,
})
local memory_recorder = assert(profile.memory.start {
	sample_bytes = 64 * 1024,
	track_free = true,
})

local retained, checksum = run_profiled_workload()
assert(#retained == 30000 and checksum > 0)
collectgarbage("collect")

local memory_result = assert(memory_recorder:stop())
checksum = checksum + post_memory_cpu_work()
assert(checksum > 0)
local cpu_result = assert(cpu_recorder:stop())

assert(cpu_result:write(cpu_path))
assert(memory_result:write(heap_path))

local cpu_stats = cpu_result:stats()
local memory_stats = memory_result:stats()
assert(cpu_stats.samples >= 50)
assert(memory_stats.samples > 0)
assert(memory_stats.inuse_space > 0)
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
