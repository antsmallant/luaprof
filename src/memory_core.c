#include "memory_core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LP_MEMORY_MAX_STACK_DEPTH 64u
#define LP_MEMORY_SYMBOL_CAPACITY 4096u
#define LP_MEMORY_AGGREGATE_CAPACITY 2048u
#define LP_MEMORY_SOURCE_CAPACITY (256u * 1024u)
#define LP_MEMORY_MAX_SOURCE_LENGTH 1024u
#define LP_MEMORY_LIVE_CAPACITY 16384u
#define LP_MEMORY_LIVE_BUCKET_CAPACITY 32768u
#define LP_NO_SOURCE UINT32_MAX
#define LP_NO_LIVE_ENTRY UINT32_MAX

typedef struct lp_compact_frame {
	uint32_t symbol;
	int32_t currentline;
} lp_compact_frame;

typedef struct lp_memory_symbol {
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
} lp_memory_symbol;

typedef struct lp_memory_aggregate {
	uint64_t hash;
	uint64_t alloc_space;
	uint64_t alloc_objects;
	uint64_t inuse_space;
	uint64_t inuse_objects;
	uint64_t sampled_bytes;
	uint64_t sample_count;
	uint16_t depth;
	bool used;
	lp_compact_frame frames[LP_MEMORY_MAX_STACK_DEPTH];
} lp_memory_aggregate;

typedef struct lp_memory_live_entry {
	void *pointer;
	uint64_t weighted_space;
	uint64_t weighted_objects;
	uint32_t aggregate_index;
	uint32_t next;
} lp_memory_live_entry;

struct lp_memory_profile {
	lp_memory_symbol *symbols;
	lp_memory_aggregate *aggregates;
	lp_memory_live_entry *live_entries;
	uint32_t *live_buckets;
	char *sources;
	uint64_t sample_bytes;
	uint64_t random_state;
	uint64_t weight_random_state;
	uint64_t bytes_until_sample;
	size_t source_used;
	size_t aggregate_count;
	size_t live_count;
	uint32_t live_free_head;
	bool sources_full;
	lp_result_stats stats;
};

static uint64_t
saturating_add(uint64_t value, uint64_t increment) {
	return UINT64_MAX - value < increment ? UINT64_MAX : value + increment;
}

static uint64_t
saturating_subtract(uint64_t value, uint64_t decrement) {
	return value < decrement ? 0 : value - decrement;
}

static size_t
pointer_bucket(const void *pointer) {
	uint64_t value = (uint64_t)(uintptr_t)pointer;
	value ^= value >> 30;
	value *= UINT64_C(0xbf58476d1ce4e5b9);
	value ^= value >> 27;
	value *= UINT64_C(0x94d049bb133111eb);
	value ^= value >> 31;
	return (size_t)value & (LP_MEMORY_LIVE_BUCKET_CAPACITY - 1u);
}

static void
subtract_live_weight(lp_memory_profile *profile,
	const lp_memory_live_entry *entry) {
	if (entry->aggregate_index < LP_MEMORY_AGGREGATE_CAPACITY) {
		lp_memory_aggregate *aggregate =
			&profile->aggregates[entry->aggregate_index];
		aggregate->inuse_space = saturating_subtract(
			aggregate->inuse_space, entry->weighted_space);
		aggregate->inuse_objects = saturating_subtract(
			aggregate->inuse_objects, entry->weighted_objects);
	}
	profile->stats.inuse_space = saturating_subtract(
		profile->stats.inuse_space, entry->weighted_space);
	profile->stats.inuse_objects = saturating_subtract(
		profile->stats.inuse_objects, entry->weighted_objects);
}

static void
remove_live(lp_memory_profile *profile, void *pointer) {
	if (profile->live_buckets == NULL || pointer == NULL) {
		return;
	}
	size_t bucket = pointer_bucket(pointer);
	uint32_t *link = &profile->live_buckets[bucket];
	while (*link != LP_NO_LIVE_ENTRY) {
		uint32_t index = *link;
		lp_memory_live_entry *entry = &profile->live_entries[index];
		if (entry->pointer == pointer) {
			subtract_live_weight(profile, entry);
			*link = entry->next;
			entry->pointer = NULL;
			entry->next = profile->live_free_head;
			profile->live_free_head = index;
			profile->live_count--;
			return;
		}
		link = &entry->next;
	}
}

static void
add_live(lp_memory_profile *profile, void *pointer,
	uint32_t aggregate_index, uint64_t weighted_space,
	uint64_t weighted_objects) {
	if (profile->live_buckets == NULL || pointer == NULL) {
		return;
	}
	size_t bucket = pointer_bucket(pointer);
	uint32_t index = profile->live_buckets[bucket];
	while (index != LP_NO_LIVE_ENTRY) {
		lp_memory_live_entry *entry = &profile->live_entries[index];
		if (entry->pointer == pointer) {
			subtract_live_weight(profile, entry);
			entry->weighted_space = weighted_space;
			entry->weighted_objects = weighted_objects;
			entry->aggregate_index = aggregate_index;
			goto add_weight;
		}
		index = entry->next;
	}
	if (profile->live_free_head == LP_NO_LIVE_ENTRY) {
		profile->stats.live_map_overflows = saturating_add(
			profile->stats.live_map_overflows, 1);
		return;
	}
	index = profile->live_free_head;
	lp_memory_live_entry *entry = &profile->live_entries[index];
	profile->live_free_head = entry->next;
	entry->pointer = pointer;
	entry->weighted_space = weighted_space;
	entry->weighted_objects = weighted_objects;
	entry->aggregate_index = aggregate_index;
	entry->next = profile->live_buckets[bucket];
	profile->live_buckets[bucket] = index;
	profile->live_count++;

add_weight:
	profile->aggregates[aggregate_index].inuse_space = saturating_add(
		profile->aggregates[aggregate_index].inuse_space, weighted_space);
	profile->aggregates[aggregate_index].inuse_objects = saturating_add(
		profile->aggregates[aggregate_index].inuse_objects,
		weighted_objects);
	profile->stats.inuse_space = saturating_add(profile->stats.inuse_space,
		weighted_space);
	profile->stats.inuse_objects = saturating_add(
		profile->stats.inuse_objects, weighted_objects);
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
next_random(lp_memory_profile *profile) {
	uint64_t value = (profile->random_state +=
		UINT64_C(0x9e3779b97f4a7c15));
	value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
	value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
	return value ^ (value >> 31);
}

uint64_t
lp_memory_geometric_interval(uint64_t mean, uint64_t random_bits) {
	if (mean <= 1) {
		return 1;
	}
	const double units = 9007199254740993.0;
	double uniform = ((double)(random_bits >> 11) + 1.0) / units;
	double log_survival = log1p(-1.0 / (double)mean);
	double interval = floor(log(uniform) / log_survival) + 1.0;
	if (interval >= (double)UINT64_MAX) {
		return UINT64_MAX;
	}
	if (interval <= 1.0) {
		return 1;
	}
	return (uint64_t)interval;
}

double
lp_memory_sample_probability(uint64_t mean, size_t allocation_size) {
	if (allocation_size == 0) {
		return 0.0;
	}
	if (mean <= 1) {
		return 1.0;
	}
	double exponent = (double)allocation_size *
		log1p(-1.0 / (double)mean);
	double probability = -expm1(exponent);
	if (probability > 1.0) {
		return 1.0;
	}
	return probability;
}

static uint64_t
randomized_weight(lp_memory_profile *profile, double value) {
	if (value >= (double)UINT64_MAX) {
		return UINT64_MAX;
	}
	if (value <= 1.0) {
		return 1;
	}
	uint64_t integral = (uint64_t)value;
	double fraction = value - (double)integral;
	uint64_t random = (profile->weight_random_state +=
		UINT64_C(0x9e3779b97f4a7c15));
	random = (random ^ (random >> 30)) *
		UINT64_C(0xbf58476d1ce4e5b9);
	random = (random ^ (random >> 27)) *
		UINT64_C(0x94d049bb133111eb);
	random ^= random >> 31;
	double uniform = (double)(random >> 11) / 9007199254740992.0;
	return integral + (uniform < fraction ? 1u : 0u);
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
symbol_matches(const lp_memory_symbol *symbol, const lp_stack_frame *frame,
	uint64_t source_hash) {
	return symbol->kind == frame->kind &&
		symbol->function == frame->function &&
		symbol->cfunction == frame->cfunction &&
		symbol->linedefined == frame->linedefined &&
		symbol->original_source_length == frame->source_length &&
		symbol->source_hash == source_hash;
}

static uint32_t
intern_symbol(lp_memory_profile *profile, const lp_stack_frame *frame) {
	uint64_t source_hash = UINT64_C(1469598103934665603);
	if (frame->source != NULL && frame->source_length != 0) {
		size_t hash_length = frame->source_length;
		if (hash_length > LP_MEMORY_MAX_SOURCE_LENGTH) {
			hash_length = LP_MEMORY_MAX_SOURCE_LENGTH;
		}
		source_hash = hash_bytes(source_hash, frame->source, hash_length);
	}
	uint64_t hash = frame_hash(frame, source_hash);
	for (size_t probe = 0; probe < LP_MEMORY_SYMBOL_CAPACITY; ++probe) {
		size_t index = (size_t)(hash + probe) &
			(LP_MEMORY_SYMBOL_CAPACITY - 1u);
		lp_memory_symbol *symbol = &profile->symbols[index];
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
			if (copy_length > LP_MEMORY_MAX_SOURCE_LENGTH) {
				copy_length = LP_MEMORY_MAX_SOURCE_LENGTH;
				profile->stats.symbol_overflows = saturating_add(
					profile->stats.symbol_overflows, 1);
			}
			if (copy_length + 1 <= LP_MEMORY_SOURCE_CAPACITY -
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
aggregate_matches(const lp_memory_aggregate *aggregate,
	const lp_compact_frame *frames, size_t depth) {
	return aggregate->depth == depth &&
		memcmp(aggregate->frames, frames, depth * sizeof(frames[0])) == 0;
}

lp_memory_profile *
lp_memory_profile_new(uint64_t sample_bytes, uint64_t seed,
	bool track_free) {
	if (sample_bytes == 0) {
		return NULL;
	}
	lp_memory_profile *profile = calloc(1, sizeof(*profile));
	if (profile == NULL) {
		return NULL;
	}
	profile->symbols = calloc(LP_MEMORY_SYMBOL_CAPACITY,
		sizeof(profile->symbols[0]));
	profile->aggregates = calloc(LP_MEMORY_AGGREGATE_CAPACITY,
		sizeof(profile->aggregates[0]));
	profile->sources = malloc(LP_MEMORY_SOURCE_CAPACITY);
	if (track_free) {
		profile->live_entries = calloc(LP_MEMORY_LIVE_CAPACITY,
			sizeof(profile->live_entries[0]));
		profile->live_buckets = malloc(LP_MEMORY_LIVE_BUCKET_CAPACITY *
			sizeof(profile->live_buckets[0]));
	}
	if (profile->symbols == NULL || profile->aggregates == NULL ||
		profile->sources == NULL || (track_free &&
			(profile->live_entries == NULL ||
			profile->live_buckets == NULL))) {
		lp_memory_profile_delete(profile);
		return NULL;
	}
	profile->live_free_head = LP_NO_LIVE_ENTRY;
	if (track_free) {
		for (uint32_t i = 0; i < LP_MEMORY_LIVE_BUCKET_CAPACITY; ++i) {
			profile->live_buckets[i] = LP_NO_LIVE_ENTRY;
		}
		for (uint32_t i = LP_MEMORY_LIVE_CAPACITY; i-- != 0;) {
			profile->live_entries[i].next = profile->live_free_head;
			profile->live_free_head = i;
		}
	}
	profile->sample_bytes = sample_bytes;
	profile->random_state = seed;
	profile->weight_random_state = seed ^ UINT64_C(0xd1b54a32d192ed03);
	profile->bytes_until_sample = lp_memory_geometric_interval(sample_bytes,
		next_random(profile));
	return profile;
}

void
lp_memory_profile_delete(lp_memory_profile *profile) {
	if (profile == NULL) {
		return;
	}
	free(profile->symbols);
	free(profile->aggregates);
	free(profile->live_entries);
	free(profile->live_buckets);
	free(profile->sources);
	free(profile);
}

void
lp_memory_profile_finish(lp_memory_profile *profile) {
	if (profile == NULL) {
		return;
	}
	free(profile->live_entries);
	free(profile->live_buckets);
	profile->live_entries = NULL;
	profile->live_buckets = NULL;
	profile->live_count = 0;
	profile->live_free_head = LP_NO_LIVE_ENTRY;
}

void
lp_memory_profile_allocation_event(lp_memory_profile *profile,
	void *old_pointer, void *new_pointer, size_t new_size, bool success) {
	if (profile == NULL || profile->live_buckets == NULL || !success ||
		old_pointer == NULL) {
		return;
	}
	if ((new_pointer == NULL && new_size == 0) ||
		(new_pointer != NULL && new_size != 0)) {
		remove_live(profile, old_pointer);
	}
}

bool
lp_memory_profile_should_sample(lp_memory_profile *profile,
	size_t allocation_size, uint64_t *weighted_space,
	uint64_t *weighted_objects) {
	if (profile == NULL || allocation_size == 0) {
		return false;
	}
	if ((uint64_t)allocation_size < profile->bytes_until_sample) {
		profile->bytes_until_sample -= (uint64_t)allocation_size;
		return false;
	}
	profile->bytes_until_sample = lp_memory_geometric_interval(
		profile->sample_bytes, next_random(profile));
	double probability = lp_memory_sample_probability(profile->sample_bytes,
		allocation_size);
	if (weighted_space != NULL) {
		*weighted_space = randomized_weight(profile,
			(double)allocation_size /
			probability);
	}
	if (weighted_objects != NULL) {
		*weighted_objects = randomized_weight(profile, 1.0 / probability);
	}
	return true;
}

void
lp_memory_profile_record(lp_memory_profile *profile,
	void *allocation_pointer, const lp_stack_frame *frames, size_t depth,
	bool truncated, size_t allocation_size, uint64_t weighted_space,
	uint64_t weighted_objects) {
	if (profile == NULL || allocation_size == 0 || weighted_space == 0 ||
		weighted_objects == 0 || (frames == NULL && depth != 0)) {
		return;
	}
	if (depth > LP_MEMORY_MAX_STACK_DEPTH) {
		depth = LP_MEMORY_MAX_STACK_DEPTH;
		truncated = true;
	}
	profile->stats.memory_samples = saturating_add(
		profile->stats.memory_samples, 1);
	profile->stats.sampled_alloc_bytes = saturating_add(
		profile->stats.sampled_alloc_bytes, allocation_size);
	profile->stats.alloc_space = saturating_add(
		profile->stats.alloc_space, weighted_space);
	profile->stats.alloc_objects = saturating_add(
		profile->stats.alloc_objects, weighted_objects);
	if (truncated) {
		profile->stats.stack_truncations = saturating_add(
			profile->stats.stack_truncations, 1);
	}

	lp_compact_frame compact[LP_MEMORY_MAX_STACK_DEPTH];
	uint64_t hash = UINT64_C(1469598103934665603);
	for (size_t i = 0; i < depth; ++i) {
		compact[i].symbol = intern_symbol(profile, &frames[i]);
		compact[i].currentline = frames[i].currentline;
		hash = hash_bytes(hash, &compact[i], sizeof(compact[i]));
	}
	for (size_t probe = 0; probe < LP_MEMORY_AGGREGATE_CAPACITY; ++probe) {
		size_t index = (size_t)(hash + probe) &
			(LP_MEMORY_AGGREGATE_CAPACITY - 1u);
		lp_memory_aggregate *aggregate = &profile->aggregates[index];
		if (aggregate->used) {
			if (aggregate->hash == hash &&
				aggregate_matches(aggregate, compact, depth)) {
				aggregate->alloc_space = saturating_add(
					aggregate->alloc_space, weighted_space);
				aggregate->alloc_objects = saturating_add(
					aggregate->alloc_objects, weighted_objects);
				aggregate->sampled_bytes = saturating_add(
					aggregate->sampled_bytes, allocation_size);
				aggregate->sample_count = saturating_add(
					aggregate->sample_count, 1);
				add_live(profile, allocation_pointer, (uint32_t)index,
					weighted_space, weighted_objects);
				return;
			}
			continue;
		}
		aggregate->used = true;
		aggregate->hash = hash;
		aggregate->alloc_space = weighted_space;
		aggregate->alloc_objects = weighted_objects;
		aggregate->sampled_bytes = allocation_size;
		aggregate->sample_count = 1;
		aggregate->depth = (uint16_t)depth;
		memcpy(aggregate->frames, compact, depth * sizeof(compact[0]));
		profile->aggregate_count++;
		add_live(profile, allocation_pointer, (uint32_t)index,
			weighted_space, weighted_objects);
		return;
	}
	profile->stats.aggregate_overflows = saturating_add(
		profile->stats.aggregate_overflows, 1);
}

void
lp_memory_profile_merge_stats(const lp_memory_profile *profile,
	lp_result_stats *stats) {
	if (profile == NULL || stats == NULL) {
		return;
	}
	stats->memory_samples = profile->stats.memory_samples;
	stats->sampled_alloc_bytes = profile->stats.sampled_alloc_bytes;
	stats->alloc_space = profile->stats.alloc_space;
	stats->alloc_objects = profile->stats.alloc_objects;
	stats->inuse_space = profile->stats.inuse_space;
	stats->inuse_objects = profile->stats.inuse_objects;
	stats->live_map_overflows = profile->stats.live_map_overflows;
	stats->stack_truncations = profile->stats.stack_truncations;
	stats->aggregate_overflows = profile->stats.aggregate_overflows;
	stats->symbol_overflows = profile->stats.symbol_overflows;
}

size_t
lp_memory_profile_sample_count(const lp_memory_profile *profile) {
	return profile == NULL ? 0 : profile->aggregate_count;
}

static const lp_memory_aggregate *
aggregate_at(const lp_memory_profile *profile, size_t wanted) {
	if (profile == NULL || wanted >= profile->aggregate_count) {
		return NULL;
	}
	for (size_t i = 0; i < LP_MEMORY_AGGREGATE_CAPACITY; ++i) {
		if (profile->aggregates[i].used && wanted-- == 0) {
			return &profile->aggregates[i];
		}
	}
	return NULL;
}

bool
lp_memory_profile_sample(const lp_memory_profile *profile, size_t index,
	lp_memory_sample_view *sample) {
	if (sample == NULL) {
		return false;
	}
	const lp_memory_aggregate *aggregate = aggregate_at(profile, index);
	if (aggregate == NULL) {
		return false;
	}
	sample->alloc_space = aggregate->alloc_space;
	sample->alloc_objects = aggregate->alloc_objects;
	sample->inuse_space = aggregate->inuse_space;
	sample->inuse_objects = aggregate->inuse_objects;
	sample->sampled_bytes = aggregate->sampled_bytes;
	sample->sample_count = aggregate->sample_count;
	sample->depth = aggregate->depth;
	return true;
}

bool
lp_memory_profile_frame(const lp_memory_profile *profile,
	size_t sample_index, size_t frame_index, lp_memory_frame_view *frame) {
	if (frame == NULL) {
		return false;
	}
	const lp_memory_aggregate *aggregate = aggregate_at(profile,
		sample_index);
	if (aggregate == NULL || frame_index >= aggregate->depth) {
		return false;
	}
	const lp_compact_frame *compact = &aggregate->frames[frame_index];
	if (compact->symbol == 0 || compact->symbol > LP_MEMORY_SYMBOL_CAPACITY) {
		return false;
	}
	const lp_memory_symbol *symbol =
		&profile->symbols[compact->symbol - 1u];
	if (!symbol->used) {
		return false;
	}
	memset(frame, 0, sizeof(*frame));
	frame->kind = symbol->kind;
	frame->function = symbol->function;
	frame->cfunction = symbol->cfunction;
	frame->linedefined = symbol->linedefined;
	frame->currentline = compact->currentline;
	if (symbol->source_offset != LP_NO_SOURCE) {
		frame->source = profile->sources + symbol->source_offset;
		frame->source_length = symbol->source_length;
	}
	return true;
}

uint64_t
lp_memory_profile_bytes_until_sample(const lp_memory_profile *profile) {
	return profile == NULL ? 0 : profile->bytes_until_sample;
}

bool
lp_memory_profile_tracks_live(const lp_memory_profile *profile) {
	return profile != NULL && profile->live_buckets != NULL;
}

size_t
lp_memory_profile_live_capacity(const lp_memory_profile *profile) {
	return lp_memory_profile_tracks_live(profile)
		? LP_MEMORY_LIVE_CAPACITY : 0;
}
