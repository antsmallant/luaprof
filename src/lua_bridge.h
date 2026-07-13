#ifndef LUAPROF_LUA_BRIDGE_H
#define LUAPROF_LUA_BRIDGE_H

#include "luaprof/runtime.h"

typedef struct lp_lua_bridge {
	lp_runtime *runtime;
	lua_State *main_state;
	uint64_t cpu_generation;
	uint64_t memory_generation;
	bool cpu_active;
	bool memory_active;
} lp_lua_bridge;

void lp_lua_bridge_init(lp_lua_bridge *bridge, lua_State *main_state);
void lp_lua_bridge_bind(lp_lua_bridge *bridge, lp_runtime *runtime);
const lp_host_ops *lp_lua_bridge_host_ops(void);

#endif
