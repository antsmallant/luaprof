#include "luaprof/runtime.h"
#include "cpu_core.h"
#include "memory_core.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct lp_collector_slot {
	bool active;
	uint64_t generation;
	lp_collector_config config;
	lp_result_stats stats;
	lp_cpu_profile *cpu_profile;
	lp_memory_profile *memory_profile;
} lp_collector_slot;

struct lp_profile_model {
	uint64_t next_symbol_id;
	uint64_t next_stack_id;
};

static _Atomic uint64_t memory_seed_counter =
	UINT64_C(0x243f6a8885a308d3);

struct lp_runtime {
	lua_State *main_state;
	lp_host_ops host_ops;
	void *host_userdata;
	uint64_t next_generation;
	bool closing;
	lp_profile_model model;
	lp_collector_slot collectors[LP_COLLECTOR_COUNT];
};

static bool
valid_kind(lp_collector_kind kind) {
	return kind >= LP_COLLECTOR_CPU && kind < LP_COLLECTOR_COUNT;
}

lp_runtime *
lp_runtime_new(lua_State *main_state, const lp_host_ops *host_ops,
	void *host_userdata) {
	lp_runtime *runtime = calloc(1, sizeof(*runtime));
	if (runtime == NULL) {
		return NULL;
	}

	runtime->main_state = main_state;
	runtime->host_userdata = host_userdata;
	runtime->model.next_symbol_id = 1;
	runtime->model.next_stack_id = 1;
	if (host_ops != NULL) {
		runtime->host_ops = *host_ops;
	}
	return runtime;
}

void
lp_runtime_delete(lp_runtime *runtime) {
	if (runtime == NULL) {
		return;
	}

	runtime->closing = true;
	for (int i = 0; i < LP_COLLECTOR_COUNT; ++i) {
		lp_collector_slot *slot = &runtime->collectors[i];
		if (!slot->active) {
			continue;
		}
		if (runtime->host_ops.stop_collector != NULL) {
			runtime->host_ops.stop_collector(runtime->host_userdata, runtime,
				runtime->main_state, (lp_collector_kind)i, slot->generation);
		}
		lp_cpu_profile_delete(slot->cpu_profile);
		lp_memory_profile_delete(slot->memory_profile);
		slot->cpu_profile = NULL;
		slot->memory_profile = NULL;
		slot->active = false;
	}
	free(runtime);
}

lp_status
lp_runtime_start(lp_runtime *runtime, lua_State *current_state,
	const lp_collector_config *config, uint64_t *generation) {
	if (runtime == NULL || config == NULL || generation == NULL ||
		!valid_kind(config->kind)) {
		return LP_ERR_ARGUMENT;
	}
	if (config->kind == LP_COLLECTOR_MEMORY &&
		config->value.memory.sample_bytes == 0) {
		return LP_ERR_ARGUMENT;
	}
	if (config->kind == LP_COLLECTOR_CPU &&
		(config->value.cpu.sample_hz == 0 ||
			config->value.cpu.sample_hz > 10000)) {
		return LP_ERR_ARGUMENT;
	}
	if (runtime->closing) {
		return LP_ERR_CLOSED;
	}

	lp_collector_slot *slot = &runtime->collectors[config->kind];
	if (slot->active) {
		return LP_ERR_BUSY;
	}

	uint64_t next = ++runtime->next_generation;
	if (next == 0) {
		next = ++runtime->next_generation;
	}
	slot->active = true;
	slot->generation = next;
	slot->config = *config;
	if (config->kind == LP_COLLECTOR_CPU) {
		slot->cpu_profile = lp_cpu_profile_new();
		if (slot->cpu_profile == NULL) {
			memset(slot, 0, sizeof(*slot));
			return LP_ERR_NOMEM;
		}
	}
	else {
		uint64_t seed = atomic_fetch_add_explicit(&memory_seed_counter,
			UINT64_C(0x9e3779b97f4a7c15), memory_order_relaxed);
		seed ^= (uint64_t)(uintptr_t)runtime ^ next;
		slot->memory_profile = lp_memory_profile_new(
			config->value.memory.sample_bytes, seed);
		if (slot->memory_profile == NULL) {
			memset(slot, 0, sizeof(*slot));
			return LP_ERR_NOMEM;
		}
	}

	if (runtime->host_ops.start_collector != NULL) {
		lp_status status = runtime->host_ops.start_collector(
			runtime->host_userdata, runtime,
			current_state == NULL ? runtime->main_state : current_state,
			next, config);
		if (status != LP_OK) {
			lp_cpu_profile_delete(slot->cpu_profile);
			lp_memory_profile_delete(slot->memory_profile);
			memset(slot, 0, sizeof(*slot));
			return status == LP_ERR_NOMEM ? status : LP_ERR_HOST;
		}
	}

	*generation = next;
	return LP_OK;
}

lp_status
lp_runtime_stop(lp_runtime *runtime, lua_State *current_state,
	lp_collector_kind kind, uint64_t generation, lp_result_meta *result) {
	if (runtime == NULL || result == NULL || !valid_kind(kind) ||
		generation == 0) {
		return LP_ERR_ARGUMENT;
	}
	if (runtime->closing) {
		return LP_ERR_CLOSED;
	}

	lp_collector_slot *slot = &runtime->collectors[kind];
	if (!slot->active) {
		return LP_ERR_INACTIVE;
	}
	if (slot->generation != generation) {
		return LP_ERR_STALE;
	}

	/* The host disarms asynchronous producers and flushes owner-thread data. */
	if (runtime->host_ops.stop_collector != NULL) {
		runtime->host_ops.stop_collector(runtime->host_userdata, runtime,
			current_state == NULL ? runtime->main_state : current_state,
			kind, generation);
	}
	if (kind == LP_COLLECTOR_CPU) {
		lp_cpu_profile_merge_stats(slot->cpu_profile, &slot->stats);
	}
	else {
		lp_memory_profile_merge_stats(slot->memory_profile, &slot->stats);
	}
	memset(result, 0, sizeof(*result));
	result->kind = kind;
	result->generation = generation;
	result->config = slot->config;
	result->stats = slot->stats;
	result->private_data = kind == LP_COLLECTOR_CPU
		? (void *)slot->cpu_profile : (void *)slot->memory_profile;
	slot->cpu_profile = NULL;
	slot->memory_profile = NULL;
	slot->active = false;
	memset(slot, 0, sizeof(*slot));
	return LP_OK;
}

static uint64_t
saturating_add(uint64_t value, uint64_t increment) {
	return UINT64_MAX - value < increment ? UINT64_MAX : value + increment;
}

void
lp_runtime_safe_point(lp_runtime *runtime, uint64_t generation,
	lua_State *current_state, unsigned int pending) {
	(void)current_state;
	if (runtime == NULL) {
		return;
	}
	lp_collector_slot *slot = &runtime->collectors[LP_COLLECTOR_CPU];
	if (!slot->active || slot->generation != generation || pending == 0) {
		return;
	}
	slot->stats.safe_points = saturating_add(slot->stats.safe_points, 1);
	slot->stats.pending_weight = saturating_add(slot->stats.pending_weight,
		pending);
}

void
lp_runtime_state_change(lp_runtime *runtime, uint64_t generation,
	lua_State *current_state, lp_vm_state state,
	lp_lua_cfunction cfunction) {
	(void)current_state;
	(void)cfunction;
	if (runtime == NULL) {
		return;
	}
	lp_collector_slot *slot = &runtime->collectors[LP_COLLECTOR_CPU];
	if (!slot->active || slot->generation != generation) {
		return;
	}
	uint64_t *counter = NULL;
	switch (state) {
	case LP_VM_HOST:
		counter = &slot->stats.state_host;
		break;
	case LP_VM_LUA:
		counter = &slot->stats.state_lua;
		break;
	case LP_VM_C:
		counter = &slot->stats.state_c;
		break;
	case LP_VM_GC:
		counter = &slot->stats.state_gc;
		break;
	default:
		return;
	}
	*counter = saturating_add(*counter, 1);
}

void
lp_runtime_allocation(lp_runtime *runtime, uint64_t generation,
	lua_State *current_state, void *old_pointer, void *new_pointer,
	size_t old_size, size_t new_size, bool success) {
	(void)current_state;
	(void)old_size;
	if (runtime == NULL) {
		return;
	}
	lp_collector_slot *slot = &runtime->collectors[LP_COLLECTOR_MEMORY];
	if (!slot->active || slot->generation != generation) {
		return;
	}
	uint64_t *counter;
	if (!success) {
		counter = &slot->stats.allocation_failures;
	}
	else if (old_pointer == NULL && new_pointer != NULL && new_size != 0) {
		counter = &slot->stats.allocations;
	}
	else if (old_pointer != NULL && new_pointer == NULL && new_size == 0) {
		counter = &slot->stats.frees;
	}
	else if (old_pointer != NULL && new_pointer != NULL && new_size != 0) {
		counter = &slot->stats.reallocations;
	}
	else {
		return;
	}
	*counter = saturating_add(*counter, 1);
}

bool
lp_runtime_memory_sample_candidate(lp_runtime *runtime,
	uint64_t generation, void *old_pointer, void *new_pointer,
	size_t new_size, bool success, uint64_t *weighted_space,
	uint64_t *weighted_objects) {
	(void)old_pointer;
	if (runtime == NULL || !success || new_pointer == NULL ||
		new_size == 0) {
		return false;
	}
	lp_collector_slot *slot = &runtime->collectors[LP_COLLECTOR_MEMORY];
	if (!slot->active || slot->generation != generation) {
		return false;
	}
	return lp_memory_profile_should_sample(slot->memory_profile, new_size,
		weighted_space, weighted_objects);
}

void
lp_runtime_memory_sample(lp_runtime *runtime, uint64_t generation,
	const lp_stack_frame *frames, size_t depth, bool truncated,
	size_t allocation_size, uint64_t weighted_space,
	uint64_t weighted_objects) {
	if (runtime == NULL) {
		return;
	}
	lp_collector_slot *slot = &runtime->collectors[LP_COLLECTOR_MEMORY];
	if (!slot->active || slot->generation != generation) {
		return;
	}
	lp_memory_profile_record(slot->memory_profile, frames, depth, truncated,
		allocation_size, weighted_space, weighted_objects);
}

void
lp_runtime_cpu_sample(lp_runtime *runtime, uint64_t generation,
	lp_vm_state state, lp_lua_cfunction cfunction,
	const lp_stack_frame *frames, size_t depth, bool truncated,
	uint64_t weight) {
	if (runtime == NULL) {
		return;
	}
	lp_collector_slot *slot = &runtime->collectors[LP_COLLECTOR_CPU];
	if (!slot->active || slot->generation != generation) {
		return;
	}
	lp_cpu_profile_record(slot->cpu_profile, state, cfunction, frames,
		depth, truncated, weight);
}

void
lp_runtime_cpu_quality(lp_runtime *runtime, uint64_t generation,
	uint64_t dropped, uint64_t unstable, uint64_t profiler_overhead) {
	if (runtime == NULL) {
		return;
	}
	lp_collector_slot *slot = &runtime->collectors[LP_COLLECTOR_CPU];
	if (!slot->active || slot->generation != generation) {
		return;
	}
	lp_cpu_profile_quality(slot->cpu_profile, dropped, unstable,
		profiler_overhead);
}

void
lp_runtime_cpu_scheduler_quality(lp_runtime *runtime, uint64_t generation,
	uint64_t stale, uint64_t workers) {
	if (runtime == NULL) {
		return;
	}
	lp_collector_slot *slot = &runtime->collectors[LP_COLLECTOR_CPU];
	if (!slot->active || slot->generation != generation) {
		return;
	}
	slot->stats.stale_events = saturating_add(slot->stats.stale_events,
		stale);
	if (workers > slot->stats.scheduler_workers) {
		slot->stats.scheduler_workers = workers;
	}
}

void
lp_result_meta_dispose(lp_result_meta *result) {
	if (result == NULL) {
		return;
	}
	if (result->kind == LP_COLLECTOR_CPU) {
		lp_cpu_profile_delete(result->private_data);
	}
	else if (result->kind == LP_COLLECTOR_MEMORY) {
		lp_memory_profile_delete(result->private_data);
	}
	result->private_data = NULL;
}

size_t
lp_result_cpu_sample_count(const lp_result_meta *result) {
	if (result == NULL || result->kind != LP_COLLECTOR_CPU) {
		return 0;
	}
	return lp_cpu_profile_sample_count(result->private_data);
}

bool
lp_result_cpu_sample(const lp_result_meta *result, size_t index,
	lp_cpu_sample_view *sample) {
	return result != NULL && result->kind == LP_COLLECTOR_CPU &&
		lp_cpu_profile_sample(result->private_data, index, sample);
}

bool
lp_result_cpu_frame(const lp_result_meta *result, size_t sample_index,
	size_t frame_index, lp_cpu_frame_view *frame) {
	return result != NULL && result->kind == LP_COLLECTOR_CPU &&
		lp_cpu_profile_frame(result->private_data, sample_index,
			frame_index, frame);
}

size_t
lp_result_memory_sample_count(const lp_result_meta *result) {
	if (result == NULL || result->kind != LP_COLLECTOR_MEMORY) {
		return 0;
	}
	return lp_memory_profile_sample_count(result->private_data);
}

bool
lp_result_memory_sample(const lp_result_meta *result, size_t index,
	lp_memory_sample_view *sample) {
	return result != NULL && result->kind == LP_COLLECTOR_MEMORY &&
		lp_memory_profile_sample(result->private_data, index, sample);
}

bool
lp_result_memory_frame(const lp_result_meta *result,
	size_t sample_index, size_t frame_index, lp_memory_frame_view *frame) {
	return result != NULL && result->kind == LP_COLLECTOR_MEMORY &&
		lp_memory_profile_frame(result->private_data, sample_index,
			frame_index, frame);
}

bool
lp_runtime_active(const lp_runtime *runtime, lp_collector_kind kind) {
	return runtime != NULL && valid_kind(kind) &&
		runtime->collectors[kind].active;
}

uint64_t
lp_runtime_generation(const lp_runtime *runtime, lp_collector_kind kind) {
	if (runtime == NULL || !valid_kind(kind)) {
		return 0;
	}
	return runtime->collectors[kind].generation;
}

lua_State *
lp_runtime_main_state(const lp_runtime *runtime) {
	return runtime == NULL ? NULL : runtime->main_state;
}

lp_profile_model *
lp_runtime_model(lp_runtime *runtime) {
	return runtime == NULL ? NULL : &runtime->model;
}

const char *
lp_status_string(lp_status status) {
	switch (status) {
	case LP_OK:
		return "ok";
	case LP_ERR_ARGUMENT:
		return "invalid argument";
	case LP_ERR_BUSY:
		return "collector is already running";
	case LP_ERR_INACTIVE:
		return "collector is not running";
	case LP_ERR_STALE:
		return "stale recorder generation";
	case LP_ERR_HOST:
		return "host failed to start collector";
	case LP_ERR_CLOSED:
		return "profile runtime is closed";
	case LP_ERR_NOMEM:
		return "out of memory";
	default:
		return "unknown error";
	}
}
