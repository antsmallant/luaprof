local skynet = require "skynet"
local profile = require "luaprof"
require "skynet.manager"

skynet.start(function()
	local recorder = assert(profile.cpu.start { sample_hz = 1000 })
	local value = 0
	for round = 1, 20 do
		for i = 1, 1000000 do
			value = value + i
		end
		skynet.sleep(0)
	end
	assert(value > 0)
	local result = assert(recorder:stop())
	local stats = result:stats()
	assert(stats.samples > 0)
	assert(stats.scheduler_workers > 0)
	io.stderr:write(string.format(
		"luaprof skynet smoke: ok samples=%d workers=%d\n",
		stats.samples, stats.scheduler_workers))
	skynet.abort()
end)
