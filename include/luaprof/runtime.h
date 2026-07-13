#ifndef LUAPROF_RUNTIME_H
#define LUAPROF_RUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct lua_State lua_State;
typedef int (*lp_lua_cfunction)(lua_State *L);
typedef struct lp_runtime lp_runtime;
typedef struct lp_profile_model lp_profile_model;

typedef enum lp_collector_kind {
	LP_COLLECTOR_CPU = 0,
	LP_COLLECTOR_MEMORY = 1,
	LP_COLLECTOR_COUNT = 2
} lp_collector_kind;

typedef enum lp_vm_state {
	LP_VM_HOST = 0,
	LP_VM_LUA = 1,
	LP_VM_C = 2,
	LP_VM_GC = 3
} lp_vm_state;

typedef enum lp_status {
	LP_OK = 0,
	LP_ERR_ARGUMENT,
	LP_ERR_BUSY,
	LP_ERR_INACTIVE,
	LP_ERR_STALE,
	LP_ERR_HOST,
	LP_ERR_CLOSED,
	LP_ERR_NOMEM
} lp_status;

typedef struct lp_cpu_config {
	uint32_t sample_hz;
} lp_cpu_config;

typedef struct lp_memory_config {
	uint64_t sample_bytes;
	bool track_free;
} lp_memory_config;

typedef struct lp_collector_config {
	lp_collector_kind kind;
	union {
		lp_cpu_config cpu;
		lp_memory_config memory;
	} value;
} lp_collector_config;

typedef struct lp_result_stats {
	uint64_t safe_points;
	uint64_t pending_weight;
	uint64_t state_host;
	uint64_t state_lua;
	uint64_t state_c;
	uint64_t state_gc;
	uint64_t allocations;
	uint64_t reallocations;
	uint64_t frees;
	uint64_t allocation_failures;
	uint64_t memory_samples;
	uint64_t sampled_alloc_bytes;
	uint64_t alloc_space;
	uint64_t alloc_objects;
	uint64_t samples;
	uint64_t sample_weight;
	uint64_t sample_host;
	uint64_t sample_lua;
	uint64_t sample_c;
	uint64_t sample_gc;
	uint64_t dropped_events;
	uint64_t unstable_events;
	uint64_t profiler_overhead_events;
	uint64_t stale_events;
	uint64_t scheduler_workers;
	uint64_t stack_truncations;
	uint64_t aggregate_overflows;
	uint64_t symbol_overflows;
} lp_result_stats;

typedef enum lp_frame_kind {
	LP_FRAME_LUA = 0,
	LP_FRAME_C = 1
} lp_frame_kind;

typedef struct lp_stack_frame {
	lp_frame_kind kind;
	const void *function;
	lp_lua_cfunction cfunction;
	const char *source;
	size_t source_length;
	int linedefined;
	int currentline;
} lp_stack_frame;

typedef struct lp_cpu_sample_view {
	lp_vm_state state;
	lp_lua_cfunction cfunction;
	uint64_t weight;
	size_t depth;
} lp_cpu_sample_view;

typedef struct lp_frame_view {
	lp_frame_kind kind;
	const void *function;
	lp_lua_cfunction cfunction;
	const char *source;
	size_t source_length;
	int linedefined;
	int currentline;
} lp_frame_view;

typedef lp_frame_view lp_cpu_frame_view;
typedef lp_frame_view lp_memory_frame_view;

typedef struct lp_memory_sample_view {
	uint64_t alloc_space;
	uint64_t alloc_objects;
	uint64_t sampled_bytes;
	uint64_t sample_count;
	size_t depth;
} lp_memory_sample_view;

typedef struct lp_result_meta {
	lp_collector_kind kind;
	uint64_t generation;
	lp_collector_config config;
	lp_result_stats stats;
	void *private_data;
} lp_result_meta;

/*
 * start_collector publishes a collector to the host. stop_collector must
 * synchronously disarm it and wait until no callback can reference this
 * generation before returning.
 */
typedef struct lp_host_ops {
	lp_status (*start_collector)(void *userdata, lp_runtime *runtime,
		lua_State *current_state, uint64_t generation,
		const lp_collector_config *config);
	void (*stop_collector)(void *userdata, lp_runtime *runtime,
		lua_State *current_state, lp_collector_kind kind,
		uint64_t generation);
} lp_host_ops;

/* Lifecycle calls run on the VM owner thread; asynchronous hosts use their
 * own execution slots and must not call these functions from a signal path. */
lp_runtime *lp_runtime_new(lua_State *main_state, const lp_host_ops *host_ops,
	void *host_userdata);
void lp_runtime_delete(lp_runtime *runtime);

lp_status lp_runtime_start(lp_runtime *runtime, lua_State *current_state,
	const lp_collector_config *config, uint64_t *generation);
lp_status lp_runtime_stop(lp_runtime *runtime, lua_State *current_state,
	lp_collector_kind kind, uint64_t generation, lp_result_meta *result);

void lp_runtime_safe_point(lp_runtime *runtime, uint64_t generation,
	lua_State *current_state, unsigned int pending);
void lp_runtime_state_change(lp_runtime *runtime, uint64_t generation,
	lua_State *current_state, lp_vm_state state,
	lp_lua_cfunction cfunction);
void lp_runtime_allocation(lp_runtime *runtime, uint64_t generation,
	lua_State *current_state, void *old_pointer, void *new_pointer,
	size_t old_size, size_t new_size, bool success);
bool lp_runtime_memory_sample_candidate(lp_runtime *runtime,
	uint64_t generation, void *old_pointer, void *new_pointer,
	size_t new_size, bool success, uint64_t *weighted_space,
	uint64_t *weighted_objects);
void lp_runtime_memory_sample(lp_runtime *runtime, uint64_t generation,
	const lp_stack_frame *frames, size_t depth, bool truncated,
	size_t allocation_size, uint64_t weighted_space,
	uint64_t weighted_objects);
void lp_runtime_cpu_sample(lp_runtime *runtime, uint64_t generation,
	lp_vm_state state, lp_lua_cfunction cfunction,
	const lp_stack_frame *frames, size_t depth, bool truncated,
	uint64_t weight);
void lp_runtime_cpu_quality(lp_runtime *runtime, uint64_t generation,
	uint64_t dropped, uint64_t unstable, uint64_t profiler_overhead);
void lp_runtime_cpu_scheduler_quality(lp_runtime *runtime,
	uint64_t generation, uint64_t stale, uint64_t workers);

void lp_result_meta_dispose(lp_result_meta *result);
size_t lp_result_cpu_sample_count(const lp_result_meta *result);
bool lp_result_cpu_sample(const lp_result_meta *result, size_t index,
	lp_cpu_sample_view *sample);
bool lp_result_cpu_frame(const lp_result_meta *result, size_t sample_index,
	size_t frame_index, lp_cpu_frame_view *frame);
size_t lp_result_memory_sample_count(const lp_result_meta *result);
bool lp_result_memory_sample(const lp_result_meta *result, size_t index,
	lp_memory_sample_view *sample);
bool lp_result_memory_frame(const lp_result_meta *result,
	size_t sample_index, size_t frame_index, lp_memory_frame_view *frame);

bool lp_runtime_active(const lp_runtime *runtime, lp_collector_kind kind);
uint64_t lp_runtime_generation(const lp_runtime *runtime,
	lp_collector_kind kind);
lua_State *lp_runtime_main_state(const lp_runtime *runtime);
lp_profile_model *lp_runtime_model(lp_runtime *runtime);
const char *lp_status_string(lp_status status);

#endif
