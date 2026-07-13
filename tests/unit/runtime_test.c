#include "luaprof/runtime.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

typedef struct fake_host {
	bool callback_active[LP_COLLECTOR_COUNT];
	unsigned starts[LP_COLLECTOR_COUNT];
	unsigned stops[LP_COLLECTOR_COUNT];
	bool fail_next_start;
} fake_host;

static lp_status
fake_start(void *userdata, lp_runtime *runtime, lua_State *current_state,
	uint64_t generation, const lp_collector_config *config) {
	fake_host *host = userdata;
	(void)current_state;
	assert(generation != 0);
	assert(lp_runtime_active(runtime, config->kind));
	if (host->fail_next_start) {
		host->fail_next_start = false;
		return LP_ERR_HOST;
	}
	host->callback_active[config->kind] = true;
	host->starts[config->kind]++;
	return LP_OK;
}

static void
fake_stop(void *userdata, lp_runtime *runtime, lua_State *current_state,
	lp_collector_kind kind, uint64_t generation) {
	fake_host *host = userdata;
	(void)current_state;
	assert(generation != 0);
	assert(!lp_runtime_active(runtime, kind));
	assert(host->callback_active[kind]);
	host->callback_active[kind] = false;
	host->stops[kind]++;
}

static lp_collector_config
cpu_config(void) {
	lp_collector_config config = { .kind = LP_COLLECTOR_CPU };
	return config;
}

static lp_collector_config
memory_config(bool track_free) {
	lp_collector_config config = {
		.kind = LP_COLLECTOR_MEMORY,
		.value.memory = {
			.sample_bytes = 4096,
			.track_free = track_free,
		},
	};
	return config;
}

int
main(void) {
	fake_host host = { 0 };
	lp_host_ops ops = {
		.start_collector = fake_start,
		.stop_collector = fake_stop,
	};
	lua_State *state_identity = (lua_State *)&host;
	lp_runtime *runtime = lp_runtime_new(state_identity, &ops, &host);
	assert(runtime != NULL);
	assert(lp_runtime_main_state(runtime) == state_identity);
	lp_profile_model *shared_model = lp_runtime_model(runtime);
	assert(shared_model != NULL);
	assert(!lp_runtime_active(runtime, LP_COLLECTOR_CPU));
	assert(!lp_runtime_active(runtime, LP_COLLECTOR_MEMORY));
	assert(host.starts[LP_COLLECTOR_CPU] == 0);
	assert(host.starts[LP_COLLECTOR_MEMORY] == 0);

	lp_collector_config cpu = cpu_config();
	lp_collector_config memory = memory_config(true);
	uint64_t cpu_generation = 0;
	uint64_t memory_generation = 0;
	lp_collector_config invalid_memory = memory;
	invalid_memory.value.memory.sample_bytes = 0;
	assert(lp_runtime_start(runtime, state_identity, &invalid_memory,
		&memory_generation) == LP_ERR_ARGUMENT);
	assert(lp_runtime_start(runtime, state_identity, &cpu,
		&cpu_generation) == LP_OK);
	assert(lp_runtime_start(runtime, state_identity, &cpu,
		&memory_generation) == LP_ERR_BUSY);
	assert(lp_runtime_start(runtime, state_identity, &memory,
		&memory_generation) == LP_OK);
	assert(cpu_generation != memory_generation);
	assert(host.callback_active[LP_COLLECTOR_CPU]);
	assert(host.callback_active[LP_COLLECTOR_MEMORY]);
	lp_runtime_safe_point(runtime, cpu_generation, state_identity, 5);
	lp_runtime_safe_point(runtime, cpu_generation + 1, state_identity, 20);
	lp_runtime_state_change(runtime, cpu_generation, state_identity,
		LP_VM_C, NULL);
	lp_runtime_allocation(runtime, memory_generation, state_identity,
		NULL, &host, 0, 64, true);
	lp_runtime_allocation(runtime, memory_generation, state_identity,
		&host, &host, 64, 128, true);
	lp_runtime_allocation(runtime, memory_generation, state_identity,
		&host, NULL, 128, 0, true);
	lp_runtime_allocation(runtime, memory_generation, state_identity,
		&host, NULL, 128, 256, false);

	lp_result_meta result;
	assert(lp_runtime_stop(runtime, state_identity, LP_COLLECTOR_MEMORY,
		memory_generation + 1, &result) == LP_ERR_STALE);
	assert(lp_runtime_stop(runtime, state_identity, LP_COLLECTOR_MEMORY,
		memory_generation, &result) == LP_OK);
	assert(result.kind == LP_COLLECTOR_MEMORY);
	assert(result.config.value.memory.track_free);
	assert(result.stats.allocations == 1);
	assert(result.stats.reallocations == 1);
	assert(result.stats.frees == 1);
	assert(result.stats.allocation_failures == 1);
	assert(!host.callback_active[LP_COLLECTOR_MEMORY]);
	assert(host.callback_active[LP_COLLECTOR_CPU]);
	assert(lp_runtime_stop(runtime, state_identity, LP_COLLECTOR_CPU,
		cpu_generation, &result) == LP_OK);
	assert(result.stats.safe_points == 1);
	assert(result.stats.pending_weight == 5);
	assert(result.stats.state_c == 1);
	assert(!host.callback_active[LP_COLLECTOR_CPU]);
	assert(lp_runtime_model(runtime) == shared_model);

	assert(lp_runtime_start(runtime, state_identity, &cpu,
		&cpu_generation) == LP_OK);
	assert(lp_runtime_start(runtime, state_identity, &memory,
		&memory_generation) == LP_OK);
	assert(lp_runtime_stop(runtime, state_identity, LP_COLLECTOR_CPU,
		cpu_generation, &result) == LP_OK);
	assert(host.callback_active[LP_COLLECTOR_MEMORY]);
	assert(lp_runtime_stop(runtime, state_identity, LP_COLLECTOR_MEMORY,
		memory_generation, &result) == LP_OK);

	host.fail_next_start = true;
	assert(lp_runtime_start(runtime, state_identity, &cpu,
		&cpu_generation) == LP_ERR_HOST);
	assert(!lp_runtime_active(runtime, LP_COLLECTOR_CPU));
	assert(!host.callback_active[LP_COLLECTOR_CPU]);
	assert(lp_runtime_start(runtime, state_identity, &cpu,
		&cpu_generation) == LP_OK);
	assert(lp_runtime_start(runtime, state_identity, &memory,
		&memory_generation) == LP_OK);

	fake_host second_host = { 0 };
	lp_runtime *second_runtime = lp_runtime_new(NULL, &ops, &second_host);
	assert(second_runtime != NULL);
	uint64_t second_generation = 0;
	assert(lp_runtime_start(second_runtime, NULL, &cpu,
		&second_generation) == LP_OK);
	assert(lp_runtime_active(runtime, LP_COLLECTOR_CPU));
	assert(lp_runtime_active(second_runtime, LP_COLLECTOR_CPU));
	lp_runtime_delete(second_runtime);
	assert(!second_host.callback_active[LP_COLLECTOR_CPU]);

	lp_runtime_delete(runtime);
	assert(!host.callback_active[LP_COLLECTOR_CPU]);
	assert(!host.callback_active[LP_COLLECTOR_MEMORY]);
	assert(host.starts[LP_COLLECTOR_CPU] == host.stops[LP_COLLECTOR_CPU]);
	assert(host.starts[LP_COLLECTOR_MEMORY] == host.stops[LP_COLLECTOR_MEMORY]);

	puts("luaprof runtime lifecycle: ok");
	return 0;
}
