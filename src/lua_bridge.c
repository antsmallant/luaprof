#include "lua_bridge.h"
#include "skynet_backend.h"
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

static size_t
capture_stack(lua_State *state, lp_stack_frame *frames, bool *truncated) {
	lua_ProfileFrame captured[LP_CAPTURE_STACK_DEPTH];
	int was_truncated = 0;
	size_t depth = lua_profile_capturestack(state, captured,
		LP_CAPTURE_STACK_DEPTH, &was_truncated);
	for (size_t i = 0; i < depth; ++i) {
		frames[i].kind = (lp_frame_kind)captured[i].kind;
		frames[i].function = captured[i].function;
		frames[i].cfunction = captured[i].cfunction;
		frames[i].source = captured[i].source;
		frames[i].source_length = captured[i].source_length;
		frames[i].name = captured[i].name;
		frames[i].name_length = captured[i].name_length;
		frames[i].linedefined = captured[i].linedefined;
		frames[i].currentline = captured[i].currentline;
	}
	*truncated = was_truncated != 0;
	return depth;
}

static bool
scheduler_active(const lp_lua_bridge *bridge) {
	return bridge->scheduler_api != NULL && bridge->scheduler_token != 0;
}

static void
record_thread_quality(lp_lua_bridge *bridge) {
	uint64_t dropped = 0;
	uint64_t unstable = 0;
	uint64_t profiler_overhead = 0;
	uint64_t overrun_events = 0;
	uint64_t overrun_ticks = 0;
	lp_thread_timer_take_quality(bridge->cpu_timer, &dropped, &unstable,
		&profiler_overhead, &overrun_events, &overrun_ticks);
	lp_runtime_cpu_quality(bridge->runtime, bridge->cpu_generation, dropped,
		unstable, profiler_overhead, overrun_events, overrun_ticks);
}

static void
record_cpu_event(lp_lua_bridge *bridge, lua_State *state, lp_vm_state vm_state,
	lp_lua_cfunction cfunction) {
	lp_stack_frame frames[LP_CAPTURE_STACK_DEPTH];
	bool truncated = false;
	size_t depth = 0;
	if (vm_state != LP_VM_HOST) {
		depth = capture_stack(state, frames, &truncated);
	}
	lp_runtime_cpu_sample(bridge->runtime, bridge->cpu_generation, vm_state,
		cfunction, frames, depth, truncated);
}

static void
drain_thread_timer(lp_lua_bridge *bridge) {
	lp_tick_event event;
	while (lp_thread_timer_next(bridge->cpu_timer, &event)) {
		record_cpu_event(bridge, event.state, event.vm_state, event.cfunction);
	}
	record_thread_quality(bridge);
}

static uint64_t
worker_count(uint64_t mask) {
	uint64_t count = 0;
	while (mask != 0) {
		mask &= mask - 1;
		count++;
	}
	return count;
}

static void
drain_scheduler(lp_lua_bridge *bridge) {
	lp_skynet_tick_event event;
	while (bridge->scheduler_api->next_event(bridge->scheduler_token,
		&event)) {
		record_cpu_event(bridge, event.state, (lp_vm_state)event.vm_state,
			(lp_lua_cfunction)event.cfunction);
	}
	lp_skynet_quality quality;
	bridge->scheduler_api->take_quality(bridge->scheduler_token, &quality);
	lp_runtime_cpu_quality(bridge->runtime, bridge->cpu_generation,
		quality.dropped, quality.unstable, quality.profiler_overhead,
		quality.overrun_events, quality.overrun_ticks);
	lp_runtime_cpu_scheduler_quality(bridge->runtime,
		bridge->cpu_generation, quality.stale,
		worker_count(quality.worker_mask));
}

static void
begin_event_drain(lp_lua_bridge *bridge) {
	if (scheduler_active(bridge)) {
		bridge->scheduler_api->begin_event_drain(bridge->scheduler_token);
	}
	else {
		lp_thread_timer_begin_event_drain(bridge->cpu_timer);
	}
}

static void
end_event_drain(lp_lua_bridge *bridge) {
	if (scheduler_active(bridge)) {
		bridge->scheduler_api->end_event_drain(bridge->scheduler_token);
	}
	else {
		lp_thread_timer_end_event_drain(bridge->cpu_timer);
	}
}

static void
drain_cpu(lp_lua_bridge *bridge) {
	if (scheduler_active(bridge)) {
		drain_scheduler(bridge);
	}
	else {
		drain_thread_timer(bridge);
	}
}

static void
safe_point(void *userdata, lua_State *L, unsigned int pending) {
	lp_lua_bridge *bridge = userdata;
	begin_event_drain(bridge);
	lp_runtime_safe_point(bridge->runtime, bridge->cpu_generation, L,
		pending);
	drain_cpu(bridge);
	end_event_drain(bridge);
}

static void
state_change(void *userdata, lua_State *L, int state,
	lua_CFunction cfunction) {
	lp_lua_bridge *bridge = userdata;
	if (scheduler_active(bridge)) {
		bridge->scheduler_api->publish_state(bridge->scheduler_token, L, state,
			(lp_skynet_lua_cfunction)cfunction);
	}
	else {
		lp_thread_timer_publish_state(bridge->cpu_timer, L, (lp_vm_state)state,
			cfunction);
	}
	lp_runtime_state_change(bridge->runtime, bridge->cpu_generation, L,
		(lp_vm_state)state, cfunction);
	if (state == LP_VM_HOST && scheduler_active(bridge)) {
		begin_event_drain(bridge);
		drain_cpu(bridge);
		end_event_drain(bridge);
	}
}

static void
allocation(void *userdata, lua_State *L,
	const lua_ProfileAllocEvent *event) {
	lp_lua_bridge *bridge = userdata;
	lp_runtime_allocation(bridge->runtime, bridge->memory_generation, L,
		event->old_pointer, event->new_pointer, event->old_size,
		event->new_size, event->success != 0);
	uint64_t weighted_space = 0;
	uint64_t weighted_objects = 0;
	if (lp_runtime_memory_sample_candidate(bridge->runtime,
		bridge->memory_generation, event->old_pointer,
		event->new_pointer, event->new_size, event->success != 0,
		&weighted_space, &weighted_objects)) {
		lp_stack_frame frames[LP_CAPTURE_STACK_DEPTH];
		bool truncated = false;
		size_t depth = capture_stack(L, frames, &truncated);
		lp_runtime_memory_sample(bridge->runtime,
			bridge->memory_generation, event->new_pointer, frames, depth,
			truncated, event->new_size, weighted_space, weighted_objects);
	}
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
		if (bridge->scheduler_api != NULL) {
			uint32_t handle = bridge->scheduler_api->current_handle();
			uint64_t token = 0;
			if (handle == 0 || bridge->scheduler_api->target_start(handle,
				bridge->main_state, generation,
				config->value.cpu.sample_hz, &token) != 0) {
				return LP_ERR_HOST;
			}
			bridge->scheduler_token = token;
		}
		else {
			bridge->cpu_timer = lp_thread_timer_new();
			if (bridge->cpu_timer == NULL) {
				return LP_ERR_NOMEM;
			}
		}
		bridge->cpu_active = true;
		bridge->cpu_generation = generation;
	}
	else {
		bridge->memory_active = true;
		bridge->memory_generation = generation;
	}
	apply_hooks(bridge, current_state);
	if (config->kind == LP_COLLECTOR_CPU && !scheduler_active(bridge)) {
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
		if (scheduler_active(bridge)) {
			begin_event_drain(bridge);
			(void)bridge->scheduler_api->target_quiesce(
				bridge->scheduler_token);
			drain_cpu(bridge);
			end_event_drain(bridge);
		}
		else {
			lp_thread_timer_disarm(bridge->cpu_timer);
			drain_thread_timer(bridge);
		}
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
		if (bridge->scheduler_token != 0) {
			bridge->scheduler_api->target_release(
				bridge->scheduler_token);
			bridge->scheduler_token = 0;
		}
		else {
			lp_thread_timer_delete(bridge->cpu_timer);
			bridge->cpu_timer = NULL;
		}
	}
}

void
lp_lua_bridge_init(lp_lua_bridge *bridge, lua_State *main_state) {
	memset(bridge, 0, sizeof(*bridge));
	bridge->main_state = main_state;
	bridge->scheduler_api = lp_skynet_backend_api();
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
