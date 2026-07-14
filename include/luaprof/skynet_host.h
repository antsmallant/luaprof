#ifndef LUAPROF_SKYNET_HOST_H
#define LUAPROF_SKYNET_HOST_H

#include <stdbool.h>
#include <stdint.h>

typedef struct lua_State lua_State;
typedef int (*lp_skynet_lua_cfunction)(lua_State *L);

#define LP_SKYNET_HOST_ABI_VERSION 1u

typedef struct lp_skynet_tick_event {
	lua_State *state;
	int vm_state;
	lp_skynet_lua_cfunction cfunction;
	unsigned int weight;
} lp_skynet_tick_event;

typedef struct lp_skynet_quality {
	uint64_t dropped;
	uint64_t unstable;
	uint64_t profiler_overhead;
	uint64_t stale;
	uint64_t worker_mask;
} lp_skynet_quality;

typedef struct lp_skynet_host_api {
	uint32_t abi_version;
	uint32_t (*current_handle)(void);
	int (*target_start)(uint32_t handle, lua_State *main_state,
		uint64_t generation, uint32_t sample_hz, uint64_t *token);
	int (*target_quiesce)(uint64_t token);
	void (*target_release)(uint64_t token);
	void (*publish_state)(uint64_t token, lua_State *state, int vm_state,
		lp_skynet_lua_cfunction cfunction);
	void (*begin_event_drain)(uint64_t token);
	void (*end_event_drain)(uint64_t token);
	bool (*next_event)(uint64_t token, lp_skynet_tick_event *event);
	void (*take_quality)(uint64_t token, lp_skynet_quality *quality);
} lp_skynet_host_api;

/* Called by the optional Skynet integration on worker/dispatch boundaries. */
void lp_skynet_host_worker_start(unsigned int worker_id);
void lp_skynet_host_worker_stop(void);
void lp_skynet_host_dispatch_enter(uint32_t handle);
void lp_skynet_host_dispatch_leave(void);

const lp_skynet_host_api *lp_skynet_host_get_api(uint32_t abi_version);

#endif
