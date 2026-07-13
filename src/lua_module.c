#include "luaprof/runtime.h"

#include <stdint.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>

#define LP_RUNTIME_METATABLE "luaprof.runtime"
#define LP_RECORDER_METATABLE "luaprof.recorder"
#define LP_RESULT_METATABLE "luaprof.result"
#define LP_DEFAULT_SAMPLE_BYTES (512u * 1024u)

typedef struct lp_runtime_holder {
	lp_runtime *runtime;
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
	holder->runtime = lp_runtime_new(main_state, NULL, NULL);
	if (holder->runtime == NULL) {
		luaL_error(L, "luaprof: out of memory");
		return NULL;
	}
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
		const char *key = lua_tostring(L, -2);
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
		.value.cpu = { .reserved = 0 },
	};
	if (lua_isnoneornil(L, 1)) {
		return config;
	}
	luaL_checktype(L, 1, LUA_TTABLE);
	check_no_unknown_options(L, 1, NULL, NULL);
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

	lp_status status = lp_runtime_start(holder->runtime, &config,
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

	lp_result_meta meta;
	lp_status status = lp_runtime_stop(recorder->runtime, recorder->kind,
		recorder->generation, &meta);
	if (status != LP_OK) {
		lua_pushnil(L);
		lua_pushfstring(L, "luaprof %s stop failed: %s",
			kind_name(recorder->kind), lp_status_string(status));
		return 2;
	}
	recorder->active = false;

	lp_lua_result *result = lua_newuserdatauv(L, sizeof(*result), 0);
	result->meta = meta;
	luaL_setmetatable(L, LP_RESULT_METATABLE);
	return 1;
}

static int
recorder_gc(lua_State *L) {
	lp_lua_recorder *recorder = luaL_checkudata(L, 1,
		LP_RECORDER_METATABLE);
	if (recorder->active) {
		lp_result_meta ignored;
		(void)lp_runtime_stop(recorder->runtime, recorder->kind,
			recorder->generation, &ignored);
		recorder->active = false;
	}
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
	lua_pushinteger(L, 0);
	lua_setfield(L, -2, "samples");
	lua_pushboolean(L, false);
	lua_setfield(L, -2, "active");
	if (result->meta.kind == LP_COLLECTOR_MEMORY) {
		lua_pushinteger(L,
			(lua_Integer)result->meta.config.value.memory.sample_bytes);
		lua_setfield(L, -2, "sample_bytes");
		lua_pushboolean(L,
			result->meta.config.value.memory.track_free);
		lua_setfield(L, -2, "track_free");
	}
	return 1;
}

static int
result_write(lua_State *L) {
	(void)luaL_checkudata(L, 1, LP_RESULT_METATABLE);
	(void)luaL_checkstring(L, 2);
	lua_pushnil(L);
	lua_pushliteral(L, "luaprof pprof exporter is not implemented yet");
	return 2;
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
	lua_pushliteral(L, "0.1.0-dev");
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
