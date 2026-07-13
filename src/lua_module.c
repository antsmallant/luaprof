#include "luaprof/runtime.h"
#include "lua_bridge.h"
#include "pprof_exporter.h"

#include <stdint.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>

#define LP_RUNTIME_METATABLE "luaprof.runtime"
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
	lp_result_meta meta;
} lp_lua_result;

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

	lp_runtime_holder *holder = lua_newuserdatauv(L, sizeof(*holder), 0);
	holder->runtime = NULL;
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
	luaL_setmetatable(L, LP_RUNTIME_METATABLE);
	lua_pushvalue(L, -1);
	lua_rawsetp(L, LUA_REGISTRYINDEX, &runtime_registry_key);
	return holder;
}

static int
runtime_gc(lua_State *L) {
	lp_runtime_holder *holder = luaL_checkudata(L, 1,
		LP_RUNTIME_METATABLE);
	lp_runtime_delete(holder->runtime);
	holder->runtime = NULL;
	return 0;
}

static void
check_no_unknown_options(lua_State *L, int index, const char *first,
	const char *second) {
	lua_pushnil(L);
	while (lua_next(L, index) != 0) {
		const char *key = lua_type(L, -2) == LUA_TSTRING
			? lua_tostring(L, -2) : NULL;
		bool known = key != NULL &&
			((first != NULL && strcmp(key, first) == 0) ||
				(second != NULL && strcmp(key, second) == 0));
		lua_pop(L, 1);
		if (!known) {
			luaL_argerror(L, index, "unknown profile option");
		}
	}
}

static lp_collector_config
cpu_config(lua_State *L) {
	lp_collector_config config = {
		.kind = LP_COLLECTOR_CPU,
		.value.cpu = { .sample_hz = LP_DEFAULT_SAMPLE_HZ },
	};
	if (lua_isnoneornil(L, 1)) {
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
memory_config(lua_State *L) {
	lp_collector_config config = {
		.kind = LP_COLLECTOR_MEMORY,
		.value.memory = {
			.sample_bytes = LP_DEFAULT_SAMPLE_BYTES,
			.track_free = false,
		},
	};
	if (lua_isnoneornil(L, 1)) {
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
	lp_runtime_holder *holder = runtime_holder(L);
	lp_lua_recorder *recorder = lua_newuserdatauv(L, sizeof(*recorder), 1);
	recorder->runtime = holder->runtime;
	recorder->kind = config.kind;
	recorder->generation = 0;
	recorder->active = false;
	luaL_setmetatable(L, LP_RECORDER_METATABLE);
	lua_pushvalue(L, -2);
	lua_setiuservalue(L, -2, 1);

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
	return start_recorder(L, cpu_config(L));
}

static int
memory_start(lua_State *L) {
	return start_recorder(L, memory_config(L));
}

static int
recorder_stop(lua_State *L) {
	lp_lua_recorder *recorder = luaL_checkudata(L, 1,
		LP_RECORDER_METATABLE);
	if (!recorder->active) {
		lua_pushnil(L);
		lua_pushliteral(L, "luaprof recorder is already stopped");
		return 2;
	}

	lp_lua_result *result = lua_newuserdatauv(L, sizeof(*result), 0);
	memset(result, 0, sizeof(*result));
	lp_status status = lp_runtime_stop(recorder->runtime, L, recorder->kind,
		recorder->generation, &result->meta);
	if (status != LP_OK) {
		lua_pop(L, 1);
		lua_pushnil(L);
		lua_pushfstring(L, "luaprof %s stop failed: %s",
			kind_name(recorder->kind), lp_status_string(status));
		return 2;
	}
	recorder->active = false;
	luaL_setmetatable(L, LP_RESULT_METATABLE);
	return 1;
}

static int
recorder_gc(lua_State *L) {
	lp_lua_recorder *recorder = luaL_checkudata(L, 1,
		LP_RECORDER_METATABLE);
	if (recorder->active) {
		lp_result_meta ignored = { 0 };
		(void)lp_runtime_stop(recorder->runtime, L, recorder->kind,
			recorder->generation, &ignored);
		lp_result_meta_dispose(&ignored);
		recorder->active = false;
	}
	return 0;
}

static int
result_gc(lua_State *L) {
	lp_lua_result *result = luaL_checkudata(L, 1, LP_RESULT_METATABLE);
	lp_result_meta_dispose(&result->meta);
	return 0;
}

static int
recorder_tostring(lua_State *L) {
	lp_lua_recorder *recorder = luaL_checkudata(L, 1,
		LP_RECORDER_METATABLE);
	lua_pushfstring(L, "luaprof.%s.recorder(%s, generation=%I)",
		kind_name(recorder->kind), recorder->active ? "active" : "stopped",
		(lua_Integer)recorder->generation);
	return 1;
}

static int
result_stats(lua_State *L) {
	lp_lua_result *result = luaL_checkudata(L, 1, LP_RESULT_METATABLE);
	lua_createtable(L, 0, 7);
	lua_pushstring(L, kind_name(result->meta.kind));
	lua_setfield(L, -2, "kind");
	lua_pushinteger(L, (lua_Integer)result->meta.generation);
	lua_setfield(L, -2, "generation");
	lua_pushinteger(L, (lua_Integer)result->meta.stats.sample_weight);
	lua_setfield(L, -2, "samples");
	lua_pushboolean(L, false);
	lua_setfield(L, -2, "active");
	lua_pushinteger(L, (lua_Integer)result->meta.stats.safe_points);
	lua_setfield(L, -2, "safe_points");
	lua_pushinteger(L, (lua_Integer)result->meta.stats.pending_weight);
	lua_setfield(L, -2, "pending_weight");
	lua_pushinteger(L, (lua_Integer)result->meta.stats.state_host);
	lua_setfield(L, -2, "state_host");
	lua_pushinteger(L, (lua_Integer)result->meta.stats.state_lua);
	lua_setfield(L, -2, "state_lua");
	lua_pushinteger(L, (lua_Integer)result->meta.stats.state_c);
	lua_setfield(L, -2, "state_c");
	lua_pushinteger(L, (lua_Integer)result->meta.stats.state_gc);
	lua_setfield(L, -2, "state_gc");
	if (result->meta.kind == LP_COLLECTOR_CPU) {
		lua_pushinteger(L,
			(lua_Integer)result->meta.config.value.cpu.sample_hz);
		lua_setfield(L, -2, "sample_hz");
		lua_pushinteger(L, (lua_Integer)result->meta.stats.sample_host);
		lua_setfield(L, -2, "sample_host");
		lua_pushinteger(L, (lua_Integer)result->meta.stats.sample_lua);
		lua_setfield(L, -2, "sample_lua");
		lua_pushinteger(L, (lua_Integer)result->meta.stats.sample_c);
		lua_setfield(L, -2, "sample_c");
		lua_pushinteger(L, (lua_Integer)result->meta.stats.sample_gc);
		lua_setfield(L, -2, "sample_gc");
		lua_pushinteger(L, (lua_Integer)result->meta.stats.dropped_events);
		lua_setfield(L, -2, "dropped_events");
		lua_pushinteger(L, (lua_Integer)result->meta.stats.unstable_events);
		lua_setfield(L, -2, "unstable_events");
		lua_pushinteger(L,
			(lua_Integer)result->meta.stats.profiler_overhead_events);
		lua_setfield(L, -2, "profiler_overhead_events");
		lua_pushinteger(L,
			(lua_Integer)result->meta.stats.stale_events);
		lua_setfield(L, -2, "stale_events");
		lua_pushinteger(L,
			(lua_Integer)result->meta.stats.scheduler_workers);
		lua_setfield(L, -2, "scheduler_workers");
		lua_pushinteger(L,
			(lua_Integer)result->meta.stats.stack_truncations);
		lua_setfield(L, -2, "stack_truncations");
		lua_pushinteger(L,
			(lua_Integer)result->meta.stats.aggregate_overflows);
		lua_setfield(L, -2, "aggregate_overflows");
		lua_pushinteger(L,
			(lua_Integer)result->meta.stats.symbol_overflows);
		lua_setfield(L, -2, "symbol_overflows");
	}
	if (result->meta.kind == LP_COLLECTOR_MEMORY) {
		lua_pushinteger(L,
			(lua_Integer)result->meta.config.value.memory.sample_bytes);
		lua_setfield(L, -2, "sample_bytes");
		lua_pushboolean(L,
			result->meta.config.value.memory.track_free);
		lua_setfield(L, -2, "track_free");
		lua_pushinteger(L, (lua_Integer)result->meta.stats.allocations);
		lua_setfield(L, -2, "allocation_events");
		lua_pushinteger(L, (lua_Integer)result->meta.stats.reallocations);
		lua_setfield(L, -2, "reallocation_events");
		lua_pushinteger(L, (lua_Integer)result->meta.stats.frees);
		lua_setfield(L, -2, "free_events");
		lua_pushinteger(L,
			(lua_Integer)result->meta.stats.allocation_failures);
		lua_setfield(L, -2, "allocation_failures");
		lua_pushinteger(L,
			(lua_Integer)result->meta.stats.memory_samples);
		lua_setfield(L, -2, "samples");
		lua_pushinteger(L,
			(lua_Integer)result->meta.stats.sampled_alloc_bytes);
		lua_setfield(L, -2, "sampled_alloc_bytes");
		lua_pushinteger(L, (lua_Integer)result->meta.stats.alloc_space);
		lua_setfield(L, -2, "alloc_space");
		lua_pushinteger(L, (lua_Integer)result->meta.stats.alloc_objects);
		lua_setfield(L, -2, "alloc_objects");
		lua_pushinteger(L, (lua_Integer)result->meta.stats.inuse_space);
		lua_setfield(L, -2, "inuse_space");
		lua_pushinteger(L, (lua_Integer)result->meta.stats.inuse_objects);
		lua_setfield(L, -2, "inuse_objects");
		lua_pushinteger(L,
			(lua_Integer)result->meta.stats.live_map_overflows);
		lua_setfield(L, -2, "live_map_overflows");
		lua_pushinteger(L,
			(lua_Integer)result->meta.stats.stack_truncations);
		lua_setfield(L, -2, "stack_truncations");
		lua_pushinteger(L,
			(lua_Integer)result->meta.stats.aggregate_overflows);
		lua_setfield(L, -2, "aggregate_overflows");
		lua_pushinteger(L,
			(lua_Integer)result->meta.stats.symbol_overflows);
		lua_setfield(L, -2, "symbol_overflows");
	}
	return 1;
}

static int
result_write(lua_State *L) {
	lp_lua_result *result = luaL_checkudata(L, 1, LP_RESULT_METATABLE);
	const char *path = luaL_checkstring(L, 2);
	lp_export_format format = LP_EXPORT_PPROF;
	const char *sample_type = NULL;
	if (!lua_isnoneornil(L, 3)) {
		luaL_checktype(L, 3, LUA_TTABLE);
		check_no_unknown_options(L, 3, "format", "sample");
		lua_getfield(L, 3, "format");
		if (!lua_isnil(L, -1)) {
			const char *name = luaL_checkstring(L, -1);
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
			sample_type = luaL_checkstring(L, -1);
		}
		lua_pop(L, 1);
	}
	char error[256];
	if (!lp_export_result(&result->meta, path, format, sample_type, error,
		sizeof(error))) {
		lua_pushnil(L);
		lua_pushstring(L, error);
		return 2;
	}
	lua_pushboolean(L, true);
	return 1;
}

static int
result_tostring(lua_State *L) {
	lp_lua_result *result = luaL_checkudata(L, 1, LP_RESULT_METATABLE);
	lua_pushfstring(L, "luaprof.%s.result(generation=%I)",
		kind_name(result->meta.kind), (lua_Integer)result->meta.generation);
	return 1;
}

static void
create_metatables(lua_State *L) {
	if (luaL_newmetatable(L, LP_RUNTIME_METATABLE)) {
		lua_pushcfunction(L, runtime_gc);
		lua_setfield(L, -2, "__gc");
	}
	lua_pop(L, 1);

	if (luaL_newmetatable(L, LP_RECORDER_METATABLE)) {
		static const luaL_Reg methods[] = {
			{ "stop", recorder_stop },
			{ NULL, NULL },
		};
		luaL_setfuncs(L, methods, 0);
		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, recorder_gc);
		lua_setfield(L, -2, "__gc");
		lua_pushcfunction(L, recorder_gc);
		lua_setfield(L, -2, "__close");
		lua_pushcfunction(L, recorder_tostring);
		lua_setfield(L, -2, "__tostring");
	}
	lua_pop(L, 1);

	if (luaL_newmetatable(L, LP_RESULT_METATABLE)) {
		static const luaL_Reg methods[] = {
			{ "stats", result_stats },
			{ "write", result_write },
			{ NULL, NULL },
		};
		luaL_setfuncs(L, methods, 0);
		lua_pushvalue(L, -1);
		lua_setfield(L, -2, "__index");
		lua_pushcfunction(L, result_gc);
		lua_setfield(L, -2, "__gc");
		lua_pushcfunction(L, result_tostring);
		lua_setfield(L, -2, "__tostring");
	}
	lua_pop(L, 1);
}

LUAMOD_API int
luaopen_luaprof(lua_State *L) {
	create_metatables(L);
	(void)runtime_holder(L);
	lua_pop(L, 1);

	lua_createtable(L, 0, 3);
	lua_pushliteral(L, "0.1.0");
	lua_setfield(L, -2, "_VERSION");

	lua_createtable(L, 0, 1);
	lua_pushcfunction(L, cpu_start);
	lua_setfield(L, -2, "start");
	lua_setfield(L, -2, "cpu");

	lua_createtable(L, 0, 1);
	lua_pushcfunction(L, memory_start);
	lua_setfield(L, -2, "start");
	lua_setfield(L, -2, "memory");
	return 1;
}
