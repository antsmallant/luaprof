#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

typedef struct bridge_stats {
	lua_State *main_state;
	void *main_block;
	unsigned int safe_calls;
	unsigned int safe_weight;
	unsigned int states[4];
	unsigned int allocations;
	unsigned int reallocations;
	unsigned int frees;
	unsigned int failures;
	int last_state;
	lua_CFunction last_cfunction;
	int saw_request_cfunction;
	int saw_c_unwind;
	int saw_coroutine_event;
	int saw_coroutine_restore;
	int saw_closethread_restore;
	int saw_main_block_free;
	lua_State *last_event_state;
} bridge_stats;

typedef struct allocator_context {
	void *main_block;
	unsigned int reject_any;
	int reject_realloc_growth;
} allocator_context;

static void *
test_allocator(void *userdata, void *pointer, size_t old_size,
	size_t new_size) {
	allocator_context *allocator = userdata;
	if (new_size == 0) {
		free(pointer);
		return NULL;
	}
	if (allocator->reject_any != 0) {
		allocator->reject_any--;
		return NULL;
	}
	if (allocator->reject_realloc_growth && pointer != NULL &&
		new_size > old_size) {
		return NULL;
	}
	void *result = realloc(pointer, new_size);
	if (pointer == NULL && allocator->main_block == NULL) {
		allocator->main_block = result;
	}
	return result;
}

static void
safe_point(void *userdata, lua_State *L, unsigned int pending) {
	bridge_stats *stats = userdata;
	assert(pending != 0);
	stats->safe_calls++;
	stats->safe_weight += pending;
	if (L != stats->main_state) {
		stats->saw_coroutine_event = 1;
	}
}

static void
state_change(void *userdata, lua_State *L, int state,
	lua_CFunction cfunction) {
	bridge_stats *stats = userdata;
	assert(state >= LUA_PROFILE_HOST && state <= LUA_PROFILE_GC);
	if (state == LUA_PROFILE_C) {
		assert(cfunction != NULL);
	}
	else {
		assert(cfunction == NULL);
	}
	if (state == LUA_PROFILE_HOST && stats->last_state == LUA_PROFILE_C) {
		stats->saw_c_unwind = 1;
	}
	if (L == stats->main_state && state == LUA_PROFILE_C &&
		stats->last_event_state != NULL &&
		stats->last_event_state != stats->main_state &&
		stats->last_state == LUA_PROFILE_HOST) {
		stats->saw_coroutine_restore = 1;
	}
	stats->states[state]++;
	stats->last_state = state;
	stats->last_cfunction = cfunction;
	stats->last_event_state = L;
	if (L != stats->main_state) {
		stats->saw_coroutine_event = 1;
	}
}

static void
allocation(void *userdata, lua_State *L,
	const lua_ProfileAllocEvent *event) {
	bridge_stats *stats = userdata;
	if (!event->success) {
		assert(event->new_pointer == NULL);
		assert(event->new_size != 0);
		stats->failures++;
	}
	else if (event->old_pointer == NULL) {
		assert(event->new_pointer != NULL);
		assert(event->old_size == 0);
		assert(event->new_size != 0);
		stats->allocations++;
	}
	else if (event->new_pointer == NULL) {
		assert(event->new_size == 0);
		stats->frees++;
		if (event->old_pointer == stats->main_block) {
			stats->saw_main_block_free = 1;
		}
	}
	else {
		assert(event->new_size != 0);
		stats->reallocations++;
	}
	if (L != stats->main_state) {
		stats->saw_coroutine_event = 1;
	}
}

static int
request_ticks(lua_State *L) {
	lua_CFunction current = NULL;
	bridge_stats *stats = lua_touserdata(L, lua_upvalueindex(1));
	assert(lua_getprofilestate(L, &current) == LUA_PROFILE_C);
	assert(current == request_ticks);
	stats->saw_request_cfunction = 1;
	lua_profile_request(L, (unsigned int)luaL_checkinteger(L, 1));
	return 0;
}

static int
call_lua(lua_State *L) {
	luaL_checktype(L, 1, LUA_TFUNCTION);
	lua_settop(L, 1);
	lua_call(L, 0, 1);
	return 1;
}

static int
raise_from_c(lua_State *L) {
	return luaL_error(L, "expected C failure");
}

static int
yield_continuation(lua_State *L, int status, lua_KContext context) {
	(void)status;
	(void)context;
	lua_pushliteral(L, "continued");
	return 1;
}

static int
yield_from_c(lua_State *L) {
	lua_pushliteral(L, "from-c");
	return lua_yieldk(L, 1, 0, yield_continuation);
}

static int
fail_next_allocations(lua_State *L) {
	allocator_context *allocator = lua_touserdata(L, lua_upvalueindex(1));
	allocator->reject_any = 2;
	return 0;
}

static int
close_coroutine(lua_State *L) {
	bridge_stats *stats = lua_touserdata(L, lua_upvalueindex(1));
	lua_State *co = lua_newthread(L);
	assert(lua_closethread(co, L) == LUA_OK);
	assert(stats->last_event_state == L);
	assert(stats->last_state == LUA_PROFILE_C);
	assert(stats->last_cfunction == close_coroutine);
	stats->saw_closethread_restore = 1;
	return 0;
}

static void
set_function(lua_State *L, const char *name, lua_CFunction function,
	void *upvalue) {
	if (upvalue == NULL) {
		lua_pushcfunction(L, function);
	}
	else {
		lua_pushlightuserdata(L, upvalue);
		lua_pushcclosure(L, function, 1);
	}
	lua_setglobal(L, name);
}

static void
run_workload(lua_State *L) {
	static const char workload[] =
		"local function inner()\n"
		"  request_ticks(7)\n"
		"  local t = {}\n"
		"  for i = 1, 100 do t[i] = { i } end\n"
		"  return 42\n"
		"end\n"
		"assert(call_lua(inner) == 42)\n"
		"local ok = pcall(raise_from_c)\n"
		"assert(not ok)\n"
		"local co = coroutine.create(function()\n"
		"  local t = { key = 'value' }\n"
		"  request_ticks(11)\n"
		"  coroutine.yield('lua-yield')\n"
		"  error('coroutine-error')\n"
		"end)\n"
		"local resumed, value = coroutine.resume(co)\n"
		"assert(resumed and value == 'lua-yield')\n"
		"resumed = coroutine.resume(co)\n"
		"assert(not resumed)\n"
		"local cco = coroutine.create(function() return yield_from_c() end)\n"
		"resumed, value = coroutine.resume(cco)\n"
		"assert(resumed and value == 'from-c')\n"
		"resumed, value = coroutine.resume(cco)\n"
		"assert(resumed and value == 'continued')\n"
		"close_coroutine()\n"
		"collectgarbage('collect')\n"
		"ok = pcall(function() fail_next_allocations(); return {} end)\n"
		"assert(not ok)\n"
		"local sum = 0\n"
		"for i = 1, 1000 do sum = sum + i end\n"
		"return sum\n";

	assert(luaL_loadbufferx(L, workload, sizeof(workload) - 1,
		"@vm_bridge_workload.lua", NULL) == LUA_OK);
	assert(lua_pcall(L, 0, 1, 0) == LUA_OK);
	assert(lua_tointeger(L, -1) == 500500);
	lua_pop(L, 1);
}

static void
force_realloc_failure(lua_State *L, allocator_context *allocator) {
	char *source = malloc(128 * 1024);
	assert(source != NULL);
	size_t used = 0;
	for (unsigned int i = 0; i < 5000; ++i) {
		int written = snprintf(source + used, 128 * 1024 - used,
			"local value_%u = %u\n", i, i);
		assert(written > 0);
		used += (size_t)written;
		assert(used < 128 * 1024);
	}

	allocator->reject_realloc_growth = 1;
	int status = luaL_loadbufferx(L, source, used,
		"@realloc_failure.lua", NULL);
	allocator->reject_realloc_growth = 0;
	free(source);
	assert(status == LUA_ERRMEM);
	lua_pop(L, 1);
}

int
main(void) {
	allocator_context allocator = { 0 };
	lua_State *L = lua_newstate(test_allocator, &allocator);
	assert(L != NULL);
	luaL_openlibs(L);

	bridge_stats stats = {
		.main_state = L,
		.main_block = allocator.main_block,
		.last_state = LUA_PROFILE_HOST,
	};
	lua_ProfileHooks hooks = {
		.safe_point = safe_point,
		.state_change = state_change,
		.allocation = allocation,
	};
	lua_profile_request(L, 1000);
	lua_setprofilehooks(L, &hooks, &stats);

	set_function(L, "request_ticks", request_ticks, &stats);
	set_function(L, "call_lua", call_lua, NULL);
	set_function(L, "raise_from_c", raise_from_c, NULL);
	set_function(L, "yield_from_c", yield_from_c, NULL);
	set_function(L, "fail_next_allocations", fail_next_allocations,
		&allocator);
	set_function(L, "close_coroutine", close_coroutine, &stats);

	lua_profile_request(L, 2);
	lua_setprofilehooks(L, &hooks, &stats);
	lua_profile_request(L, 3);
	run_workload(L);
	force_realloc_failure(L, &allocator);

	lua_CFunction current = (lua_CFunction)request_ticks;
	assert(lua_getprofilestate(L, &current) == LUA_PROFILE_HOST);
	assert(current == NULL);
	assert(stats.safe_calls >= 3);
	assert(stats.safe_weight == 23);
	assert(stats.states[LUA_PROFILE_LUA] != 0);
	assert(stats.states[LUA_PROFILE_C] != 0);
	assert(stats.states[LUA_PROFILE_GC] != 0);
	assert(stats.states[LUA_PROFILE_HOST] != 0);
	assert(stats.saw_request_cfunction);
	assert(stats.saw_c_unwind);
	assert(stats.saw_coroutine_event);
	assert(stats.saw_coroutine_restore);
	assert(stats.saw_closethread_restore);
	assert(stats.allocations != 0);
	assert(stats.reallocations != 0);
	assert(stats.frees != 0);
	assert(stats.failures >= 2);

	unsigned int allocations = stats.allocations;
	unsigned int states = stats.states[LUA_PROFILE_LUA] +
		stats.states[LUA_PROFILE_C] + stats.states[LUA_PROFILE_GC] +
		stats.states[LUA_PROFILE_HOST];
	lua_setprofilehooks(L, NULL, NULL);
	assert(luaL_dostring(L, "local t = {}; for i = 1, 100 do t[i] = i end")
		== LUA_OK);
	lua_gc(L, LUA_GCCOLLECT);
	assert(stats.allocations == allocations);
	assert(states == stats.states[LUA_PROFILE_LUA] +
		stats.states[LUA_PROFILE_C] + stats.states[LUA_PROFILE_GC] +
		stats.states[LUA_PROFILE_HOST]);
	lua_close(L);

	allocator_context close_allocator = { 0 };
	lua_State *closing = lua_newstate(test_allocator, &close_allocator);
	assert(closing != NULL);
	luaL_openlibs(closing);
	bridge_stats close_stats = {
		.main_state = closing,
		.main_block = close_allocator.main_block,
		.last_state = LUA_PROFILE_HOST,
	};
	lua_setprofilehooks(closing, &hooks, &close_stats);
	assert(luaL_dostring(closing,
		"local t = {}; for i = 1, 100 do t[i] = { i } end") == LUA_OK);
	lua_close(closing);
	assert(close_stats.frees != 0);
	assert(close_stats.saw_main_block_free);

	puts("luaprof Lua VM bridge: ok");
	return EXIT_SUCCESS;
}
