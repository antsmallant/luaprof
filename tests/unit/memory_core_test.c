#include "memory_core.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t
next_random(uint64_t *state) {
	uint64_t value = (*state += UINT64_C(0x9e3779b97f4a7c15));
	value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
	return value ^ (value >> 31);
}

static lp_stack_frame
test_frame(int line) {
	static const char source[] = "@memory_core_test.lua";
	lp_stack_frame frame = {
		.kind = LP_FRAME_LUA,
		.function = (const void *)(uintptr_t)1,
		.source = source,
		.source_length = sizeof(source) - 1,
		.linedefined = 1,
		.currentline = line,
	};
	return frame;
}

static void
test_interval_and_probability(void) {
	assert(lp_memory_geometric_interval(1, 0) == 1);
	assert(lp_memory_geometric_interval(1, UINT64_MAX) == 1);
	assert(lp_memory_geometric_interval(1000, UINT64_MAX) == 1);
	assert(lp_memory_geometric_interval(1000, 0) > 10000);

	uint64_t random = UINT64_C(0x123456789abcdef0);
	long double total = 0;
	for (size_t i = 0; i < 200000; ++i) {
		total += (long double)lp_memory_geometric_interval(1000,
			next_random(&random));
	}
	double average = (double)(total / 200000.0L);
	assert(average > 990.0 && average < 1010.0);

	double one_byte = lp_memory_sample_probability(1024, 1);
	assert(fabs(one_byte - 1.0 / 1024.0) < 1e-12);
	double one_mean = lp_memory_sample_probability(1024, 1024);
	assert(one_mean > 0.63 && one_mean < 0.64);
	assert(lp_memory_sample_probability(1024, SIZE_MAX) > 0.999999);
}

static void
test_budget_and_exact_mode(void) {
	lp_memory_profile *sampled = lp_memory_profile_new(100, 7);
	assert(sampled != NULL);
	uint64_t budget = lp_memory_profile_bytes_until_sample(sampled);
	uint64_t space = 0;
	uint64_t objects = 0;
	if (budget > 1) {
		assert(!lp_memory_profile_should_sample(sampled,
			(size_t)(budget - 1), &space, &objects));
		assert(lp_memory_profile_bytes_until_sample(sampled) == 1);
	}
	assert(lp_memory_profile_should_sample(sampled, 1, &space, &objects));
	assert(space >= 100);
	assert(objects >= 100);
	lp_memory_profile_delete(sampled);

	lp_memory_profile *exact = lp_memory_profile_new(1, 11);
	assert(exact != NULL);
	lp_stack_frame frame = test_frame(10);
	const size_t sizes[] = { 10, 20, 30 };
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
		assert(lp_memory_profile_should_sample(exact, sizes[i], &space,
			&objects));
		assert(space == sizes[i]);
		assert(objects == 1);
		lp_memory_profile_record(exact, &frame, 1, false, sizes[i],
			space, objects);
	}
	lp_result_stats stats = { 0 };
	lp_memory_profile_merge_stats(exact, &stats);
	assert(stats.memory_samples == 3);
	assert(stats.sampled_alloc_bytes == 60);
	assert(stats.alloc_space == 60);
	assert(stats.alloc_objects == 3);
	assert(lp_memory_profile_sample_count(exact) == 1);
	lp_memory_sample_view sample;
	assert(lp_memory_profile_sample(exact, 0, &sample));
	assert(sample.alloc_space == 60);
	assert(sample.alloc_objects == 3);
	assert(sample.sampled_bytes == 60);
	assert(sample.sample_count == 3);
	assert(sample.depth == 1);
	lp_memory_frame_view view;
	assert(lp_memory_profile_frame(exact, 0, 0, &view));
	assert(view.currentline == 10);
	assert(view.source_length == sizeof("@memory_core_test.lua") - 1);
	lp_memory_profile_delete(exact);
}

static void
test_weighted_convergence(void) {
	lp_memory_profile *profile = lp_memory_profile_new(1024, 19);
	assert(profile != NULL);
	lp_stack_frame small = test_frame(1);
	lp_stack_frame large = test_frame(2);
	for (size_t i = 0; i < 100000; ++i) {
		uint64_t space;
		uint64_t objects;
		if (lp_memory_profile_should_sample(profile, 64, &space,
			&objects)) {
			lp_memory_profile_record(profile, &small, 1, false, 64,
				space, objects);
		}
		if (lp_memory_profile_should_sample(profile, 256, &space,
			&objects)) {
			lp_memory_profile_record(profile, &large, 1, false, 256,
				space, objects);
		}
	}

	uint64_t spaces[2] = { 0, 0 };
	uint64_t objects[2] = { 0, 0 };
	for (size_t i = 0; i < lp_memory_profile_sample_count(profile); ++i) {
		lp_memory_sample_view sample;
		lp_memory_frame_view frame;
		assert(lp_memory_profile_sample(profile, i, &sample));
		assert(lp_memory_profile_frame(profile, i, 0, &frame));
		size_t index = frame.currentline == 1 ? 0 : 1;
		spaces[index] += sample.alloc_space;
		objects[index] += sample.alloc_objects;
	}
	double space_ratio = (double)spaces[1] / (double)spaces[0];
	double object_ratio = (double)objects[1] / (double)objects[0];
	assert(space_ratio > 3.8 && space_ratio < 4.2);
	assert(object_ratio > 0.95 && object_ratio < 1.05);
	lp_memory_profile_delete(profile);
}

static void
test_bounds_and_large_allocation(void) {
	lp_memory_profile *profile = lp_memory_profile_new(1, 23);
	assert(profile != NULL);
	for (int line = 0; line < 2049; ++line) {
		lp_stack_frame frame = test_frame(line);
		uint64_t space;
		uint64_t objects;
		assert(lp_memory_profile_should_sample(profile, 1, &space,
			&objects));
		lp_memory_profile_record(profile, &frame, 1, false, 1, space,
			objects);
	}
	lp_result_stats stats = { 0 };
	lp_memory_profile_merge_stats(profile, &stats);
	assert(stats.memory_samples == 2049);
	assert(stats.aggregate_overflows == 1);
	lp_memory_profile_delete(profile);

	profile = lp_memory_profile_new(1024, 29);
	assert(profile != NULL);
	uint64_t space;
	uint64_t objects;
	assert(lp_memory_profile_should_sample(profile, UINT32_MAX, &space,
		&objects));
	assert(space >= UINT32_MAX);
	assert(objects == 1);
	lp_stack_frame frame = test_frame(3);
	lp_memory_profile_record(profile, &frame, 1, false, UINT32_MAX,
		space, objects);
	lp_memory_profile_merge_stats(profile, &stats);
	assert(stats.memory_samples == 1);
	lp_memory_profile_delete(profile);
}

int
main(void) {
	test_interval_and_probability();
	test_budget_and_exact_mode();
	test_weighted_convergence();
	test_bounds_and_large_allocation();
	puts("luaprof alloc-space memory core: ok");
	return EXIT_SUCCESS;
}
