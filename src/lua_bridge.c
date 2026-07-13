#include "lua_bridge.h"

#include <string.h>

#include <lua.h>

_Static_assert(LUA_PROFILE_HOST == LP_VM_HOST, "profile state mismatch");
_Static_assert(LUA_PROFILE_LUA == LP_VM_LUA, "profile state mismatch");
_Static_assert(LUA_PROFILE_C == LP_VM_C, "profile state mismatch");
_Static_assert(LUA_PROFILE_GC == LP_VM_GC, "profile state mismatch");

static void
safe_point(void *userdata, lua_State *L, unsigned int pending) {
	lp_lua_bridge *bridge = userdata;
	lp_runtime_safe_point(bridge->runtime, bridge->cpu_generation, L,
		pending);
}

static void
state_change(void *userdata, lua_State *L, int state,
	lua_CFunction cfunction) {
	lp_lua_bridge *bridge = userdata;
	lp_runtime_state_change(bridge->runtime, bridge->cpu_generation, L,
		(lp_vm_state)state, cfunction);
}

static void
allocation(void *userdata, lua_State *L,
	const lua_ProfileAllocEvent *event) {
	lp_lua_bridge *bridge = userdata;
	lp_runtime_allocation(bridge->runtime, bridge->memory_generation, L,
		event->old_pointer, event->new_pointer, event->old_size,
		event->new_size, event->success != 0);
}

static void
apply_hooks(lp_lua_bridge *bridge, lua_State *current_state) {
	lua_ProfileHooks hooks = { 0 };
	if (bridge->cpu_active) {
		hooks.safe_point = safe_point;
		hooks.state_change = state_change;
	}
	if (bridge->memory_active) {
		hooks.allocation = allocation;
	}
	if (!bridge->cpu_active && !bridge->memory_active) {
		lua_setprofilehooks(current_state, NULL, NULL);
	}
	else {
		lua_setprofilehooks(current_state, &hooks, bridge);
	}
}

static lp_status
start_collector(void *userdata, lp_runtime *runtime,
	lua_State *current_state, uint64_t generation,
	const lp_collector_config *config) {
	lp_lua_bridge *bridge = userdata;
	(void)runtime;
	if (config->kind == LP_COLLECTOR_CPU) {
		bridge->cpu_active = true;
		bridge->cpu_generation = generation;
	}
	else {
		bridge->memory_active = true;
		bridge->memory_generation = generation;
	}
	apply_hooks(bridge, current_state);
	return LP_OK;
}

static void
stop_collector(void *userdata, lp_runtime *runtime,
	lua_State *current_state, lp_collector_kind kind, uint64_t generation) {
	lp_lua_bridge *bridge = userdata;
	(void)runtime;
	if (kind == LP_COLLECTOR_CPU && bridge->cpu_generation == generation) {
		bridge->cpu_active = false;
		bridge->cpu_generation = 0;
	}
	if (kind == LP_COLLECTOR_MEMORY &&
		bridge->memory_generation == generation) {
		bridge->memory_active = false;
		bridge->memory_generation = 0;
	}
	apply_hooks(bridge, current_state);
}

void
lp_lua_bridge_init(lp_lua_bridge *bridge, lua_State *main_state) {
	memset(bridge, 0, sizeof(*bridge));
	bridge->main_state = main_state;
}

void
lp_lua_bridge_bind(lp_lua_bridge *bridge, lp_runtime *runtime) {
	bridge->runtime = runtime;
}

const lp_host_ops *
lp_lua_bridge_host_ops(void) {
	static const lp_host_ops operations = {
		.start_collector = start_collector,
		.stop_collector = stop_collector,
	};
	return &operations;
}
