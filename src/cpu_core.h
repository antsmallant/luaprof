#ifndef LUAPROF_CPU_CORE_H
#define LUAPROF_CPU_CORE_H

#include "luaprof/runtime.h"

typedef struct lp_cpu_profile lp_cpu_profile;

lp_cpu_profile *lp_cpu_profile_new(void);
void lp_cpu_profile_delete(lp_cpu_profile *profile);
void lp_cpu_profile_record(lp_cpu_profile *profile, lp_vm_state state,
	lp_lua_cfunction cfunction, const lp_stack_frame *frames, size_t depth,
	bool truncated, uint64_t weight);
void lp_cpu_profile_quality(lp_cpu_profile *profile, uint64_t dropped,
	uint64_t unstable, uint64_t profiler_overhead);
void lp_cpu_profile_merge_stats(const lp_cpu_profile *profile,
	lp_result_stats *stats);
size_t lp_cpu_profile_sample_count(const lp_cpu_profile *profile);
bool lp_cpu_profile_sample(const lp_cpu_profile *profile, size_t index,
	lp_cpu_sample_view *sample);
bool lp_cpu_profile_frame(const lp_cpu_profile *profile, size_t sample_index,
	size_t frame_index, lp_cpu_frame_view *frame);

#endif
