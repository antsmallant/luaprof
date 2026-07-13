#include "cpu_core.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
test_aggregate_bound(void) {
	lp_cpu_profile *profile = lp_cpu_profile_new();
	assert(profile != NULL);
	lp_stack_frame frame = {
		.kind = LP_FRAME_LUA,
		.function = profile,
		.source = "@aggregate_bound.lua",
		.source_length = sizeof("@aggregate_bound.lua") - 1,
		.linedefined = 1,
	};
	for (int line = 1; line <= 5000; ++line) {
		frame.currentline = line;
		lp_cpu_profile_record(profile, LP_VM_LUA, NULL, &frame, 1,
			false, 1);
	}
	lp_result_stats stats = { 0 };
	lp_cpu_profile_merge_stats(profile, &stats);
	assert(stats.samples == 5000);
	assert(stats.sample_weight == 5000);
	assert(stats.aggregate_overflows != 0);
	assert(lp_cpu_profile_sample_count(profile) < 5000);
	lp_cpu_profile_delete(profile);
}

static void
test_symbol_bound(void) {
	lp_cpu_profile *profile = lp_cpu_profile_new();
	assert(profile != NULL);
	lp_stack_frame frame = {
		.kind = LP_FRAME_LUA,
		.source = "@symbol_bound.lua",
		.source_length = sizeof("@symbol_bound.lua") - 1,
		.linedefined = 1,
		.currentline = 2,
	};
	for (uintptr_t identity = 1; identity <= 5000; ++identity) {
		frame.function = (const void *)identity;
		lp_cpu_profile_record(profile, LP_VM_LUA, NULL, &frame, 1,
			false, 1);
	}
	lp_result_stats stats = { 0 };
	lp_cpu_profile_merge_stats(profile, &stats);
	assert(stats.symbol_overflows != 0);
	assert(stats.aggregate_overflows != 0);
	lp_cpu_profile_delete(profile);
}

static void
test_source_and_stack_bound(void) {
	lp_cpu_profile *profile = lp_cpu_profile_new();
	assert(profile != NULL);
	char source[2048];
	memset(source, 'x', sizeof(source));
	lp_stack_frame frames[65];
	for (size_t i = 0; i < 65; ++i) {
		frames[i] = (lp_stack_frame) {
			.kind = LP_FRAME_LUA,
			.function = (const void *)(uintptr_t)(i + 1),
			.source = source,
			.source_length = sizeof(source),
			.linedefined = (int)i,
			.currentline = (int)i + 1,
		};
	}
	lp_cpu_profile_record(profile, LP_VM_LUA, NULL, frames, 65, false, 3);
	lp_result_stats stats = { 0 };
	lp_cpu_profile_merge_stats(profile, &stats);
	assert(stats.stack_truncations == 1);
	assert(stats.symbol_overflows != 0);
	lp_cpu_sample_view sample;
	assert(lp_cpu_profile_sample(profile, 0, &sample));
	assert(sample.depth == 64);
	assert(sample.weight == 3);
	lp_cpu_frame_view frame;
	assert(lp_cpu_profile_frame(profile, 0, 0, &frame));
	assert(frame.source != NULL);
	assert(frame.source_length == 1024);
	lp_cpu_profile_delete(profile);
}

int
main(void) {
	test_aggregate_bound();
	test_symbol_bound();
	test_source_and_stack_bound();
	puts("luaprof bounded CPU aggregation: ok");
	return EXIT_SUCCESS;
}
