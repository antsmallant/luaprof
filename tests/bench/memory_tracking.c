#define _POSIX_C_SOURCE 200809L

#include "luaprof/runtime.h"

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define ITERATIONS UINT64_C(5000000)

static uint64_t
nanoseconds(struct timespec value) {
	return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
		(uint64_t)value.tv_nsec;
}

static double
run_case(const char *name, int active, bool track_free) {
	lp_runtime *runtime = lp_runtime_new(NULL, NULL, NULL);
	assert(runtime != NULL);
	uint64_t generation = 1;
	if (active) {
		lp_collector_config config = {
			.kind = LP_COLLECTOR_MEMORY,
			.value.memory = {
				.sample_bytes = 4096,
				.track_free = track_free,
			},
		};
		assert(lp_runtime_start(runtime, NULL, &config, &generation) == LP_OK);
	}

	lp_stack_frame frame = {
		.kind = LP_FRAME_LUA,
		.function = runtime,
		.source = "@memory_tracking_bench.lua",
		.source_length = sizeof("@memory_tracking_bench.lua") - 1,
		.linedefined = 1,
		.currentline = 1,
	};
	struct timespec begin;
	struct timespec end;
	clock_gettime(CLOCK_MONOTONIC, &begin);
	for (uint64_t i = 0; i < ITERATIONS; ++i) {
		void *pointer = (void *)(uintptr_t)(((i & 65535u) + 1u) * 16u);
		size_t size = 48u + (size_t)(i & 31u);
		lp_runtime_allocation(runtime, generation, NULL, NULL, pointer, 0,
			size, true);
		uint64_t weighted_space = 0;
		uint64_t weighted_objects = 0;
		if (lp_runtime_memory_sample_candidate(runtime, generation, NULL,
			pointer, size, true, &weighted_space, &weighted_objects)) {
			lp_runtime_memory_sample(runtime, generation, pointer, &frame, 1,
				false, size, weighted_space, weighted_objects);
		}
		lp_runtime_allocation(runtime, generation, NULL, pointer, NULL, size,
			0, true);
		(void)lp_runtime_memory_sample_candidate(runtime, generation, pointer,
			NULL, 0, true, &weighted_space, &weighted_objects);
	}
	clock_gettime(CLOCK_MONOTONIC, &end);

	if (active) {
		lp_result_meta result;
		assert(lp_runtime_stop(runtime, NULL, LP_COLLECTOR_MEMORY, generation,
			&result) == LP_OK);
		assert(result.stats.inuse_space == 0);
		assert(result.stats.inuse_objects == 0);
		lp_result_meta_dispose(&result);
	}
	lp_runtime_delete(runtime);
	uint64_t elapsed = nanoseconds(end) - nanoseconds(begin);
	double per_pair = (double)elapsed / (double)ITERATIONS;
	printf("memory hook %-11s %7.2f ns/alloc-free pair\n", name,
		per_pair);
	return per_pair;
}

int
main(void) {
	(void)run_case("inactive", 0, false);
	(void)run_case("alloc-space", 1, false);
	(void)run_case("in-use", 1, true);
	return 0;
}
