#include "luaprof/runtime.h"
#include "lua_bridge.h"
#include "lua_symbols.h"
#include "pprof_exporter.h"

#include <stdint.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>

#if !defined(LUA_USE_LUAPROF)
#error "luaprof requires Lua built with LUAPROF=1"
#endif

#if defined(LUAPROF_EXPECT_LUA_VERSION)
_Static_assert(LUA_VERSION_NUM == LUAPROF_EXPECT_LUA_VERSION,
	"luaprof module built against an unexpected Lua ABI");
#endif
_Static_assert(LUA_PROFILE_ABI_VERSION == 2,
	"luaprof module built against an unexpected profiler bridge ABI");

#define LP_RUNTIME_METATABLE "luaprof.runtime"
#define LP_WORK_GUARD_METATABLE "luaprof.work_guard"
#define LP_RECORDER_METATABLE "luaprof.recorder"
#define LP_RESULT_METATABLE "luaprof.result"
#define LP_DEFAULT_SAMPLE_BYTES (512u * 1024u)
#define LP_DEFAULT_SAMPLE_HZ 100u

typedef struct lp_runtime_holder {
	lp_runtime *runtime;
	lp_lua_bridge bridge;
} lp_runtime_holder;

typedef struct lp_lua_recorder {
	lp_runtime *runtime;
	lp_collector_kind kind;
	uint64_t generation;
	bool active;
} lp_lua_recorder;

typedef struct lp_lua_result {
	lp_result value;
} lp_lua_result;

typedef struct lp_lua_work_guard {
	lp_lua_bridge *bridge;
} lp_lua_work_guard;

#if defined(LUAPROF_TESTING)
void lp_lua_module_test_point(lua_State *L, const char *name);
#define module_test_point(L,name) lp_lua_module_test_point((L), (name))
#else
#define module_test_point(L,name) ((void)0)
#endif

static const char runtime_registry_key;

static const char *
kind_name(lp_collector_kind kind) {
	return kind == LP_COLLECTOR_CPU ? "cpu" : "memory";
}

static lp_runtime_holder *
runtime_holder(lua_State *L) {
	lua_rawgetp(L, LUA_REGISTRYINDEX, &runtime_registry_key);
	if (!lua_isnil(L, -1)) {
		return luaL_checkudata(L, -1, LP_RUNTIME_METATABLE);
	}
	lua_pop(L, 1);

	lp_runtime_holder *holder = lua_newuserdatauv(L, sizeof(*holder), 1);
	holder->runtime = NULL;
	luaL_setmetatable(L, LP_RUNTIME_METATABLE);
	lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
	lua_State *main_state = lua_tothread(L, -1);
	lua_pop(L, 1);
	lp_lua_bridge_init(&holder->bridge, main_state);
	holder->runtime = lp_runtime_new(main_state, lp_lua_bridge_host_ops(),
		&holder->bridge);
	if (holder->runtime == NULL) {
		luaL_error(L, "luaprof: out of memory");
		return NULL;
	}
	lp_lua_bridge_bind(&holder->bridge, holder->runtime);
	lp_lua_work_guard *guard = lua_newuserdatauv(L, sizeof(*guard), 0);
	guard->bridge = &holder->bridge;
	luaL_setmetatable(L, LP_WORK_GUARD_METATABLE);
	lua_setiuservalue(L, -2, 1);
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &runtime_registry_key);
	return holder;
}

static lp_runtime_holder *
closure_runtime_holder(lua_State *L) {
	return lua_touserdata(L, lua_upvalueindex(1));
}

static bool
push_profiler_work_guard(lua_State *L) {
	/* Both upvalues refer to objects preallocated while loading the module. */
	lp_lua_work_guard *guard = lua_touserdata(L, lua_upvalueindex(2));
	if (guard == NULL) {
		return false;
	}
	lua_pushvalue(L, lua_upvalueindex(2));
	if (!lp_lua_bridge_begin_profiler_work(guard->bridge)) {
		lua_pop(L, 1);
		return false;
	}
	lua_toclose(L, -1);
	return true;
}

static int
runtime_gc(lua_State *L) {
	lp_runtime_holder *holder = luaL_checkudata(L, 1,
		LP_RUNTIME_METATABLE);
	lp_runtime_delete(holder->runtime);
	holder->runtime = NULL;
	return 0;
}

static int
profiler_work_guard_close(lua_State *L) {
	lp_lua_work_guard *guard = lua_touserdata(L, 1);
	if (guard != NULL) {
		lp_lua_bridge_end_profiler_work(guard->bridge, true);
	}
	return 0;
}

static void
check_no_unknown_options(lua_State *L, int index, const char *first,
	const char *second) {
	lua_pushnil(L);
	while (lua_next(L, index) != 0) {
		size_t key_length = 0;
		const char *key = lua_type(L, -2) == LUA_TSTRING
			? lua_tolstring(L, -2, &key_length) : NULL;
		bool known = key != NULL &&
			((first != NULL && key_length == strlen(first) &&
				memcmp(key, first, key_length) == 0) ||
				(second != NULL && key_length == strlen(second) &&
					memcmp(key, second, key_length) == 0));
		lua_pop(L, 1);
		if (!known) {
			luaL_argerror(L, index, "unknown profile option");
		}
	}
}

static const char *
check_c_string(lua_State *L, int index, int argument, const char *name,
	size_t *length) {
	const char *value = luaL_checklstring(L, index, length);
	if (memchr(value, '\0', *length) != NULL) {
		luaL_argerror(L, argument, lua_pushfstring(L,
			"%s must not contain NUL bytes", name));
	}
	return value;
}

static lp_collector_config
cpu_config(lua_State *L, int argument_count) {
	lp_collector_config config = {
		.kind = LP_COLLECTOR_CPU,
		.value.cpu = { .sample_hz = LP_DEFAULT_SAMPLE_HZ },
	};
	if (argument_count == 0 || lua_isnil(L, 1)) {
		return config;
	}
	luaL_checktype(L, 1, LUA_TTABLE);
	check_no_unknown_options(L, 1, "sample_hz", NULL);
	lua_getfield(L, 1, "sample_hz");
	if (!lua_isnil(L, -1)) {
		if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 1 ||
			lua_tointeger(L, -1) > 10000) {
			luaL_argerror(L, 1,
				"sample_hz must be an integer between 1 and 10000");
		}
		config.value.cpu.sample_hz = (uint32_t)lua_tointeger(L, -1);
	}
	lua_pop(L, 1);
	return config;
}

static lp_collector_config
memory_config(lua_State *L, int argument_count) {
	lp_collector_config config = {
		.kind = LP_COLLECTOR_MEMORY,
		.value.memory = {
			.sample_bytes = LP_DEFAULT_SAMPLE_BYTES,
			.track_free = false,
		},
	};
	if (argument_count == 0 || lua_isnil(L, 1)) {
		return config;
	}

	luaL_checktype(L, 1, LUA_TTABLE);
	check_no_unknown_options(L, 1, "sample_bytes", "track_free");

	lua_getfield(L, 1, "sample_bytes");
	if (!lua_isnil(L, -1)) {
		if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) < 1) {
			luaL_argerror(L, 1, "sample_bytes must be a positive integer");
		}
		config.value.memory.sample_bytes =
			(uint64_t)lua_tointeger(L, -1);
	}
	lua_pop(L, 1);

	lua_getfield(L, 1, "track_free");
	if (!lua_isnil(L, -1)) {
		if (lua_type(L, -1) != LUA_TBOOLEAN) {
			luaL_argerror(L, 1, "track_free must be a boolean");
		}
		config.value.memory.track_free = lua_toboolean(L, -1);
	}
	lua_pop(L, 1);
	return config;
}

static int
start_recorder(lua_State *L, lp_collector_config config) {
	lp_runtime_holder *holder = closure_runtime_holder(L);
	lp_lua_recorder *recorder = lua_newuserdatauv(L, sizeof(*recorder), 2);
	recorder->runtime = holder->runtime;
	recorder->kind = config.kind;
	recorder->generation = 0;
	recorder->active = false;
	luaL_setmetatable(L, LP_RECORDER_METATABLE);
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_setiuservalue(L, -2, 1);
	lp_lua_result *result = lua_newuserdatauv(L, sizeof(*result), 0);
	memset(result, 0, sizeof(*result));
	luaL_setmetatable(L, LP_RESULT_METATABLE);
	lua_setiuservalue(L, -2, 2);

	lp_status status = lp_runtime_start(holder->runtime, L, &config,
		&recorder->generation);
	if (status != LP_OK) {
		lua_pushnil(L);
		lua_pushfstring(L, "luaprof %s start failed: %s",
			kind_name(config.kind), lp_status_string(status));
		return 2;
	}
	recorder->active = true;
	return 1;
}

static int
cpu_start(lua_State *L) {
	int argument_count = lua_gettop(L);
	(void)push_profiler_work_guard(L);
	lp_collector_config config = cpu_config(L, argument_count);
	return start_recorder(L, config);
}

static int
memory_start(lua_State *L) {
	int argument_count = lua_gettop(L);
	(void)push_profiler_work_guard(L);
	lp_collector_config config = memory_config(L, argument_count);
	module_test_point(L, "memory_start");
	return start_recorder(L, config);
}

static int
recorder_stop(lua_State *L) {
	(void)push_profiler_work_guard(L);
	lp_lua_recorder *recorder = luaL_checkudata(L, 1,
		LP_RECORDER_METATABLE);
	if (!recorder->active) {
		lua_pushnil(L);
		lua_pushliteral(L, "luaprof recorder is already stopped");
		return 2;
	}
	if (recorder->kind == LP_COLLECTOR_CPU) {
		module_test_point(L, "cpu_stop");
	}
	else {
		module_test_point(L, "memory_stop");
	}

	lua_getiuservalue(L, 1, 2);
	lp_lua_result *result = luaL_checkudata(L, -1, LP_RESULT_METATABLE);
	lp_status status = lp_runtime_stop(recorder->runtime, L, recorder->kind,
		recorder->generation, &result->value);
	if (status != LP_OK) {
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushfstring(L, "luaprof %s stop failed: %s",
			kind_name(recorder->kind), lp_status_string(status));
		return 2;
	}
	recorder->active = false;
	lua_pushnil(L);
	lua_setiuservalue(L, 1, 2);
	return 1;
}

static int
recorder_gc(lua_State *L) {
	(void)push_profiler_work_guard(L);
	lp_lua_recorder *recorder = luaL_checkudata(L, 1,
		LP_RECORDER_METATABLE);
	if (recorder->active) {
		lp_result ignored = { 0 };
		(void)lp_runtime_stop(recorder->runtime, L, recorder->kind,
			recorder->generation, &ignored);
		lp_result_dispose(&ignored);
		recorder->active = false;
	}
	return 0;
}

static int
result_gc(lua_State *L) {
	(void)push_profiler_work_guard(L);
	lp_lua_result *result = luaL_checkudata(L, 1, LP_RESULT_METATABLE);
	lp_result_dispose(&result->value);
	return 0;
}

static int
recorder_tostring(lua_State *L) {
	(void)push_profiler_work_guard(L);
	lp_lua_recorder *recorder = luaL_checkudata(L, 1,
		LP_RECORDER_METATABLE);
	lua_pushfstring(L, "luaprof.%s.recorder(%s, generation=%I)",
		kind_name(recorder->kind), recorder->active ? "active" : "stopped",
		(lua_Integer)recorder->generation);
	return 1;
}

static int
result_stats(lua_State *L) {
	(void)push_profiler_work_guard(L);
	lp_lua_result *result = luaL_checkudata(L, 1, LP_RESULT_METATABLE);
	module_test_point(L, "result_stats");
	lua_createtable(L, 0, 7);
	lua_pushstring(L, kind_name(result->value.kind));
	lua_setfield(L, -2, "kind");
	lua_pushinteger(L, (lua_Integer)result->value.generation);
	lua_setfield(L, -2, "generation");
	lua_pushinteger(L, (lua_Integer)result->value.stats.samples);
	lua_setfield(L, -2, "samples");
	lua_pushboolean(L, false);
	lua_setfield(L, -2, "active");
	lua_pushinteger(L, (lua_Integer)result->value.stats.safe_points);
	lua_setfield(L, -2, "safe_points");
	lua_pushinteger(L, (lua_Integer)result->value.stats.pending_weight);
	lua_setfield(L, -2, "pending_weight");
	lua_pushinteger(L, (lua_Integer)result->value.stats.state_host);
	lua_setfield(L, -2, "state_host");
	lua_pushinteger(L, (lua_Integer)result->value.stats.state_lua);
	lua_setfield(L, -2, "state_lua");
	lua_pushinteger(L, (lua_Integer)result->value.stats.state_c);
	lua_setfield(L, -2, "state_c");
	lua_pushinteger(L, (lua_Integer)result->value.stats.state_gc);
	lua_setfield(L, -2, "state_gc");
	if (result->value.kind == LP_COLLECTOR_CPU) {
		lua_pushinteger(L,
			(lua_Integer)result->value.config.value.cpu.sample_hz);
		lua_setfield(L, -2, "sample_hz");
		lua_pushinteger(L, (lua_Integer)result->value.stats.sample_host);
		lua_setfield(L, -2, "sample_host");
		lua_pushinteger(L, (lua_Integer)result->value.stats.sample_lua);
		lua_setfield(L, -2, "sample_lua");
		lua_pushinteger(L, (lua_Integer)result->value.stats.sample_c);
		lua_setfield(L, -2, "sample_c");
		lua_pushinteger(L, (lua_Integer)result->value.stats.sample_gc);
		lua_setfield(L, -2, "sample_gc");
		lua_pushinteger(L,
			(lua_Integer)result->value.stats.overrun_events);
		lua_setfield(L, -2, "overrun_events");
		lua_pushinteger(L, (lua_Integer)result->value.stats.overrun_ticks);
		lua_setfield(L, -2, "overrun_ticks");
		lua_pushinteger(L, (lua_Integer)result->value.stats.dropped_events);
		lua_setfield(L, -2, "dropped_events");
		lua_pushinteger(L, (lua_Integer)result->value.stats.unstable_events);
		lua_setfield(L, -2, "unstable_events");
		lua_pushinteger(L,
			(lua_Integer)result->value.stats.profiler_overhead_events);
		lua_setfield(L, -2, "profiler_overhead_events");
		lua_pushinteger(L,
			(lua_Integer)result->value.stats.stale_events);
		lua_setfield(L, -2, "stale_events");
		lua_pushinteger(L,
			(lua_Integer)result->value.stats.timer_failures);
		lua_setfield(L, -2, "timer_failures");
		lua_pushinteger(L,
			(lua_Integer)result->value.stats.scheduler_workers);
		lua_setfield(L, -2, "scheduler_workers");
		lua_pushinteger(L,
			(lua_Integer)result->value.stats.stack_truncations);
		lua_setfield(L, -2, "stack_truncations");
		lua_pushinteger(L,
			(lua_Integer)result->value.stats.aggregate_overflows);
		lua_setfield(L, -2, "aggregate_overflows");
		lua_pushinteger(L,
			(lua_Integer)result->value.stats.symbol_overflows);
		lua_setfield(L, -2, "symbol_overflows");
	}
	if (result->value.kind == LP_COLLECTOR_MEMORY) {
		lua_pushinteger(L,
			(lua_Integer)result->value.config.value.memory.sample_bytes);
		lua_setfield(L, -2, "sample_bytes");
		lua_pushboolean(L,
			result->value.config.value.memory.track_free);
		lua_setfield(L, -2, "track_free");
		lua_pushinteger(L, (lua_Integer)result->value.stats.allocations);
		lua_setfield(L, -2, "allocation_events");
		lua_pushinteger(L, (lua_Integer)result->value.stats.reallocations);
		lua_setfield(L, -2, "reallocation_events");
		lua_pushinteger(L, (lua_Integer)result->value.stats.frees);
		lua_setfield(L, -2, "free_events");
		lua_pushinteger(L,
			(lua_Integer)result->value.stats.allocation_failures);
		lua_setfield(L, -2, "allocation_failures");
		lua_pushinteger(L,
			(lua_Integer)result->value.stats.memory_samples);
		lua_setfield(L, -2, "samples");
		lua_pushinteger(L,
			(lua_Integer)result->value.stats.sampled_alloc_bytes);
		lua_setfield(L, -2, "sampled_alloc_bytes");
		lua_pushinteger(L, (lua_Integer)result->value.stats.alloc_space);
		lua_setfield(L, -2, "alloc_space");
		lua_pushinteger(L, (lua_Integer)result->value.stats.alloc_objects);
		lua_setfield(L, -2, "alloc_objects");
		lua_pushinteger(L, (lua_Integer)result->value.stats.inuse_space);
		lua_setfield(L, -2, "inuse_space");
		lua_pushinteger(L, (lua_Integer)result->value.stats.inuse_objects);
		lua_setfield(L, -2, "inuse_objects");
		lua_pushinteger(L,
			(lua_Integer)result->value.stats.live_map_overflows);
		lua_setfield(L, -2, "live_map_overflows");
		lua_pushinteger(L,
			(lua_Integer)result->value.stats.stack_truncations);
		lua_setfield(L, -2, "stack_truncations");
		lua_pushinteger(L,
			(lua_Integer)result->value.stats.aggregate_overflows);
		lua_setfield(L, -2, "aggregate_overflows");
		lua_pushinteger(L,
			(lua_Integer)result->value.stats.symbol_overflows);
		lua_setfield(L, -2, "symbol_overflows");
	}
	return 1;
}

static int
result_write(lua_State *L) {
	int argument_count = lua_gettop(L);
	(void)push_profiler_work_guard(L);
	lp_lua_result *result = luaL_checkudata(L, 1, LP_RESULT_METATABLE);
	if (argument_count < 2) {
		return luaL_argerror(L, 2, "path expected");
	}
	size_t path_length;
	const char *path = check_c_string(L, 2, 2, "path", &path_length);
	lp_export_format format = LP_EXPORT_PPROF;
	const char *sample_type = NULL;
	if (argument_count >= 3 && !lua_isnil(L, 3)) {
		luaL_checktype(L, 3, LUA_TTABLE);
		check_no_unknown_options(L, 3, "format", "sample");
		lua_getfield(L, 3, "format");
		if (!lua_isnil(L, -1)) {
			size_t name_length;
			const char *name = check_c_string(L, -1, 3, "format",
				&name_length);
			if (strcmp(name, "pprof") == 0) {
				format = LP_EXPORT_PPROF;
			}
			else if (strcmp(name, "folded") == 0) {
				format = LP_EXPORT_FOLDED;
			}
			else {
				return luaL_argerror(L, 3,
					"format must be 'pprof' or 'folded'");
			}
		}
		lua_pop(L, 1);
		lua_getfield(L, 3, "sample");
		if (!lua_isnil(L, -1)) {
			size_t sample_length;
			sample_type = check_c_string(L, -1, 3, "sample",
				&sample_length);
		}
		lua_pop(L, 1);
	}
	module_test_point(L, "result_write");
	char error[256];
	lp_lua_symbols *lua_symbols = lp_lua_symbols_collect(L);
	lp_export_symbols symbols = {
		.userdata = lua_symbols,
		.cfunction_name = lp_lua_symbols_lookup,
	};
	bool success = lp_export_result_with_symbols(&result->value, path, format,
		sample_type, lua_symbols == NULL ? NULL : &symbols, error,
		sizeof(error));
	lp_lua_symbols_delete(lua_symbols);
	if (!success) {
		lua_pushnil(L);
		lua_pushstring(L, error);
		return 2;
	}
	lua_pushboolean(L, true);
	return 1;
}

static int
result_tostring(lua_State *L) {
	(void)push_profiler_work_guard(L);
	lp_lua_result *result = luaL_checkudata(L, 1, LP_RESULT_METATABLE);
	lua_pushfstring(L, "luaprof.%s.result(generation=%I)",
		kind_name(result->value.kind), (lua_Integer)result->value.generation);
	return 1;
}

static void
create_runtime_metatables(lua_State *L) {
	if (luaL_newmetatable(L, LP_RUNTIME_METATABLE)) {
		lua_pushcfunction(L, runtime_gc);
		lua_setfield(L, -2, "__gc");
	}
	lua_pop(L, 1);

	if (luaL_newmetatable(L, LP_WORK_GUARD_METATABLE)) {
		lua_pushcfunction(L, profiler_work_guard_close);
		lua_setfield(L, -2, "__close");
	}
	lua_pop(L, 1);
}

static void
push_runtime_closure(lua_State *L, int holder_index, int guard_index,
	lua_CFunction function) {
	lua_pushvalue(L, holder_index);
	lua_pushvalue(L, guard_index);
	lua_pushcclosure(L, function, 2);
}

static void
create_value_metatables(lua_State *L, int holder_index, int guard_index) {
	holder_index = lua_absindex(L, holder_index);
	guard_index = lua_absindex(L, guard_index);

	if (luaL_newmetatable(L, LP_RECORDER_METATABLE)) {
		static const luaL_Reg methods[] = {
			{ "stop", recorder_stop },
			{ NULL, NULL },
		};
		lua_pushvalue(L, holder_index);
		lua_pushvalue(L, guard_index);
		luaL_setfuncs(L, methods, 2);
		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "__index");
		push_runtime_closure(L, holder_index, guard_index, recorder_gc);
		lua_setfield(L, -2, "__gc");
		push_runtime_closure(L, holder_index, guard_index, recorder_gc);
		lua_setfield(L, -2, "__close");
		push_runtime_closure(L, holder_index, guard_index,
			recorder_tostring);
		lua_setfield(L, -2, "__tostring");
	}
	lua_pop(L, 1);

	if (luaL_newmetatable(L, LP_RESULT_METATABLE)) {
		static const luaL_Reg methods[] = {
			{ "stats", result_stats },
			{ "write", result_write },
			{ NULL, NULL },
		};
		lua_pushvalue(L, holder_index);
		lua_pushvalue(L, guard_index);
		luaL_setfuncs(L, methods, 2);
		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "__index");
		push_runtime_closure(L, holder_index, guard_index, result_gc);
		lua_setfield(L, -2, "__gc");
		push_runtime_closure(L, holder_index, guard_index,
			result_tostring);
		lua_setfield(L, -2, "__tostring");
	}
	lua_pop(L, 1);
}

LUAMOD_API int
luaopen_luaprof(lua_State *L) {
	create_runtime_metatables(L);
	(void)runtime_holder(L);
	int holder_index = lua_absindex(L, -1);
	lua_getiuservalue(L, holder_index, 1);
	int guard_index = lua_absindex(L, -1);
	create_value_metatables(L, holder_index, guard_index);

	lua_createtable(L, 0, 3);
	lua_pushliteral(L, "0.1.0");
	lua_setfield(L, -2, "_VERSION");

	lua_createtable(L, 0, 1);
	push_runtime_closure(L, holder_index, guard_index, cpu_start);
	lua_setfield(L, -2, "start");
	lua_setfield(L, -2, "cpu");

	lua_createtable(L, 0, 1);
	push_runtime_closure(L, holder_index, guard_index, memory_start);
	lua_setfield(L, -2, "start");
	lua_setfield(L, -2, "memory");
	lua_remove(L, guard_index);
	lua_remove(L, holder_index);
	return 1;
}
