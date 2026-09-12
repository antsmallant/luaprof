#define _POSIX_C_SOURCE 200809L

#include "luaprof/runtime.h"
#include "thread_timer.h"
#include "thread_timer_test.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

int luaopen_luaprof(lua_State *L);

static lp_thread_timer *active_timer;
static char armed_point[32];
static bool armed_error;

lp_status __real_lp_thread_timer_arm(lp_thread_timer *timer,
	uint32_t sample_hz);

lp_status
__wrap_lp_thread_timer_arm(lp_thread_timer *timer, uint32_t sample_hz) {
	lp_status status = __real_lp_thread_timer_arm(timer, sample_hz);
	if (status == LP_OK) {
		active_timer = timer;
	}
	return status;
}

void
lp_lua_module_test_point(lua_State *L, const char *name) {
	if (armed_point[0] == '\0' || strcmp(armed_point, name) != 0) {
		return;
	}
	bool raise_error = armed_error;
	armed_point[0] = '\0';
	armed_error = false;
	assert(active_timer != NULL);
	lp_thread_timer_test_inject_tick(active_timer, 0);
	if (raise_error) {
		luaL_error(L, "injected profiler API failure");
	}
}

static int
arm_test_point(lua_State *L) {
	const char *name = luaL_checkstring(L, 1);
	assert(strlen(name) < sizeof(armed_point));
	(void)snprintf(armed_point, sizeof(armed_point), "%s", name);
	armed_error = lua_toboolean(L, 2) != 0;
	return 0;
}

static int
inject_business_tick(lua_State *L) {
	(void)L;
	assert(active_timer != NULL);
	lp_thread_timer_test_inject_tick(active_timer, 0);
	return 0;
}

static void
run_chunk(lua_State *L, const char *source) {
	assert(luaL_loadbufferx(L, source, strlen(source),
		"@lua_api_profiler_work.lua", NULL) == LUA_OK);
	if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
		fprintf(stderr, "%s\n", lua_tostring(L, -1));
		assert(0);
	}
}

int
main(void) {
	char output_path[] = "/tmp/luaprof-api-work-XXXXXX";
	int output_fd = mkstemp(output_path);
	assert(output_fd >= 0);
	assert(close(output_fd) == 0);

	lua_State *L = luaL_newstate();
	assert(L != NULL);
	luaL_openlibs(L);
	luaL_requiref(L, "luaprof", luaopen_luaprof, 1);
	lua_pop(L, 1);
	lua_pushcfunction(L, arm_test_point);
	lua_setglobal(L, "arm_test_point");
	lua_pushcfunction(L, inject_business_tick);
	lua_setglobal(L, "inject_business_tick");
	lua_pushstring(L, output_path);
	lua_setglobal(L, "profiler_output_path");

	run_chunk(L,
		"local luaprof = require('luaprof')\n"
		"local frozen = assert(luaprof.memory.start({ sample_bytes = 1 }))\n"
		"frozen = assert(frozen:stop())\n"
		"local cpu = assert(luaprof.cpu.start({ sample_hz = 1 }))\n"
		"arm_test_point('memory_start', false)\n"
		"local memory = assert(luaprof.memory.start({ sample_bytes = 1 }))\n"
		"arm_test_point('memory_stop', false)\n"
		"assert(memory:stop())\n"
		"arm_test_point('result_stats', false)\n"
		"assert(frozen:stats().kind == 'memory')\n"
		"arm_test_point('result_write', false)\n"
		"assert(frozen:write(profiler_output_path))\n"
		"arm_test_point('result_stats', true)\n"
		"local ok, err = pcall(frozen.stats, frozen)\n"
		"assert(not ok and err:match('injected profiler API failure'))\n"
		"inject_business_tick()\n"
		"local stats = assert(cpu:stop()):stats()\n"
		"assert(stats.samples == 1, stats.samples)\n"
		"assert(stats.sample_c == 1, stats.sample_c)\n"
		"assert(stats.profiler_overhead_events == 5,\n"
		"  stats.profiler_overhead_events)\n");

	assert(armed_point[0] == '\0');
	lua_close(L);
	assert(unlink(output_path) == 0);
	puts("luaprof Lua API profiler work: ok");
	return EXIT_SUCCESS;
}
