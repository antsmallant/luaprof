#include "pprof_exporter.h"
#include "native_symbol.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#define LP_EXPORT_STACK_DEPTH 65u
#define LP_EXPORT_FUNCTION_HASH_CAPACITY 16384u
#define LP_EXPORT_LOCATION_HASH_CAPACITY 262144u

typedef struct lp_buffer {
	unsigned char *data;
	size_t size;
	size_t capacity;
	bool failed;
} lp_buffer;

typedef struct lp_export_string {
	char *data;
	size_t length;
} lp_export_string;

typedef struct lp_frame_desc {
	lp_frame_kind kind;
	const void *function;
	lp_lua_cfunction cfunction;
	const char *source;
	size_t source_length;
	const char *name;
	size_t name_length;
	int linedefined;
	int currentline;
	const char *synthetic;
} lp_frame_desc;

typedef struct lp_export_function {
	uint64_t hash;
	uint64_t id;
	lp_frame_kind kind;
	const void *function;
	lp_lua_cfunction cfunction;
	const char *source;
	size_t source_length;
	int linedefined;
	const char *synthetic;
	int64_t name;
	int64_t filename;
	uint32_t mapping_index;
} lp_export_function;

typedef struct lp_export_mapping {
	uint64_t id;
	uint64_t start;
	uint64_t limit;
	uint64_t offset;
	int64_t filename;
	bool has_functions;
	bool has_filenames;
	bool has_line_numbers;
} lp_export_mapping;

typedef struct lp_export_location {
	uint64_t id;
	uint32_t function_index;
	uint32_t mapping_index;
	int line;
	uint64_t address;
} lp_export_location;

typedef struct lp_export_sample {
	uint64_t locations[LP_EXPORT_STACK_DEPTH];
	size_t depth;
	int64_t values[4];
} lp_export_sample;

typedef struct lp_export_model {
	lp_export_string *strings;
	size_t string_count;
	size_t string_capacity;
	lp_export_function *functions;
	size_t function_count;
	size_t function_capacity;
	uint32_t *function_hash;
	lp_export_location *locations;
	size_t location_count;
	size_t location_capacity;
	uint32_t *location_hash;
	lp_export_sample *samples;
	size_t sample_count;
	size_t sample_capacity;
	lp_export_mapping *mappings;
	size_t mapping_count;
	size_t mapping_capacity;
	const lp_export_symbols *symbols;
	const char *sample_names[4];
	const char *sample_units[4];
	int64_t sample_name_indices[4];
	size_t value_count;
	size_t default_value;
	int64_t period_type;
	int64_t period_unit;
	int64_t period;
	int64_t comment;
	bool failed;
	char failure[160];
} lp_export_model;

static void
set_error(char *error, size_t capacity, const char *format, ...) {
	if (error == NULL || capacity == 0) {
		return;
	}
	va_list arguments;
	va_start(arguments, format);
	(void)vsnprintf(error, capacity, format, arguments);
	va_end(arguments);
}

static void
model_fail(lp_export_model *model, const char *message) {
	if (!model->failed) {
		model->failed = true;
		(void)snprintf(model->failure, sizeof(model->failure), "%s",
			message);
	}
}

static bool
grow_array(void **data, size_t *capacity, size_t wanted, size_t item_size) {
	if (wanted <= *capacity) {
		return true;
	}
	size_t next = *capacity == 0 ? 16 : *capacity;
	while (next < wanted) {
		if (next > SIZE_MAX / 2) {
			return false;
		}
		next *= 2;
	}
	if (next > SIZE_MAX / item_size) {
		return false;
	}
	void *replacement = realloc(*data, next * item_size);
	if (replacement == NULL) {
		return false;
	}
	*data = replacement;
	*capacity = next;
	return true;
}

static bool
buffer_reserve(lp_buffer *buffer, size_t additional) {
	if (buffer->failed) {
		return false;
	}
	if (additional > SIZE_MAX - buffer->size) {
		buffer->failed = true;
		return false;
	}
	size_t wanted = buffer->size + additional;
	if (!grow_array((void **)&buffer->data, &buffer->capacity, wanted, 1)) {
		buffer->failed = true;
		return false;
	}
	return true;
}

static void
buffer_append(lp_buffer *buffer, const void *data, size_t size) {
	if (size == 0) {
		return;
	}
	if (!buffer_reserve(buffer, size)) {
		return;
	}
	memcpy(buffer->data + buffer->size, data, size);
	buffer->size += size;
}

static void
buffer_varint(lp_buffer *buffer, uint64_t value) {
	unsigned char encoded[10];
	size_t count = 0;
	do {
		encoded[count] = (unsigned char)(value & 0x7fu);
		value >>= 7;
		if (value != 0) {
			encoded[count] |= 0x80u;
		}
		count++;
	} while (value != 0);
	buffer_append(buffer, encoded, count);
}

static void
buffer_key(lp_buffer *buffer, unsigned int field, unsigned int wire) {
	buffer_varint(buffer, ((uint64_t)field << 3) | wire);
}

static void
buffer_field_varint(lp_buffer *buffer, unsigned int field, uint64_t value) {
	buffer_key(buffer, field, 0);
	buffer_varint(buffer, value);
}

static void
buffer_field_bytes(lp_buffer *buffer, unsigned int field, const void *data,
	size_t size) {
	buffer_key(buffer, field, 2);
	buffer_varint(buffer, size);
	buffer_append(buffer, data, size);
}

static void
buffer_field_message(lp_buffer *buffer, unsigned int field,
	const lp_buffer *message) {
	buffer_field_bytes(buffer, field, message->data, message->size);
}

static void
buffer_dispose(lp_buffer *buffer) {
	free(buffer->data);
	memset(buffer, 0, sizeof(*buffer));
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

static int64_t
add_string(lp_export_model *model, const char *value, size_t length) {
	if (model->failed || !grow_array((void **)&model->strings,
		&model->string_capacity, model->string_count + 1,
		sizeof(model->strings[0]))) {
		model_fail(model, "out of memory while building string table");
		return -1;
	}
	char *copy = malloc(length + 1);
	if (copy == NULL) {
		model_fail(model, "out of memory while copying profile string");
		return -1;
	}
	memcpy(copy, value, length);
	copy[length] = '\0';
	model->strings[model->string_count] = (lp_export_string) {
		.data = copy,
		.length = length,
	};
	return (int64_t)model->string_count++;
}

static int64_t
add_literal(lp_export_model *model, const char *value) {
	return add_string(model, value, strlen(value));
}

static uint32_t
intern_mapping(lp_export_model *model, uint64_t start, uint64_t limit,
	uint64_t offset, const char *filename, size_t filename_length,
	bool has_functions, bool has_filenames, bool has_line_numbers) {
	for (size_t i = 0; i < model->mapping_count; ++i) {
		lp_export_mapping *mapping = &model->mappings[i];
		const lp_export_string *stored = &model->strings[mapping->filename];
		if (mapping->start == start && mapping->limit == limit &&
			mapping->offset == offset && stored->length == filename_length &&
			memcmp(stored->data, filename, filename_length) == 0) {
			mapping->has_functions |= has_functions;
			mapping->has_filenames |= has_filenames;
			mapping->has_line_numbers |= has_line_numbers;
			return (uint32_t)i;
		}
	}
	if (!grow_array((void **)&model->mappings, &model->mapping_capacity,
		model->mapping_count + 1, sizeof(model->mappings[0]))) {
		model_fail(model, "out of memory while building mappings");
		return UINT32_MAX;
	}
	int64_t file_index = add_string(model, filename, filename_length);
	if (file_index < 0) {
		return UINT32_MAX;
	}
	uint32_t index = (uint32_t)model->mapping_count++;
	model->mappings[index] = (lp_export_mapping) {
		.id = (uint64_t)index + 1u,
		.start = start,
		.limit = limit,
		.offset = offset,
		.filename = file_index,
		.has_functions = has_functions,
		.has_filenames = has_filenames,
		.has_line_numbers = has_line_numbers,
	};
	return index;
}

static uint64_t
frame_hash(const lp_frame_desc *frame) {
	uint64_t hash = UINT64_C(1469598103934665603);
	hash = hash_bytes(hash, &frame->kind, sizeof(frame->kind));
	hash = hash_bytes(hash, &frame->function, sizeof(frame->function));
	hash = hash_bytes(hash, &frame->cfunction, sizeof(frame->cfunction));
	hash = hash_bytes(hash, &frame->linedefined, sizeof(frame->linedefined));
	if (frame->synthetic != NULL) {
		hash = hash_bytes(hash, frame->synthetic, strlen(frame->synthetic));
	}
	else if (frame->source != NULL) {
		hash = hash_bytes(hash, frame->source, frame->source_length);
	}
	return hash;
}

static bool
function_matches(const lp_export_function *function,
	const lp_frame_desc *frame, uint64_t hash) {
	if (function->hash != hash || function->kind != frame->kind ||
		function->function != frame->function ||
		function->cfunction != frame->cfunction ||
		function->linedefined != frame->linedefined ||
		function->source_length != frame->source_length) {
		return false;
	}
	if (function->synthetic != NULL || frame->synthetic != NULL) {
		return function->synthetic != NULL && frame->synthetic != NULL &&
			strcmp(function->synthetic, frame->synthetic) == 0;
	}
	return function->source_length == 0 ||
		(function->source != NULL && frame->source != NULL &&
			memcmp(function->source, frame->source,
				function->source_length) == 0);
}

static bool
format_function(lp_export_model *model, const lp_frame_desc *frame,
	int64_t *name_index, int64_t *filename_index,
	uint32_t *mapping_index) {
	char name[1200];
	lp_native_symbol native;
	const char *filename = "[unknown]";
	size_t filename_length = sizeof("[unknown]") - 1;
	if (frame->synthetic != NULL) {
		(void)snprintf(name, sizeof(name), "%s", frame->synthetic);
		filename = "[luaprof]";
		filename_length = sizeof("[luaprof]") - 1;
	}
	else if (frame->kind == LP_FRAME_C) {
		size_t lua_name_length = 0;
		const char *lua_name = model->symbols != NULL &&
			model->symbols->cfunction_name != NULL
			? model->symbols->cfunction_name(model->symbols->userdata,
				frame->cfunction, &lua_name_length) : NULL;
		bool native_found = lp_native_symbol_resolve(
			(const void *)frame->cfunction, &native);
		if (lua_name != NULL && lua_name_length != 0 &&
			native.name_length != 0 &&
			(lua_name_length != native.name_length ||
				memcmp(lua_name, native.name, lua_name_length) != 0)) {
			(void)snprintf(name, sizeof(name), "%.*s [%.*s]",
				(int)lua_name_length, lua_name, (int)native.name_length,
				native.name);
		}
		else if (lua_name != NULL && lua_name_length != 0) {
			(void)snprintf(name, sizeof(name), "%.*s", (int)lua_name_length,
				lua_name);
		}
		else if (native.name_length != 0) {
			(void)snprintf(name, sizeof(name), "%.*s",
				(int)native.name_length, native.name);
		}
		else {
			(void)snprintf(name, sizeof(name), "lua_CFunction@0x%" PRIxPTR,
				(uintptr_t)frame->cfunction);
		}
		if (native_found && native.path_length != 0) {
			filename = native.path;
			filename_length = native.path_length;
		}
		else {
			filename = "[C]";
			filename_length = sizeof("[C]") - 1;
		}
		if (native.has_mapping && native.path_length != 0) {
			*mapping_index = intern_mapping(model, native.mapping_start,
				native.mapping_limit, native.mapping_offset, native.path,
				native.path_length, lua_name_length != 0 ||
					native.name_length != 0, false, false);
			if (*mapping_index == UINT32_MAX) {
				return false;
			}
		}
	}
	else if (frame->name != NULL && frame->name_length != 0) {
		(void)snprintf(name, sizeof(name), "%.*s",
			(int)frame->name_length, frame->name);
		if (frame->source != NULL && frame->source_length != 0) {
			filename = frame->source;
			filename_length = frame->source_length;
			if ((filename[0] == '@' || filename[0] == '=') &&
				filename_length > 1) {
				filename++;
				filename_length--;
			}
		}
	}
	else if (frame->source != NULL && frame->source_length != 0) {
		const char *source = frame->source;
		size_t length = frame->source_length;
		if ((source[0] == '@' || source[0] == '=') && length > 1) {
			source++;
			length--;
		}
		if (frame->linedefined == 0) {
			(void)snprintf(name, sizeof(name), "main chunk");
		}
		else {
			(void)snprintf(name, sizeof(name), "lua:%.*s:%d", (int)length,
				source, frame->linedefined);
		}
		filename = source;
		filename_length = length;
	}
	else {
		(void)snprintf(name, sizeof(name), "lua_function@0x%" PRIxPTR
			":%d", (uintptr_t)frame->function, frame->linedefined);
	}
	*name_index = add_literal(model, name);
	*filename_index = add_string(model, filename, filename_length);
	return !model->failed;
}

static uint32_t
intern_function(lp_export_model *model, const lp_frame_desc *frame) {
	uint64_t hash = frame_hash(frame);
	for (size_t probe = 0; probe < LP_EXPORT_FUNCTION_HASH_CAPACITY;
		++probe) {
		size_t slot = (size_t)(hash + probe) &
			(LP_EXPORT_FUNCTION_HASH_CAPACITY - 1u);
		uint32_t stored = model->function_hash[slot];
		if (stored != 0) {
			uint32_t index = stored - 1u;
			if (function_matches(&model->functions[index], frame, hash)) {
				return index;
			}
			continue;
		}
		if (!grow_array((void **)&model->functions,
			&model->function_capacity, model->function_count + 1,
			sizeof(model->functions[0]))) {
			model_fail(model, "out of memory while building functions");
			return UINT32_MAX;
		}
		int64_t name = -1;
		int64_t filename = -1;
		uint32_t mapping_index = 0;
		if (!format_function(model, frame, &name, &filename,
			&mapping_index)) {
			return UINT32_MAX;
		}
		uint32_t index = (uint32_t)model->function_count++;
		model->functions[index] = (lp_export_function) {
			.hash = hash,
			.id = (uint64_t)index + 1,
			.kind = frame->kind,
			.function = frame->function,
			.cfunction = frame->cfunction,
			.source = frame->source,
			.source_length = frame->source_length,
			.linedefined = frame->linedefined,
			.synthetic = frame->synthetic,
			.name = name,
			.filename = filename,
			.mapping_index = mapping_index,
		};
		model->function_hash[slot] = index + 1u;
		return index;
	}
	model_fail(model, "function table capacity exceeded");
	return UINT32_MAX;
}

static uint64_t
location_hash(uint32_t function_index, int line) {
	uint64_t hash = UINT64_C(1469598103934665603);
	hash = hash_bytes(hash, &function_index, sizeof(function_index));
	return hash_bytes(hash, &line, sizeof(line));
}

static uint64_t
intern_location(lp_export_model *model, const lp_frame_desc *frame) {
	uint32_t function_index = intern_function(model, frame);
	if (function_index == UINT32_MAX) {
		return 0;
	}
	int line = frame->currentline > 0 ? frame->currentline : 0;
	uint64_t hash = location_hash(function_index, line);
	for (size_t probe = 0; probe < LP_EXPORT_LOCATION_HASH_CAPACITY;
		++probe) {
		size_t slot = (size_t)(hash + probe) &
			(LP_EXPORT_LOCATION_HASH_CAPACITY - 1u);
		uint32_t stored = model->location_hash[slot];
		if (stored != 0) {
			lp_export_location *location =
				&model->locations[stored - 1u];
			if (location->function_index == function_index &&
				location->line == line) {
				return location->id;
			}
			continue;
		}
		if (!grow_array((void **)&model->locations,
			&model->location_capacity, model->location_count + 1,
			sizeof(model->locations[0]))) {
			model_fail(model, "out of memory while building locations");
			return 0;
		}
		uint32_t index = (uint32_t)model->location_count++;
		model->locations[index] = (lp_export_location) {
			.id = (uint64_t)index + 1,
			.function_index = function_index,
			.mapping_index = model->functions[function_index].mapping_index,
			.line = line,
			.address = frame->kind == LP_FRAME_C
				? (uint64_t)(uintptr_t)frame->cfunction : 0,
		};
		model->location_hash[slot] = index + 1u;
		return model->locations[index].id;
	}
	model_fail(model, "location table capacity exceeded");
	return 0;
}

static int64_t
clamp_i64(uint64_t value) {
	return value > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)value;
}

static int64_t
multiply_i64(uint64_t left, uint64_t right) {
	if (left != 0 && right > (uint64_t)INT64_MAX / left) {
		return INT64_MAX;
	}
	return (int64_t)(left * right);
}

static lp_frame_desc
frame_desc(const lp_frame_view *frame) {
	return (lp_frame_desc) {
		.kind = frame->kind,
		.function = frame->function,
		.cfunction = frame->cfunction,
		.source = frame->source,
		.source_length = frame->source_length,
		.name = frame->name,
		.name_length = frame->name_length,
		.linedefined = frame->linedefined,
		.currentline = frame->currentline,
	};
}

static lp_frame_desc
synthetic_desc(const char *name) {
	return (lp_frame_desc) {
		.kind = LP_FRAME_LUA,
		.linedefined = 0,
		.currentline = 0,
		.synthetic = name,
	};
}

static lp_frame_desc
cfunction_desc(lp_lua_cfunction function) {
	return (lp_frame_desc) {
		.kind = LP_FRAME_C,
		.cfunction = function,
		.linedefined = -1,
		.currentline = -1,
	};
}

static bool
append_export_sample(lp_export_model *model, const lp_frame_desc *frames,
	size_t depth, const int64_t *values) {
	if (depth > LP_EXPORT_STACK_DEPTH || !grow_array(
		(void **)&model->samples, &model->sample_capacity,
		model->sample_count + 1, sizeof(model->samples[0]))) {
		model_fail(model, "out of memory while building samples");
		return false;
	}
	lp_export_sample *sample = &model->samples[model->sample_count++];
	memset(sample, 0, sizeof(*sample));
	for (size_t i = 0; i < depth; ++i) {
		sample->locations[i] = intern_location(model, &frames[i]);
		if (sample->locations[i] == 0) {
			return false;
		}
	}
	sample->depth = depth;
	memcpy(sample->values, values,
		model->value_count * sizeof(sample->values[0]));
	return true;
}

static bool
build_cpu_samples(lp_export_model *model, const lp_result_meta *result) {
	uint64_t period = UINT64_C(1000000000) /
		result->config.value.cpu.sample_hz;
	for (size_t i = 0; i < lp_result_cpu_sample_count(result); ++i) {
		lp_cpu_sample_view sample;
		if (!lp_result_cpu_sample(result, i, &sample)) {
			model_fail(model, "invalid CPU sample view");
			return false;
		}
		if (sample.depth >= LP_EXPORT_STACK_DEPTH) {
			model_fail(model, "CPU sample stack is too deep");
			return false;
		}
		lp_frame_desc frames[LP_EXPORT_STACK_DEPTH];
		size_t depth = 0;
		if (sample.state == LP_VM_HOST) {
			frames[depth++] = synthetic_desc("[host]");
		}
		else if (sample.state == LP_VM_GC) {
			frames[depth++] = synthetic_desc("[gc]");
		}
		bool has_cfunction = false;
		lp_frame_view captured[LP_EXPORT_STACK_DEPTH - 1u];
		for (size_t j = 0; j < sample.depth; ++j) {
			if (!lp_result_cpu_frame(result, i, j, &captured[j])) {
				memset(&captured[j], 0, sizeof(captured[j]));
				captured[j].kind = LP_FRAME_LUA;
			}
			if (sample.cfunction != NULL &&
				captured[j].kind == LP_FRAME_C &&
				captured[j].cfunction == sample.cfunction) {
				has_cfunction = true;
			}
		}
		if (sample.state == LP_VM_C && sample.cfunction != NULL &&
			!has_cfunction) {
			frames[depth++] = cfunction_desc(sample.cfunction);
		}
		for (size_t j = 0; j < sample.depth &&
			depth < LP_EXPORT_STACK_DEPTH; ++j) {
			frames[depth++] = captured[j].source == NULL &&
				captured[j].function == NULL &&
				captured[j].cfunction == NULL
				? synthetic_desc("[unknown frame]")
				: frame_desc(&captured[j]);
		}
		if (depth == 0) {
			frames[depth++] = synthetic_desc("[lua]");
		}
		int64_t values[4] = {
			clamp_i64(sample.weight),
			multiply_i64(sample.weight, period),
		};
		if (!append_export_sample(model, frames, depth, values)) {
			return false;
		}
	}
	return true;
}

static bool
build_memory_samples(lp_export_model *model, const lp_result_meta *result) {
	for (size_t i = 0; i < lp_result_memory_sample_count(result); ++i) {
		lp_memory_sample_view sample;
		if (!lp_result_memory_sample(result, i, &sample)) {
			model_fail(model, "invalid memory sample view");
			return false;
		}
		if (sample.depth >= LP_EXPORT_STACK_DEPTH) {
			model_fail(model, "memory sample stack is too deep");
			return false;
		}
		lp_frame_desc frames[LP_EXPORT_STACK_DEPTH];
		size_t depth = 0;
		for (size_t j = 0; j < sample.depth; ++j) {
			lp_frame_view captured;
			if (!lp_result_memory_frame(result, i, j, &captured)) {
				frames[depth++] = synthetic_desc("[unknown frame]");
			}
			else {
				frames[depth++] = frame_desc(&captured);
			}
		}
		if (depth == 0) {
			frames[depth++] = synthetic_desc("[allocation]");
		}
		int64_t values[4] = {
			clamp_i64(sample.alloc_objects),
			clamp_i64(sample.alloc_space),
			clamp_i64(sample.inuse_objects),
			clamp_i64(sample.inuse_space),
		};
		if (!append_export_sample(model, frames, depth, values)) {
			return false;
		}
	}
	return true;
}

static bool
select_metric(lp_export_model *model, const char *requested) {
	if (requested == NULL) {
		return true;
	}
	for (size_t i = 0; i < model->value_count; ++i) {
		if (strcmp(requested, model->sample_names[i]) == 0) {
			model->default_value = i;
			return true;
		}
	}
	model_fail(model, "sample is not available for this result kind");
	return false;
}

static bool
model_init(lp_export_model *model, const lp_result_meta *result,
	const char *sample_type, const lp_export_symbols *symbols) {
	memset(model, 0, sizeof(*model));
	model->symbols = symbols;
	if ((result->kind == LP_COLLECTOR_CPU &&
		result->config.value.cpu.sample_hz == 0) ||
		(result->kind == LP_COLLECTOR_MEMORY &&
			result->config.value.memory.sample_bytes == 0)) {
		model_fail(model, "invalid result sampling configuration");
		return false;
	}
	model->function_hash = calloc(LP_EXPORT_FUNCTION_HASH_CAPACITY,
		sizeof(model->function_hash[0]));
	model->location_hash = calloc(LP_EXPORT_LOCATION_HASH_CAPACITY,
		sizeof(model->location_hash[0]));
	if (model->function_hash == NULL || model->location_hash == NULL) {
		model_fail(model, "out of memory while creating export tables");
		return false;
	}
	(void)add_literal(model, "");
	if (intern_mapping(model, 0, 0, 0, "luaprof",
		sizeof("luaprof") - 1, true, true, true) == UINT32_MAX) {
		return false;
	}
	if (result->kind == LP_COLLECTOR_CPU) {
		model->sample_names[0] = "samples";
		model->sample_units[0] = "count";
		model->sample_names[1] = "cpu";
		model->sample_units[1] = "nanoseconds";
		model->value_count = 2;
		model->default_value = 1;
		model->period_type = add_literal(model, "cpu");
		model->period_unit = add_literal(model, "nanoseconds");
		model->period = (int64_t)(UINT64_C(1000000000) /
			result->config.value.cpu.sample_hz);
		model->comment = add_literal(model, "luaprof CPU sampling profile");
	}
	else if (result->kind == LP_COLLECTOR_MEMORY) {
		model->sample_names[0] = "alloc_objects";
		model->sample_units[0] = "count";
		model->sample_names[1] = "alloc_space";
		model->sample_units[1] = "bytes";
		model->sample_names[2] = "inuse_objects";
		model->sample_units[2] = "count";
		model->sample_names[3] = "inuse_space";
		model->sample_units[3] = "bytes";
		model->value_count = 4;
		model->default_value = result->config.value.memory.track_free ? 3 : 1;
		model->period_type = add_literal(model, "space");
		model->period_unit = add_literal(model, "bytes");
		model->period = clamp_i64(
			result->config.value.memory.sample_bytes);
		model->comment = add_literal(model, "luaprof memory sampling profile");
	}
	else {
		model_fail(model, "invalid result kind");
		return false;
	}
	for (size_t i = 0; i < model->value_count; ++i) {
		model->sample_name_indices[i] = add_literal(model,
			model->sample_names[i]);
		(void)add_literal(model, model->sample_units[i]);
	}
	if (!select_metric(model, sample_type) || model->failed) {
		return false;
	}
	return result->kind == LP_COLLECTOR_CPU
		? build_cpu_samples(model, result)
		: build_memory_samples(model, result);
}

static void
model_dispose(lp_export_model *model) {
	for (size_t i = 0; i < model->string_count; ++i) {
		free(model->strings[i].data);
	}
	free(model->strings);
	free(model->functions);
	free(model->function_hash);
	free(model->locations);
	free(model->location_hash);
	free(model->samples);
	free(model->mappings);
	memset(model, 0, sizeof(*model));
}

static void
emit_value_type(lp_buffer *profile, unsigned int field, int64_t type,
	int64_t unit) {
	lp_buffer value = { 0 };
	buffer_field_varint(&value, 1, (uint64_t)type);
	buffer_field_varint(&value, 2, (uint64_t)unit);
	buffer_field_message(profile, field, &value);
	if (value.failed) {
		profile->failed = true;
	}
	buffer_dispose(&value);
}

static void
emit_sample(lp_buffer *profile, const lp_export_sample *sample,
	size_t value_count) {
	lp_buffer message = { 0 };
	lp_buffer packed = { 0 };
	for (size_t i = 0; i < sample->depth; ++i) {
		buffer_varint(&packed, sample->locations[i]);
	}
	buffer_field_message(&message, 1, &packed);
	packed.size = 0;
	for (size_t i = 0; i < value_count; ++i) {
		buffer_varint(&packed, (uint64_t)sample->values[i]);
	}
	buffer_field_message(&message, 2, &packed);
	buffer_field_message(profile, 2, &message);
	if (message.failed || packed.failed) {
		profile->failed = true;
	}
	buffer_dispose(&packed);
	buffer_dispose(&message);
}

static void
emit_location(lp_buffer *profile, const lp_export_model *model,
	const lp_export_location *location) {
	lp_buffer message = { 0 };
	buffer_field_varint(&message, 1, location->id);
	buffer_field_varint(&message, 2,
		model->mappings[location->mapping_index].id);
	if (location->address != 0) {
		buffer_field_varint(&message, 3, location->address);
	}
	lp_buffer line = { 0 };
	buffer_field_varint(&line, 1,
		model->functions[location->function_index].id);
	if (location->line != 0) {
		buffer_field_varint(&line, 2, (uint64_t)(int64_t)location->line);
	}
	buffer_field_message(&message, 4, &line);
	buffer_field_message(profile, 4, &message);
	if (message.failed || line.failed) {
		profile->failed = true;
	}
	buffer_dispose(&line);
	buffer_dispose(&message);
}

static void
emit_mapping(lp_buffer *profile, const lp_export_mapping *mapping) {
	lp_buffer message = { 0 };
	buffer_field_varint(&message, 1, mapping->id);
	if (mapping->start != 0) {
		buffer_field_varint(&message, 2, mapping->start);
	}
	if (mapping->limit != 0) {
		buffer_field_varint(&message, 3, mapping->limit);
	}
	if (mapping->offset != 0) {
		buffer_field_varint(&message, 4, mapping->offset);
	}
	buffer_field_varint(&message, 5, (uint64_t)mapping->filename);
	if (mapping->has_functions) {
		buffer_field_varint(&message, 7, 1);
	}
	if (mapping->has_filenames) {
		buffer_field_varint(&message, 8, 1);
	}
	if (mapping->has_line_numbers) {
		buffer_field_varint(&message, 9, 1);
	}
	buffer_field_message(profile, 3, &message);
	if (message.failed) {
		profile->failed = true;
	}
	buffer_dispose(&message);
}

static void
emit_function(lp_buffer *profile, const lp_export_function *function) {
	lp_buffer message = { 0 };
	buffer_field_varint(&message, 1, function->id);
	buffer_field_varint(&message, 2, (uint64_t)function->name);
	buffer_field_varint(&message, 3, (uint64_t)function->name);
	buffer_field_varint(&message, 4, (uint64_t)function->filename);
	if (function->linedefined > 0) {
		buffer_field_varint(&message, 5,
			(uint64_t)(int64_t)function->linedefined);
	}
	buffer_field_message(profile, 5, &message);
	if (message.failed) {
		profile->failed = true;
	}
	buffer_dispose(&message);
}

static bool
encode_profile(const lp_export_model *model, lp_buffer *profile) {
	for (size_t i = 0; i < model->value_count; ++i) {
		int64_t unit = -1;
		for (size_t j = 0; j < model->string_count; ++j) {
			if (strcmp(model->strings[j].data, model->sample_units[i]) == 0) {
				unit = (int64_t)j;
				break;
			}
		}
		if (unit < 0) {
			return false;
		}
		emit_value_type(profile, 1, model->sample_name_indices[i], unit);
	}
	for (size_t i = 0; i < model->sample_count; ++i) {
		emit_sample(profile, &model->samples[i], model->value_count);
	}
	for (size_t i = 0; i < model->mapping_count; ++i) {
		emit_mapping(profile, &model->mappings[i]);
	}
	for (size_t i = 0; i < model->location_count; ++i) {
		emit_location(profile, model, &model->locations[i]);
	}
	for (size_t i = 0; i < model->function_count; ++i) {
		emit_function(profile, &model->functions[i]);
	}
	for (size_t i = 0; i < model->string_count; ++i) {
		buffer_field_bytes(profile, 6, model->strings[i].data,
			model->strings[i].length);
	}
	emit_value_type(profile, 11, model->period_type, model->period_unit);
	buffer_field_varint(profile, 12, (uint64_t)model->period);
	buffer_field_varint(profile, 13, (uint64_t)model->comment);
	buffer_field_varint(profile, 14,
		(uint64_t)model->sample_name_indices[model->default_value]);
	return !profile->failed;
}

static bool
write_gzip(const char *path, const lp_buffer *profile, char *error,
	size_t error_capacity) {
	gzFile file = gzopen(path, "wb");
	if (file == NULL) {
		set_error(error, error_capacity, "cannot open '%s' for writing: %s",
			path, strerror(errno));
		return false;
	}
	size_t offset = 0;
	while (offset < profile->size) {
		size_t remaining = profile->size - offset;
		unsigned int chunk = remaining > INT_MAX ? INT_MAX
			: (unsigned int)remaining;
		int written = gzwrite(file, profile->data + offset, chunk);
		if (written <= 0) {
			int code = 0;
			const char *message = gzerror(file, &code);
			set_error(error, error_capacity, "cannot write '%s': %s", path,
				message == NULL ? "gzip error" : message);
			(void)gzclose(file);
			return false;
		}
		offset += (size_t)written;
	}
	if (gzclose(file) != Z_OK) {
		set_error(error, error_capacity, "cannot finish gzip stream '%s'",
			path);
		return false;
	}
	return true;
}

static void
write_folded_name(FILE *file, const char *name) {
	for (; *name != '\0'; ++name) {
		char value = *name;
		if (value == ';' || value == '\n' || value == '\r') {
			value = '_';
		}
		(void)fputc(value, file);
	}
}

static bool
write_folded(const char *path, const lp_export_model *model, char *error,
	size_t error_capacity) {
	FILE *file = fopen(path, "wb");
	if (file == NULL) {
		set_error(error, error_capacity, "cannot open '%s': %s", path,
			strerror(errno));
		return false;
	}
	for (size_t i = 0; i < model->sample_count; ++i) {
		const lp_export_sample *sample = &model->samples[i];
		int64_t value = sample->values[model->default_value];
		if (value == 0) {
			continue;
		}
		for (size_t j = sample->depth; j-- != 0;) {
			const lp_export_location *location =
				&model->locations[sample->locations[j] - 1u];
			const lp_export_function *function =
				&model->functions[location->function_index];
			if (j + 1 != sample->depth) {
				(void)fputc(';', file);
			}
			write_folded_name(file,
				model->strings[function->name].data);
		}
		(void)fprintf(file, " %" PRId64 "\n", value);
	}
	if (fclose(file) != 0) {
		set_error(error, error_capacity, "cannot finish '%s': %s", path,
			strerror(errno));
		return false;
	}
	return true;
}

bool
lp_export_result_with_symbols(const lp_result_meta *result, const char *path,
	lp_export_format format, const char *sample_type,
	const lp_export_symbols *symbols, char *error, size_t error_capacity) {
	if (error != NULL && error_capacity != 0) {
		error[0] = '\0';
	}
	if (result == NULL || path == NULL || path[0] == '\0' ||
		result->private_data == NULL ||
		(format != LP_EXPORT_PPROF && format != LP_EXPORT_FOLDED)) {
		set_error(error, error_capacity, "invalid export arguments");
		return false;
	}
	lp_export_model model;
	if (!model_init(&model, result, sample_type, symbols)) {
		set_error(error, error_capacity, "%s",
			model.failure[0] == '\0' ? "cannot build profile" : model.failure);
		model_dispose(&model);
		return false;
	}
	bool success;
	if (format == LP_EXPORT_FOLDED) {
		success = write_folded(path, &model, error, error_capacity);
	}
	else {
		lp_buffer profile = { 0 };
		success = encode_profile(&model, &profile);
		if (!success) {
			set_error(error, error_capacity,
				"out of memory while encoding profile");
		}
		else {
			success = write_gzip(path, &profile, error, error_capacity);
		}
		buffer_dispose(&profile);
	}
	model_dispose(&model);
	return success;
}

bool
lp_export_result(const lp_result_meta *result, const char *path,
	lp_export_format format, const char *sample_type, char *error,
	size_t error_capacity) {
	return lp_export_result_with_symbols(result, path, format, sample_type,
		NULL, error, error_capacity);
}
