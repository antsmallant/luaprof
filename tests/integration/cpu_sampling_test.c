#define _POSIX_C_SOURCE 200809L

#include "lua_bridge.h"
#include "luaprof/runtime.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

typedef struct test_vm {
	lua_State *L;
	lp_lua_bridge bridge;
	lp_runtime *runtime;
} test_vm;

static uint64_t
thread_cpu_ns(void) {
	struct timespec now;
	assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0);
	return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
		(uint64_t)now.tv_nsec;
}

static void
busy_for(uint64_t duration_ns) {
	volatile uint64_t value = 1;
	uint64_t deadline = thread_cpu_ns() + duration_ns;
	do {
		for (unsigned int i = 0; i < 10000; ++i) {
			value = value * UINT64_C(6364136223846793005) + 1;
		}
	} while (thread_cpu_ns() < deadline);
	(void)value;
}

static int
busy_cfunction(lua_State *L) {
	uint64_t duration_ns = (uint64_t)luaL_checkinteger(L, 1);
	busy_for(duration_ns);
	return 0;
}

static void
vm_open(test_vm *vm) {
	memset(vm, 0, sizeof(*vm));
	vm->L = luaL_newstate();
	assert(vm->L != NULL);
	luaL_openlibs(vm->L);
	lp_lua_bridge_init(&vm->bridge, vm->L);
	vm->runtime = lp_runtime_new(vm->L, lp_lua_bridge_host_ops(),
		&vm->bridge);
	assert(vm->runtime != NULL);
	lp_lua_bridge_bind(&vm->bridge, vm->runtime);
	lua_pushcfunction(vm->L, busy_cfunction);
	lua_setglobal(vm->L, "busy_cfunction");
}

static void
vm_close(test_vm *vm) {
	lp_runtime_delete(vm->runtime);
	lua_close(vm->L);
}

static uint64_t
start_cpu(test_vm *vm, uint32_t sample_hz) {
	lp_collector_config config = {
		.kind = LP_COLLECTOR_CPU,
		.value.cpu = { .sample_hz = sample_hz },
	};
	uint64_t generation = 0;
	assert(lp_runtime_start(vm->runtime, vm->L, &config, &generation)
		== LP_OK);
	return generation;
}

static lp_result_meta
stop_cpu(test_vm *vm, uint64_t generation) {
	lp_result_meta result = { 0 };
	assert(lp_runtime_stop(vm->runtime, vm->L, LP_COLLECTOR_CPU,
		generation, &result) == LP_OK);
	return result;
}

static void
run_chunk(lua_State *L, const char *source, const char *name) {
	assert(luaL_loadbufferx(L, source, strlen(source), name, NULL) == LUA_OK);
	if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
		fprintf(stderr, "%s\n", lua_tostring(L, -1));
		assert(0);
	}
}

static int
result_has_cfunction(const lp_result_meta *result,
	lp_lua_cfunction wanted, int require_lua_caller) {
	size_t count = lp_result_cpu_sample_count(result);
	for (size_t i = 0; i < count; ++i) {
		lp_cpu_sample_view sample;
		assert(lp_result_cpu_sample(result, i, &sample));
		if (sample.state != LP_VM_C || sample.cfunction != wanted) {
			continue;
		}
		if (!require_lua_caller) {
			return 1;
		}
		for (size_t frame_index = 0; frame_index < sample.depth;
			++frame_index) {
			lp_cpu_frame_view frame;
			assert(lp_result_cpu_frame(result, i, frame_index, &frame));
			if (frame.kind == LP_FRAME_LUA && frame.source != NULL &&
				frame.currentline > 0) {
				return 1;
			}
		}
	}
	return 0;
}

static void
test_lua_samples(void) {
	test_vm vm;
	vm_open(&vm);
	uint64_t generation = start_cpu(&vm, 1000);
	run_chunk(vm.L,
		"local value = 0\n"
		"for i = 1, 30000000 do value = value + i end\n"
		"assert(value > 0)\n",
		"@cpu_lua_workload.lua");
	lp_result_meta result = stop_cpu(&vm, generation);
	assert(result.stats.sample_lua >= 20);
	assert(result.stats.sample_weight == result.stats.sample_lua +
		result.stats.sample_c + result.stats.sample_gc +
		result.stats.sample_host);
	assert(lp_result_cpu_sample_count(&result) != 0);
	int saw_source = 0;
	for (size_t i = 0; i < lp_result_cpu_sample_count(&result); ++i) {
		lp_cpu_sample_view sample;
		assert(lp_result_cpu_sample(&result, i, &sample));
		for (size_t j = 0; j < sample.depth; ++j) {
			lp_cpu_frame_view frame;
			assert(lp_result_cpu_frame(&result, i, j, &frame));
			if (frame.source != NULL && frame.currentline == 2 &&
				frame.source_length == sizeof("@cpu_lua_workload.lua") - 1 &&
				memcmp(frame.source, "@cpu_lua_workload.lua",
					frame.source_length) == 0) {
				saw_source = 1;
			}
		}
	}
	assert(saw_source);
	lp_result_meta_dispose(&result);
	vm_close(&vm);
}

static void
test_cfunction_samples(void) {
	test_vm vm;
	vm_open(&vm);
	uint64_t generation = start_cpu(&vm, 1000);
	run_chunk(vm.L,
		"busy_cfunction(80000000)\n"
		"local after = true\n",
		"@cpu_c_workload.lua");
	lp_result_meta result = stop_cpu(&vm, generation);
	assert(result.stats.sample_c >= 20);
	assert(result_has_cfunction(&result, busy_cfunction, 1));
	lp_result_meta_dispose(&result);
	vm_close(&vm);
}

static void
test_host_and_sleep(void) {
	test_vm vm;
	vm_open(&vm);
	uint64_t generation = start_cpu(&vm, 500);
	busy_for(UINT64_C(50000000));
	lp_result_meta host = stop_cpu(&vm, generation);
	assert(host.stats.sample_host >= 10);
	lp_result_meta_dispose(&host);

	generation = start_cpu(&vm, 50);
	struct timespec sleep_time = { .tv_sec = 0, .tv_nsec = 100000000 };
	assert(nanosleep(&sleep_time, NULL) == 0);
	lp_result_meta sleep = stop_cpu(&vm, generation);
	assert(sleep.stats.sample_weight == 0);
	lp_result_meta_dispose(&sleep);
	vm_close(&vm);
}

static void
test_gc_samples(void) {
	test_vm vm;
	vm_open(&vm);
	uint64_t generation = start_cpu(&vm, 1000);
	run_chunk(vm.L,
		"for round = 1, 12 do\n"
		"  local values = {}\n"
		"  for i = 1, 50000 do values[i] = { i, tostring(i) } end\n"
		"  values = nil\n"
		"  collectgarbage('collect')\n"
		"end\n",
		"@cpu_gc_workload.lua");
	lp_result_meta result = stop_cpu(&vm, generation);
	assert(result.stats.sample_gc != 0);
	lp_result_meta_dispose(&result);
	vm_close(&vm);
}

static void
test_known_hotspot_ratio(void) {
	test_vm vm;
	vm_open(&vm);
	uint64_t generation = start_cpu(&vm, 1000);
	run_chunk(vm.L,
		"local function hot()\n"
		"  local value = 0\n"
		"  for i = 1, 4000000 do value = value + i end\n"
		"end\n"
		"local function cold()\n"
		"  local value = 0\n"
		"  for i = 1, 1000000 do value = value + i end\n"
		"end\n"
		"for round = 1, 8 do hot(); cold() end\n",
		"@cpu_ratio_workload.lua");
	lp_result_meta result = stop_cpu(&vm, generation);
	uint64_t hot = 0;
	uint64_t cold = 0;
	for (size_t i = 0; i < lp_result_cpu_sample_count(&result); ++i) {
		lp_cpu_sample_view sample;
		assert(lp_result_cpu_sample(&result, i, &sample));
		for (size_t j = 0; j < sample.depth; ++j) {
			lp_cpu_frame_view frame;
			assert(lp_result_cpu_frame(&result, i, j, &frame));
			if (frame.kind != LP_FRAME_LUA || frame.source == NULL ||
				frame.source_length != sizeof("@cpu_ratio_workload.lua") - 1 ||
				memcmp(frame.source, "@cpu_ratio_workload.lua",
					frame.source_length) != 0) {
				continue;
			}
			if (frame.linedefined == 1) {
				hot += sample.weight;
			}
			else if (frame.linedefined == 5) {
				cold += sample.weight;
			}
			break;
		}
	}
	assert(hot >= 20);
	assert(cold >= 5);
	double ratio = (double)hot / (double)cold;
	assert(ratio > 2.5 && ratio < 6.0);
	lp_result_meta_dispose(&result);
	vm_close(&vm);
}

static void *
thread_worker(void *argument) {
	uint64_t *samples = argument;
	test_vm vm;
	vm_open(&vm);
	uint64_t generation = start_cpu(&vm, 1000);
	run_chunk(vm.L,
		"local value = 0\n"
		"for i = 1, 20000000 do value = value + i end\n",
		"@cpu_thread_workload.lua");
	lp_result_meta result = stop_cpu(&vm, generation);
	*samples = result.stats.sample_lua;
	lp_result_meta_dispose(&result);
	vm_close(&vm);
	return NULL;
}

static void
test_multiple_threads(void) {
	pthread_t threads[2];
	uint64_t samples[2] = { 0, 0 };
	for (size_t i = 0; i < 2; ++i) {
		assert(pthread_create(&threads[i], NULL, thread_worker, &samples[i])
			== 0);
	}
	for (size_t i = 0; i < 2; ++i) {
		assert(pthread_join(threads[i], NULL) == 0);
		assert(samples[i] >= 10);
	}
}

static void
test_repeated_stop(void) {
	test_vm vm;
	vm_open(&vm);
	for (unsigned int i = 0; i < 50; ++i) {
		uint64_t generation = start_cpu(&vm, 10000);
		busy_for(UINT64_C(300000));
		lp_result_meta result = stop_cpu(&vm, generation);
		lp_result_meta_dispose(&result);
	}
	vm_close(&vm);
}

static void
test_one_vm_per_thread(void) {
	test_vm first;
	test_vm second;
	vm_open(&first);
	vm_open(&second);
	uint64_t first_generation = start_cpu(&first, 1000);
	lp_collector_config config = {
		.kind = LP_COLLECTOR_CPU,
		.value.cpu = { .sample_hz = 1000 },
	};
	uint64_t second_generation = 0;
	assert(lp_runtime_start(second.runtime, second.L, &config,
		&second_generation) == LP_ERR_HOST);
	lp_result_meta result = stop_cpu(&first, first_generation);
	lp_result_meta_dispose(&result);
	vm_close(&second);
	vm_close(&first);
}

static void
test_delete_active_runtime(void) {
	test_vm vm;
	vm_open(&vm);
	(void)start_cpu(&vm, 1000);
	busy_for(UINT64_C(5000000));
	lp_runtime_delete(vm.runtime);
	vm.runtime = NULL;
	lua_close(vm.L);
}

int
main(void) {
	test_lua_samples();
	test_cfunction_samples();
	test_host_and_sleep();
	test_gc_samples();
	test_known_hotspot_ratio();
	test_multiple_threads();
	test_repeated_stop();
	test_one_vm_per_thread();
	test_delete_active_runtime();
	puts("luaprof thread CPU sampling: ok");
	return EXIT_SUCCESS;
}
