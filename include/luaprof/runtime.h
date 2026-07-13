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
	uint32_t reserved;
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
} lp_result_stats;

typedef struct lp_result_meta {
	lp_collector_kind kind;
	uint64_t generation;
	lp_collector_config config;
	lp_result_stats stats;
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

bool lp_runtime_active(const lp_runtime *runtime, lp_collector_kind kind);
uint64_t lp_runtime_generation(const lp_runtime *runtime,
	lp_collector_kind kind);
lua_State *lp_runtime_main_state(const lp_runtime *runtime);
lp_profile_model *lp_runtime_model(lp_runtime *runtime);
const char *lp_status_string(lp_status status);

#endif
