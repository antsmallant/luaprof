local skynet = require "skynet"
local profile = require "luaprof"
local sharetable = require "skynet.sharetable"
require "skynet.manager"

skynet.start(function()
	assert(_VERSION == "Lua 5.5")
	sharetable.loadtable("luaprof_s11", {
		message = "embedded-lua",
		nested = { 17, 23 },
	})
	local shared = assert(sharetable.query("luaprof_s11"))
	assert(shared.message == "embedded-lua")
	assert(shared.nested[1] == 17 and shared.nested[2] == 23)

	-- This short integration smoke test uses 1000Hz to collect samples quickly.
	local cpu_recorder = assert(profile.cpu.start { sample_hz = 1000 })
	local memory_recorder = assert(profile.memory.start {
		sample_bytes = 64 * 1024,
		track_free = true,
	})
	local value = 0
	local keep = {}
	for round = 1, 20 do
		keep = {}
		for i = 1, 10000 do
			keep[i] = { i, tostring(i) }
		end
		for i = 1, 1000000 do
			value = value + i
		end
		skynet.sleep(0)
	end
	assert(value > 0)
	assert(#keep == 10000)
	local memory_result = assert(memory_recorder:stop())
	local memory_stats = memory_result:stats()
	assert(memory_stats.samples > 0)
	assert(memory_stats.inuse_space > 0)
	assert(memory_stats.live_map_overflows == 0)
	for i = 1, 5000000 do
		value = value + i
	end
	local cpu_result = assert(cpu_recorder:stop())
	local cpu_stats = cpu_result:stats()
	assert(cpu_stats.samples > 0)
	assert(cpu_stats.scheduler_workers > 0)
	io.stderr:write(string.format(
		"luaprof skynet smoke: ok cpu=%d lua/c/gc=%d/%d/%d " ..
		"overrun_events/ticks=%d/%d drop/unstable=%d/%d workers=%d " ..
		"memory=%d inuse=%d\n",
		cpu_stats.samples, cpu_stats.sample_lua, cpu_stats.sample_c,
		cpu_stats.sample_gc, cpu_stats.overrun_events,
		cpu_stats.overrun_ticks, cpu_stats.dropped_events,
		cpu_stats.unstable_events, cpu_stats.scheduler_workers,
		memory_stats.samples, memory_stats.inuse_space))
	skynet.abort()
end)
