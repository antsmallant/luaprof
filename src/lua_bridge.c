#include "lua_bridge.h"
#include "thread_timer.h"

#include <string.h>

#include <lua.h>

_Static_assert(LUA_PROFILE_HOST == LP_VM_HOST, "profile state mismatch");
_Static_assert(LUA_PROFILE_LUA == LP_VM_LUA, "profile state mismatch");
_Static_assert(LUA_PROFILE_C == LP_VM_C, "profile state mismatch");
_Static_assert(LUA_PROFILE_GC == LP_VM_GC, "profile state mismatch");
_Static_assert(LUA_PROFILE_FRAME_LUA == LP_FRAME_LUA,
	"profile frame mismatch");
_Static_assert(LUA_PROFILE_FRAME_C == LP_FRAME_C,
	"profile frame mismatch");

#define LP_CAPTURE_STACK_DEPTH 64u

static void
record_timer_quality(lp_lua_bridge *bridge) {
	uint64_t dropped = 0;
	uint64_t unstable = 0;
	uint64_t profiler_overhead = 0;
	lp_thread_timer_take_quality(bridge->cpu_timer, &dropped, &unstable,
		&profiler_overhead);
	lp_runtime_cpu_quality(bridge->runtime, bridge->cpu_generation, dropped,
		unstable, profiler_overhead);
}

static void
drain_timer(lp_lua_bridge *bridge) {
	lp_tick_event event;
	while (lp_thread_timer_next(bridge->cpu_timer, &event)) {
		lua_ProfileFrame captured[LP_CAPTURE_STACK_DEPTH];
		lp_stack_frame frames[LP_CAPTURE_STACK_DEPTH];
		int truncated = 0;
		size_t depth = lua_profile_capturestack(event.state, captured,
			LP_CAPTURE_STACK_DEPTH, &truncated);
		for (size_t i = 0; i < depth; ++i) {
			frames[i].kind = (lp_frame_kind)captured[i].kind;
			frames[i].function = captured[i].function;
			frames[i].cfunction = captured[i].cfunction;
			frames[i].source = captured[i].source;
			frames[i].source_length = captured[i].source_length;
			frames[i].linedefined = captured[i].linedefined;
			frames[i].currentline = captured[i].currentline;
		}
		lp_runtime_cpu_sample(bridge->runtime, bridge->cpu_generation,
			event.vm_state, event.cfunction, frames, depth,
			truncated != 0, event.weight);
	}
	record_timer_quality(bridge);
}

static void
safe_point(void *userdata, lua_State *L, unsigned int pending) {
	lp_lua_bridge *bridge = userdata;
	lp_thread_timer_begin_collection(bridge->cpu_timer);
	lp_runtime_safe_point(bridge->runtime, bridge->cpu_generation, L,
		pending);
	drain_timer(bridge);
	lp_thread_timer_end_collection(bridge->cpu_timer);
}

static void
state_change(void *userdata, lua_State *L, int state,
	lua_CFunction cfunction) {
	lp_lua_bridge *bridge = userdata;
	lp_thread_timer_publish(bridge->cpu_timer, L, (lp_vm_state)state,
		cfunction);
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
		bridge->cpu_timer = lp_thread_timer_new();
		if (bridge->cpu_timer == NULL) {
			return LP_ERR_NOMEM;
		}
		bridge->cpu_active = true;
		bridge->cpu_generation = generation;
	}
	else {
		bridge->memory_active = true;
		bridge->memory_generation = generation;
	}
	apply_hooks(bridge, current_state);
	if (config->kind == LP_COLLECTOR_CPU) {
		lp_status status = lp_thread_timer_arm(bridge->cpu_timer,
			config->value.cpu.sample_hz);
		if (status != LP_OK) {
			bridge->cpu_active = false;
			bridge->cpu_generation = 0;
			apply_hooks(bridge, current_state);
			lp_thread_timer_delete(bridge->cpu_timer);
			bridge->cpu_timer = NULL;
			return status;
		}
	}
	return LP_OK;
}

static void
stop_collector(void *userdata, lp_runtime *runtime,
	lua_State *current_state, lp_collector_kind kind, uint64_t generation) {
	lp_lua_bridge *bridge = userdata;
	(void)runtime;
	if (kind == LP_COLLECTOR_CPU && bridge->cpu_generation == generation) {
		lp_thread_timer_disarm(bridge->cpu_timer);
		drain_timer(bridge);
		bridge->cpu_active = false;
		bridge->cpu_generation = 0;
	}
	if (kind == LP_COLLECTOR_MEMORY &&
		bridge->memory_generation == generation) {
		bridge->memory_active = false;
		bridge->memory_generation = 0;
	}
	apply_hooks(bridge, current_state);
	if (kind == LP_COLLECTOR_CPU) {
		lp_thread_timer_delete(bridge->cpu_timer);
		bridge->cpu_timer = NULL;
	}
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
