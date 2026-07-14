#define _POSIX_C_SOURCE 200809L

#include "lua_bridge.h"
#include "luaprof/runtime.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

typedef struct test_vm {
	lua_State *L;
	lp_lua_bridge bridge;
	lp_runtime *runtime;
} test_vm;

static void
open_vm(test_vm *vm) {
	memset(vm, 0, sizeof(*vm));
	vm->L = luaL_newstate();
	assert(vm->L != NULL);
	luaL_openlibs(vm->L);
	lp_lua_bridge_init(&vm->bridge, vm->L);
	vm->runtime = lp_runtime_new(vm->L, lp_lua_bridge_host_ops(),
		&vm->bridge);
	assert(vm->runtime != NULL);
	lp_lua_bridge_bind(&vm->bridge, vm->runtime);
}

static void
close_vm(test_vm *vm) {
	lp_runtime_delete(vm->runtime);
	lua_close(vm->L);
}

static void
run_chunk(lua_State *L, const char *source, const char *name) {
	assert(luaL_loadbufferx(L, source, strlen(source), name, NULL) == LUA_OK);
	if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
		fprintf(stderr, "%s\n", lua_tostring(L, -1));
		assert(0);
	}
}

static uint64_t
start_cpu(test_vm *vm) {
	lp_collector_config config = {
		.kind = LP_COLLECTOR_CPU,
		.value.cpu = { .sample_hz = 1000 },
	};
	uint64_t generation;
	assert(lp_runtime_start(vm->runtime, vm->L, &config, &generation)
		== LP_OK);
	return generation;
}

static uint64_t
start_memory(test_vm *vm) {
	lp_collector_config config = {
		.kind = LP_COLLECTOR_MEMORY,
		.value.memory = {
			.sample_bytes = 1024,
			.track_free = true,
		},
	};
	uint64_t generation;
	assert(lp_runtime_start(vm->runtime, vm->L, &config, &generation)
		== LP_OK);
	return generation;
}

static lp_result
stop(test_vm *vm, lp_collector_kind kind, uint64_t generation) {
	lp_result result;
	assert(lp_runtime_stop(vm->runtime, vm->L, kind, generation, &result)
		== LP_OK);
	return result;
}

static bool
cpu_has_source(const lp_result *result, const char *source) {
	size_t length = strlen(source);
	for (size_t i = 0; i < lp_result_cpu_sample_count(result); ++i) {
		lp_cpu_sample_view sample;
		assert(lp_result_cpu_sample(result, i, &sample));
		for (size_t j = 0; j < sample.depth; ++j) {
			lp_cpu_frame_view frame;
			assert(lp_result_cpu_frame(result, i, j, &frame));
			if (frame.source != NULL && frame.source_length == length &&
				memcmp(frame.source, source, length) == 0) {
				return true;
			}
		}
	}
	return false;
}

static bool
memory_has_source(const lp_result *result, const char *source) {
	size_t length = strlen(source);
	for (size_t i = 0; i < lp_result_memory_sample_count(result); ++i) {
		lp_memory_sample_view sample;
		assert(lp_result_memory_sample(result, i, &sample));
		for (size_t j = 0; j < sample.depth; ++j) {
			lp_memory_frame_view frame;
			assert(lp_result_memory_frame(result, i, j, &frame));
			if (frame.source != NULL && frame.source_length == length &&
				memcmp(frame.source, source, length) == 0) {
				return true;
			}
		}
	}
	return false;
}

static const char combined_workload[] =
	"for round = 1, 8 do\n"
	"  local values = {}\n"
	"  for i = 1, 30000 do values[i] = { i, tostring(i) } end\n"
	"  local total = 0\n"
	"  for i = 1, 1500000 do total = total + i end\n"
	"end\n";

static void
test_memory_stops_first(void) {
	test_vm vm;
	open_vm(&vm);
	uint64_t cpu = start_cpu(&vm);
	uint64_t memory = start_memory(&vm);
	run_chunk(vm.L, combined_workload, "@combined_before_memory_stop.lua");
	lp_result memory_result = stop(&vm, LP_COLLECTOR_MEMORY, memory);
	assert(memory_result.stats.memory_samples != 0);
	assert(memory_result.stats.inuse_space != 0);
	run_chunk(vm.L,
		"local total = 0\n"
		"for i = 1, 20000000 do total = total + i end\n",
		"@combined_after_memory_stop.lua");
	lp_result cpu_result = stop(&vm, LP_COLLECTOR_CPU, cpu);
	assert(cpu_result.stats.sample_lua >= 20);
	assert(cpu_has_source(&cpu_result, "@combined_after_memory_stop.lua"));
	lp_result_dispose(&memory_result);
	lp_result_dispose(&cpu_result);
	close_vm(&vm);
}

static void
test_cpu_stops_first(void) {
	test_vm vm;
	open_vm(&vm);
	uint64_t cpu = start_cpu(&vm);
	uint64_t memory = start_memory(&vm);
	run_chunk(vm.L, combined_workload, "@combined_before_cpu_stop.lua");
	lp_result cpu_result = stop(&vm, LP_COLLECTOR_CPU, cpu);
	assert(cpu_result.stats.sample_lua != 0);
	run_chunk(vm.L,
		"local keep = {}\n"
		"for i = 1, 30000 do keep[i] = string.rep('m', i % 100 + 1) end\n"
		"_G.combined_keep = keep\n",
		"@combined_after_cpu_stop.lua");
	lp_result memory_result = stop(&vm, LP_COLLECTOR_MEMORY, memory);
	assert(memory_result.stats.memory_samples != 0);
	assert(memory_has_source(&memory_result, "@combined_after_cpu_stop.lua"));
	lp_result_dispose(&cpu_result);
	lp_result_dispose(&memory_result);
	close_vm(&vm);
}

static void
test_repeated_combined_lifecycle(void) {
	test_vm vm;
	open_vm(&vm);
	for (unsigned int cycle = 0; cycle < 100; ++cycle) {
		uint64_t cpu = start_cpu(&vm);
		uint64_t memory = start_memory(&vm);
		run_chunk(vm.L,
			"local values = {}\n"
			"for i = 1, 200 do values[i] = { i, tostring(i) } end\n",
			"@combined_lifecycle.lua");
		lp_result first;
		lp_result second;
		if ((cycle & 1u) == 0) {
			first = stop(&vm, LP_COLLECTOR_CPU, cpu);
			second = stop(&vm, LP_COLLECTOR_MEMORY, memory);
		}
		else {
			first = stop(&vm, LP_COLLECTOR_MEMORY, memory);
			second = stop(&vm, LP_COLLECTOR_CPU, cpu);
		}
		lp_result_dispose(&first);
		lp_result_dispose(&second);
		assert(!lp_runtime_active(vm.runtime, LP_COLLECTOR_CPU));
		assert(!lp_runtime_active(vm.runtime, LP_COLLECTOR_MEMORY));
	}
	close_vm(&vm);
}

int
main(int argc, char **argv) {
	if (argc == 2 && strcmp(argv[1], "--lifecycle-only") == 0) {
		test_repeated_combined_lifecycle();
		puts("luaprof combined sampling lifecycle: ok");
		return EXIT_SUCCESS;
	}
	assert(argc == 1);
	test_memory_stops_first();
	test_cpu_stops_first();
	test_repeated_combined_lifecycle();
	puts("luaprof combined CPU/memory sampling: ok");
	return EXIT_SUCCESS;
}
