#include "cpu_core.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define LP_CPU_MAX_STACK_DEPTH 64u
#define LP_CPU_SYMBOL_CAPACITY 4096u
#define LP_CPU_AGGREGATE_CAPACITY 2048u
#define LP_CPU_SOURCE_CAPACITY (256u * 1024u)
#define LP_CPU_MAX_SOURCE_LENGTH 1024u
#define LP_NO_SOURCE UINT32_MAX

typedef struct lp_compact_frame {
	uint32_t symbol;
	int32_t currentline;
} lp_compact_frame;

typedef struct lp_cpu_symbol {
	uint64_t hash;
	const void *function;
	lp_lua_cfunction cfunction;
	uint64_t source_hash;
	size_t original_source_length;
	uint32_t source_offset;
	uint16_t source_length;
	int linedefined;
	lp_frame_kind kind;
	bool used;
} lp_cpu_symbol;

typedef struct lp_cpu_aggregate {
	uint64_t hash;
	uint64_t weight;
	lp_lua_cfunction cfunction;
	uint16_t depth;
	lp_vm_state state;
	bool used;
	lp_compact_frame frames[LP_CPU_MAX_STACK_DEPTH];
} lp_cpu_aggregate;

struct lp_cpu_profile {
	lp_cpu_symbol *symbols;
	lp_cpu_aggregate *aggregates;
	char *sources;
	size_t source_used;
	size_t aggregate_count;
	bool sources_full;
	lp_result_stats stats;
};

static uint64_t
saturating_add(uint64_t value, uint64_t increment) {
	return UINT64_MAX - value < increment ? UINT64_MAX : value + increment;
}

static uint64_t
hash_bytes(uint64_t hash, const void *data, size_t size) {
	const unsigned char *bytes = data;
	for (size_t i = 0; i < size; ++i) {
		hash ^= bytes[i];
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

static uint64_t
frame_hash(const lp_stack_frame *frame, uint64_t source_hash) {
	uint64_t hash = UINT64_C(1469598103934665603);
	hash = hash_bytes(hash, &frame->kind, sizeof(frame->kind));
	hash = hash_bytes(hash, &frame->function, sizeof(frame->function));
	hash = hash_bytes(hash, &frame->cfunction, sizeof(frame->cfunction));
	hash = hash_bytes(hash, &frame->linedefined, sizeof(frame->linedefined));
	hash = hash_bytes(hash, &frame->source_length,
		sizeof(frame->source_length));
	return hash_bytes(hash, &source_hash, sizeof(source_hash));
}

static bool
symbol_matches(const lp_cpu_symbol *symbol, const lp_stack_frame *frame,
	uint64_t source_hash) {
	return symbol->kind == frame->kind &&
		symbol->function == frame->function &&
		symbol->cfunction == frame->cfunction &&
		symbol->linedefined == frame->linedefined &&
		symbol->original_source_length == frame->source_length &&
		symbol->source_hash == source_hash;
}

static uint32_t
intern_symbol(lp_cpu_profile *profile, const lp_stack_frame *frame) {
	uint64_t source_hash = UINT64_C(1469598103934665603);
	if (frame->source != NULL && frame->source_length != 0) {
		size_t hash_length = frame->source_length;
		if (hash_length > LP_CPU_MAX_SOURCE_LENGTH) {
			hash_length = LP_CPU_MAX_SOURCE_LENGTH;
		}
		source_hash = hash_bytes(source_hash, frame->source,
			hash_length);
	}
	uint64_t hash = frame_hash(frame, source_hash);
	for (size_t probe = 0; probe < LP_CPU_SYMBOL_CAPACITY; ++probe) {
		size_t index = (size_t)(hash + probe) &
			(LP_CPU_SYMBOL_CAPACITY - 1u);
		lp_cpu_symbol *symbol = &profile->symbols[index];
		if (symbol->used) {
			if (symbol->hash == hash &&
				symbol_matches(symbol, frame, source_hash)) {
				return (uint32_t)index + 1u;
			}
			continue;
		}

		symbol->used = true;
		symbol->hash = hash;
		symbol->function = frame->function;
		symbol->cfunction = frame->cfunction;
		symbol->source_hash = source_hash;
		symbol->original_source_length = frame->source_length;
		symbol->source_offset = LP_NO_SOURCE;
		symbol->linedefined = frame->linedefined;
		symbol->kind = frame->kind;
		if (frame->source != NULL && frame->source_length != 0 &&
			!profile->sources_full) {
			size_t copy_length = frame->source_length;
			if (copy_length > LP_CPU_MAX_SOURCE_LENGTH) {
				copy_length = LP_CPU_MAX_SOURCE_LENGTH;
				profile->stats.symbol_overflows = saturating_add(
					profile->stats.symbol_overflows, 1);
			}
			if (copy_length + 1 <= LP_CPU_SOURCE_CAPACITY -
				profile->source_used) {
				symbol->source_offset = (uint32_t)profile->source_used;
				symbol->source_length = (uint16_t)copy_length;
				memcpy(profile->sources + profile->source_used,
					frame->source, copy_length);
				profile->sources[profile->source_used + copy_length] = '\0';
				profile->source_used += copy_length + 1;
			}
			else {
				profile->sources_full = true;
				profile->stats.symbol_overflows = saturating_add(
					profile->stats.symbol_overflows, 1);
			}
		}
		return (uint32_t)index + 1u;
	}
	profile->stats.symbol_overflows = saturating_add(
		profile->stats.symbol_overflows, 1);
	return 0;
}

static bool
aggregate_matches(const lp_cpu_aggregate *aggregate, lp_vm_state state,
	lp_lua_cfunction cfunction, const lp_compact_frame *frames,
	size_t depth) {
	return aggregate->state == state && aggregate->cfunction == cfunction &&
		aggregate->depth == depth &&
		memcmp(aggregate->frames, frames, depth * sizeof(frames[0])) == 0;
}

lp_cpu_profile *
lp_cpu_profile_new(void) {
	lp_cpu_profile *profile = calloc(1, sizeof(*profile));
	if (profile == NULL) {
		return NULL;
	}
	profile->symbols = calloc(LP_CPU_SYMBOL_CAPACITY,
		sizeof(profile->symbols[0]));
	profile->aggregates = calloc(LP_CPU_AGGREGATE_CAPACITY,
		sizeof(profile->aggregates[0]));
	profile->sources = malloc(LP_CPU_SOURCE_CAPACITY);
	if (profile->symbols == NULL || profile->aggregates == NULL ||
		profile->sources == NULL) {
		lp_cpu_profile_delete(profile);
		return NULL;
	}
	return profile;
}

void
lp_cpu_profile_delete(lp_cpu_profile *profile) {
	if (profile == NULL) {
		return;
	}
	free(profile->symbols);
	free(profile->aggregates);
	free(profile->sources);
	free(profile);
}

void
lp_cpu_profile_record(lp_cpu_profile *profile, lp_vm_state state,
	lp_lua_cfunction cfunction, const lp_stack_frame *frames, size_t depth,
	bool truncated, uint64_t weight) {
	if (profile == NULL || weight == 0 || state < LP_VM_HOST ||
		state > LP_VM_GC) {
		return;
	}
	if (depth > LP_CPU_MAX_STACK_DEPTH) {
		depth = LP_CPU_MAX_STACK_DEPTH;
		truncated = true;
	}
	profile->stats.samples = saturating_add(profile->stats.samples, 1);
	profile->stats.sample_weight = saturating_add(
		profile->stats.sample_weight, weight);
	uint64_t *state_weight[] = {
		&profile->stats.sample_host,
		&profile->stats.sample_lua,
		&profile->stats.sample_c,
		&profile->stats.sample_gc,
	};
	*state_weight[state] = saturating_add(*state_weight[state], weight);
	if (truncated) {
		profile->stats.stack_truncations = saturating_add(
			profile->stats.stack_truncations, 1);
	}

	lp_compact_frame compact[LP_CPU_MAX_STACK_DEPTH];
	uint64_t hash = UINT64_C(1469598103934665603);
	hash = hash_bytes(hash, &state, sizeof(state));
	hash = hash_bytes(hash, &cfunction, sizeof(cfunction));
	for (size_t i = 0; i < depth; ++i) {
		compact[i].symbol = intern_symbol(profile, &frames[i]);
		compact[i].currentline = frames[i].currentline;
		hash = hash_bytes(hash, &compact[i], sizeof(compact[i]));
	}

	for (size_t probe = 0; probe < LP_CPU_AGGREGATE_CAPACITY; ++probe) {
		size_t index = (size_t)(hash + probe) &
			(LP_CPU_AGGREGATE_CAPACITY - 1u);
		lp_cpu_aggregate *aggregate = &profile->aggregates[index];
		if (aggregate->used) {
			if (aggregate->hash == hash && aggregate_matches(aggregate,
				state, cfunction, compact, depth)) {
				aggregate->weight = saturating_add(aggregate->weight,
					weight);
				return;
			}
			continue;
		}
		aggregate->used = true;
		aggregate->hash = hash;
		aggregate->weight = weight;
		aggregate->cfunction = cfunction;
		aggregate->depth = (uint16_t)depth;
		aggregate->state = state;
		memcpy(aggregate->frames, compact, depth * sizeof(compact[0]));
		profile->aggregate_count++;
		return;
	}
	profile->stats.aggregate_overflows = saturating_add(
		profile->stats.aggregate_overflows, weight);
}

void
lp_cpu_profile_quality(lp_cpu_profile *profile, uint64_t dropped,
	uint64_t unstable, uint64_t profiler_overhead) {
	if (profile == NULL) {
		return;
	}
	profile->stats.dropped_events = saturating_add(
		profile->stats.dropped_events, dropped);
	profile->stats.unstable_events = saturating_add(
		profile->stats.unstable_events, unstable);
	profile->stats.profiler_overhead_events = saturating_add(
		profile->stats.profiler_overhead_events, profiler_overhead);
}

void
lp_cpu_profile_merge_stats(const lp_cpu_profile *profile,
	lp_result_stats *stats) {
	if (profile == NULL || stats == NULL) {
		return;
	}
	stats->samples = profile->stats.samples;
	stats->sample_weight = profile->stats.sample_weight;
	stats->sample_host = profile->stats.sample_host;
	stats->sample_lua = profile->stats.sample_lua;
	stats->sample_c = profile->stats.sample_c;
	stats->sample_gc = profile->stats.sample_gc;
	stats->dropped_events = profile->stats.dropped_events;
	stats->unstable_events = profile->stats.unstable_events;
	stats->profiler_overhead_events =
		profile->stats.profiler_overhead_events;
	stats->stack_truncations = profile->stats.stack_truncations;
	stats->aggregate_overflows = profile->stats.aggregate_overflows;
	stats->symbol_overflows = profile->stats.symbol_overflows;
}

size_t
lp_cpu_profile_sample_count(const lp_cpu_profile *profile) {
	return profile == NULL ? 0 : profile->aggregate_count;
}

static const lp_cpu_aggregate *
aggregate_at(const lp_cpu_profile *profile, size_t wanted) {
	if (profile == NULL || wanted >= profile->aggregate_count) {
		return NULL;
	}
	for (size_t i = 0; i < LP_CPU_AGGREGATE_CAPACITY; ++i) {
		if (profile->aggregates[i].used && wanted-- == 0) {
			return &profile->aggregates[i];
		}
	}
	return NULL;
}

bool
lp_cpu_profile_sample(const lp_cpu_profile *profile, size_t index,
	lp_cpu_sample_view *sample) {
	const lp_cpu_aggregate *aggregate = aggregate_at(profile, index);
	if (aggregate == NULL || sample == NULL) {
		return false;
	}
	sample->state = aggregate->state;
	sample->cfunction = aggregate->cfunction;
	sample->weight = aggregate->weight;
	sample->depth = aggregate->depth;
	return true;
}

bool
lp_cpu_profile_frame(const lp_cpu_profile *profile, size_t sample_index,
	size_t frame_index, lp_cpu_frame_view *frame) {
	const lp_cpu_aggregate *aggregate = aggregate_at(profile, sample_index);
	if (aggregate == NULL || frame == NULL ||
		frame_index >= aggregate->depth) {
		return false;
	}
	uint32_t symbol_id = aggregate->frames[frame_index].symbol;
	memset(frame, 0, sizeof(*frame));
	frame->currentline = aggregate->frames[frame_index].currentline;
	if (symbol_id == 0 || symbol_id > LP_CPU_SYMBOL_CAPACITY) {
		return true;
	}
	const lp_cpu_symbol *symbol = &profile->symbols[symbol_id - 1u];
	frame->kind = symbol->kind;
	frame->function = symbol->function;
	frame->cfunction = symbol->cfunction;
	frame->linedefined = symbol->linedefined;
	if (symbol->source_offset != LP_NO_SOURCE) {
		frame->source = profile->sources + symbol->source_offset;
		frame->source_length = symbol->source_length;
	}
	return true;
}
