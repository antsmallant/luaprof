local profile = require "luaprof"
local skynet = require "skynet"
require "skynet.manager"

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

local function write_results(cpu_result, memory_result)
	local output_dir = assert(os.getenv("LUAPROF_OUTPUT_DIR"),
		"missing LUAPROF_OUTPUT_DIR")
	assert(cpu_result:write(output_dir .. "/skynet-cpu.pb.gz"))
	assert(memory_result:write(output_dir .. "/skynet-heap.pb.gz"))
end

skynet.start(function()
	assert(_VERSION == "Lua 5.5")
	local cpu_recorder = assert(profile.cpu.start { sample_hz = 250 })
	local memory_recorder = assert(profile.memory.start {
		sample_bytes = 64 * 1024,
		track_free = true,
	})

	local retained = build_retained_cache(20000)
	skynet.sleep(0)
	local checksum = build_temporary_batches(30, 5000)
	skynet.sleep(0)
	checksum = checksum + calculate_orders(30000000)
	skynet.sleep(0)
	checksum = checksum + calculate_discounts(12000000)
	skynet.sleep(0)
	checksum = checksum + format_event_labels(1000000)
	assert(#retained == 20000 and checksum > 0)
	collectgarbage("collect")

	local memory_result = assert(memory_recorder:stop())
	checksum = checksum + calculate_orders(10000000)
	assert(checksum > 0)
	local cpu_result = assert(cpu_recorder:stop())
	write_results(cpu_result, memory_result)

	local cpu_stats = cpu_result:stats()
	local memory_stats = memory_result:stats()
	assert(cpu_stats.samples >= 25)
	assert(cpu_stats.scheduler_workers > 0)
	assert(memory_stats.samples > 0)
	assert(memory_stats.inuse_space > 0)
	io.stderr:write(string.format(
		"luaprof skynet demo: ok cpu=%d lua/c/gc=%d/%d/%d " ..
		"overrun_events/ticks=%d/%d workers=%d memory=%d inuse=%d\n",
		cpu_stats.samples, cpu_stats.sample_lua, cpu_stats.sample_c,
		cpu_stats.sample_gc, cpu_stats.overrun_events,
		cpu_stats.overrun_ticks, cpu_stats.scheduler_workers,
		memory_stats.samples, memory_stats.inuse_space))
	skynet.abort()
end)
