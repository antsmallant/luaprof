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

typedef struct allocation_tracker {
	bool enabled;
	uint64_t objects;
	uint64_t bytes;
	uint64_t live_objects;
	uint64_t live_bytes;
	struct tracked_allocation *live;
	size_t live_count;
	size_t live_capacity;
} allocation_tracker;

typedef struct tracked_allocation {
	void *pointer;
	size_t size;
} tracked_allocation;

typedef struct test_vm {
	lua_State *L;
	lp_lua_bridge bridge;
	lp_runtime *runtime;
	allocation_tracker tracker;
} test_vm;

static void *
tracking_allocator(void *userdata, void *pointer, size_t old_size,
	size_t new_size) {
	(void)old_size;
	allocation_tracker *tracker = userdata;
	if (new_size == 0) {
		if (tracker->enabled) {
			for (size_t i = 0; i < tracker->live_count; ++i) {
				if (tracker->live[i].pointer == pointer) {
					tracker->live_bytes -= tracker->live[i].size;
					tracker->live_objects--;
					tracker->live[i] =
						tracker->live[--tracker->live_count];
					break;
				}
			}
		}
		free(pointer);
		return NULL;
	}
	size_t tracked_index = SIZE_MAX;
	if (tracker->enabled) {
		for (size_t i = 0; i < tracker->live_count; ++i) {
			if (tracker->live[i].pointer == pointer) {
				tracked_index = i;
				break;
			}
		}
	}
	void *new_pointer = realloc(pointer, new_size);
	if (new_pointer != NULL && tracker->enabled) {
		if (tracked_index != SIZE_MAX) {
			tracker->live_bytes -= tracker->live[tracked_index].size;
			tracker->live_objects--;
			tracker->live[tracked_index] =
				tracker->live[--tracker->live_count];
		}
		if (tracker->live_count == tracker->live_capacity) {
			size_t capacity = tracker->live_capacity == 0
				? 256 : tracker->live_capacity * 2;
			tracked_allocation *live = realloc(tracker->live,
				capacity * sizeof(live[0]));
			if (live == NULL) {
				abort();
			}
			tracker->live = live;
			tracker->live_capacity = capacity;
		}
		tracker->live[tracker->live_count++] = (tracked_allocation) {
			.pointer = new_pointer,
			.size = new_size,
		};
		tracker->live_bytes += new_size;
		tracker->live_objects++;
		tracker->objects++;
		tracker->bytes += new_size;
	}
	return new_pointer;
}

static void
open_vm(test_vm *vm) {
	memset(vm, 0, sizeof(*vm));
	vm->L = lua_newstate(tracking_allocator, &vm->tracker);
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
	free(vm->tracker.live);
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
start_memory(test_vm *vm, uint64_t sample_bytes, bool track_free) {
	lp_collector_config config = {
		.kind = LP_COLLECTOR_MEMORY,
		.value.memory = {
			.sample_bytes = sample_bytes,
			.track_free = track_free,
		},
	};
	uint64_t generation = 0;
	assert(lp_runtime_start(vm->runtime, vm->L, &config, &generation)
		== LP_OK);
	return generation;
}

static lp_result_meta
stop_memory(test_vm *vm, uint64_t generation) {
	lp_result_meta result = { 0 };
	assert(lp_runtime_stop(vm->runtime, vm->L, LP_COLLECTOR_MEMORY,
		generation, &result) == LP_OK);
	return result;
}

static int
result_has_source(const lp_result_meta *result, const char *source) {
	size_t source_length = strlen(source);
	for (size_t i = 0; i < lp_result_memory_sample_count(result); ++i) {
		lp_memory_sample_view sample;
		assert(lp_result_memory_sample(result, i, &sample));
		for (size_t j = 0; j < sample.depth; ++j) {
			lp_memory_frame_view frame;
			assert(lp_result_memory_frame(result, i, j, &frame));
			if (frame.source != NULL &&
				frame.source_length == source_length &&
				memcmp(frame.source, source, source_length) == 0) {
				return 1;
			}
		}
	}
	return 0;
}

static void
test_exact_mode(void) {
	test_vm vm;
	open_vm(&vm);
	uint64_t generation = start_memory(&vm, 1, false);
	vm.tracker.enabled = true;
	run_chunk(vm.L,
		"for round = 1, 8 do\n"
		"  local values = {}\n"
		"  for i = 1, 2000 do values[i] = { i, tostring(i) } end\n"
		"end\n",
		"@memory_exact.lua");
	vm.tracker.enabled = false;
	lp_result_meta result = stop_memory(&vm, generation);
	assert(vm.tracker.objects != 0);
	assert(result.stats.memory_samples == vm.tracker.objects);
	assert(result.stats.sampled_alloc_bytes == vm.tracker.bytes);
	assert(result.stats.alloc_space == vm.tracker.bytes);
	assert(result.stats.alloc_objects == vm.tracker.objects);
	assert(result.stats.inuse_space == 0);
	assert(result.stats.inuse_objects == 0);
	assert(result.stats.memory_samples == result.stats.allocations +
		result.stats.reallocations);
	assert(result.stats.reallocations != 0);
	assert(lp_result_memory_sample_count(&result) != 0);
	assert(result_has_source(&result, "@memory_exact.lua"));
	lp_result_meta_dispose(&result);
	close_vm(&vm);
}

static void
test_sampled_mode(void) {
	test_vm vm;
	open_vm(&vm);
	uint64_t generation = start_memory(&vm, 4096, false);
	run_chunk(vm.L,
		"local keep = {}\n"
		"for i = 1, 30000 do keep[i] = string.rep('x', i % 200 + 1) end\n",
		"@memory_sampled.lua");
	lp_result_meta result = stop_memory(&vm, generation);
	assert(result.stats.memory_samples > 10);
	assert(result.stats.memory_samples < result.stats.allocations +
		result.stats.reallocations);
	assert(result.stats.alloc_space > result.stats.sampled_alloc_bytes);
	assert(result.stats.alloc_objects > result.stats.memory_samples);
	assert(result_has_source(&result, "@memory_sampled.lua"));
	lp_result_meta_dispose(&result);
	close_vm(&vm);
}

static void
test_exact_live_mode(void) {
	test_vm vm;
	open_vm(&vm);
	uint64_t generation = start_memory(&vm, 1, true);
	vm.tracker.enabled = true;
	run_chunk(vm.L,
		"local keep = {}\n"
		"for i = 1, 1000 do keep[i] = { i, tostring(i) } end\n"
		"for round = 1, 4 do\n"
		"  local discard = {}\n"
		"  for i = 1, 2000 do discard[i] = string.rep('x', i % 80) end\n"
		"end\n"
		"collectgarbage('collect')\n"
		"_G.memory_live_keep = keep\n",
		"@memory_live.lua");
	lp_result_meta result = stop_memory(&vm, generation);
	vm.tracker.enabled = false;
	assert(vm.tracker.live_objects != 0);
	assert(vm.tracker.live_objects < vm.tracker.objects);
	assert(result.stats.frees != 0);
	assert(result.stats.inuse_space == vm.tracker.live_bytes);
	assert(result.stats.inuse_objects == vm.tracker.live_objects);
	assert(result.stats.live_map_overflows == 0);
	uint64_t aggregate_space = 0;
	uint64_t aggregate_objects = 0;
	for (size_t i = 0; i < lp_result_memory_sample_count(&result); ++i) {
		lp_memory_sample_view sample;
		assert(lp_result_memory_sample(&result, i, &sample));
		aggregate_space += sample.inuse_space;
		aggregate_objects += sample.inuse_objects;
	}
	assert(aggregate_space == result.stats.inuse_space);
	assert(aggregate_objects == result.stats.inuse_objects);
	assert(result_has_source(&result, "@memory_live.lua"));
	lp_result_meta_dispose(&result);
	close_vm(&vm);
}

int
main(void) {
	test_exact_mode();
	test_sampled_mode();
	test_exact_live_mode();
	puts("luaprof alloc-space memory sampling: ok");
	return EXIT_SUCCESS;
}
