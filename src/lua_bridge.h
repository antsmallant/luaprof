#ifndef LUAPROF_LUA_BRIDGE_H
#define LUAPROF_LUA_BRIDGE_H

#include "luaprof/runtime.h"

typedef struct lp_lua_bridge {
	lp_runtime *runtime;
	struct lp_thread_timer *cpu_timer;
	const struct lp_skynet_host_api *scheduler_api;
	lua_State *main_state;
	uint64_t cpu_generation;
	uint64_t scheduler_token;
	uint64_t memory_generation;
	bool cpu_active;
	bool memory_active;
} lp_lua_bridge;

void lp_lua_bridge_init(lp_lua_bridge *bridge, lua_State *main_state);
void lp_lua_bridge_bind(lp_lua_bridge *bridge, lp_runtime *runtime);
const lp_host_ops *lp_lua_bridge_host_ops(void);

#endif
