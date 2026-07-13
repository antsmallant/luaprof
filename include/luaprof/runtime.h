#ifndef LUAPROF_RUNTIME_H
#define LUAPROF_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

typedef struct lua_State lua_State;
typedef struct lp_runtime lp_runtime;
typedef struct lp_profile_model lp_profile_model;

typedef enum lp_collector_kind {
	LP_COLLECTOR_CPU = 0,
	LP_COLLECTOR_MEMORY = 1,
	LP_COLLECTOR_COUNT = 2
} lp_collector_kind;

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

typedef struct lp_result_meta {
	lp_collector_kind kind;
	uint64_t generation;
	lp_collector_config config;
} lp_result_meta;

/*
 * start_collector publishes a collector to the host. stop_collector must
 * synchronously disarm it and wait until no callback can reference this
 * generation before returning.
 */
typedef struct lp_host_ops {
	lp_status (*start_collector)(void *userdata, lp_runtime *runtime,
		uint64_t generation, const lp_collector_config *config);
	void (*stop_collector)(void *userdata, lp_runtime *runtime,
		lp_collector_kind kind, uint64_t generation);
} lp_host_ops;

/* Lifecycle calls run on the VM owner thread; asynchronous hosts use their
 * own execution slots and must not call these functions from a signal path. */
lp_runtime *lp_runtime_new(lua_State *main_state, const lp_host_ops *host_ops,
	void *host_userdata);
void lp_runtime_delete(lp_runtime *runtime);

lp_status lp_runtime_start(lp_runtime *runtime,
	const lp_collector_config *config, uint64_t *generation);
lp_status lp_runtime_stop(lp_runtime *runtime, lp_collector_kind kind,
	uint64_t generation, lp_result_meta *result);

bool lp_runtime_active(const lp_runtime *runtime, lp_collector_kind kind);
uint64_t lp_runtime_generation(const lp_runtime *runtime,
	lp_collector_kind kind);
lua_State *lp_runtime_main_state(const lp_runtime *runtime);
lp_profile_model *lp_runtime_model(lp_runtime *runtime);
const char *lp_status_string(lp_status status);

#endif
