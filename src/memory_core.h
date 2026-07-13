#ifndef LUAPROF_MEMORY_CORE_H
#define LUAPROF_MEMORY_CORE_H

#include "luaprof/runtime.h"

typedef struct lp_memory_profile lp_memory_profile;

lp_memory_profile *lp_memory_profile_new(uint64_t sample_bytes,
	uint64_t seed, bool track_free);
void lp_memory_profile_delete(lp_memory_profile *profile);
void lp_memory_profile_finish(lp_memory_profile *profile);
void lp_memory_profile_allocation_event(lp_memory_profile *profile,
	void *old_pointer, void *new_pointer, size_t new_size, bool success);
bool lp_memory_profile_should_sample(lp_memory_profile *profile,
	size_t allocation_size, uint64_t *weighted_space,
	uint64_t *weighted_objects);
void lp_memory_profile_record(lp_memory_profile *profile,
	void *allocation_pointer, const lp_stack_frame *frames, size_t depth,
	bool truncated, size_t allocation_size, uint64_t weighted_space,
	uint64_t weighted_objects);
void lp_memory_profile_merge_stats(const lp_memory_profile *profile,
	lp_result_stats *stats);
size_t lp_memory_profile_sample_count(const lp_memory_profile *profile);
bool lp_memory_profile_sample(const lp_memory_profile *profile, size_t index,
	lp_memory_sample_view *sample);
bool lp_memory_profile_frame(const lp_memory_profile *profile,
	size_t sample_index, size_t frame_index, lp_memory_frame_view *frame);

uint64_t lp_memory_geometric_interval(uint64_t mean, uint64_t random_bits);
double lp_memory_sample_probability(uint64_t mean, size_t allocation_size);
uint64_t lp_memory_profile_bytes_until_sample(
	const lp_memory_profile *profile);
bool lp_memory_profile_tracks_live(const lp_memory_profile *profile);
size_t lp_memory_profile_live_capacity(const lp_memory_profile *profile);

#endif
