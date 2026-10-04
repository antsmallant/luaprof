#define _POSIX_C_SOURCE 200809L

#include "luaprof/runtime.h"
#include "native_symbol.h"
#include "pprof_exporter.h"

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <zlib.h>

typedef struct profile_summary {
	size_t sample_types;
	size_t samples;
	size_t locations;
	size_t functions;
	size_t mappings;
	size_t strings;
	size_t expected_values;
	uint64_t value_totals[4];
	uint64_t max_location_reference;
	uint64_t max_function_reference;
	uint64_t default_sample_type;
	const unsigned char *string_data[128];
	size_t string_lengths[128];
	uint64_t comments[32];
	size_t comment_count;
	bool saw_period_type;
	bool saw_period;
	bool saw_empty_string_first;
	bool saw_cpu;
	bool saw_alloc_space;
	bool saw_inuse_space;
	bool saw_cpu_source;
	bool saw_memory_source;
	bool saw_cfunction;
	bool saw_raw_cfunction;
	bool saw_post_tick_callback;
} profile_summary;

static int
profiled_cfunction(lua_State *L) {
	(void)L;
	return 0;
}

static lp_lua_cfunction
unresolved_cfunction(void) {
	return (lp_lua_cfunction)(uintptr_t)1;
}

static const char *
cfunction_name(void *userdata, lp_lua_cfunction function, size_t *length) {
	(void)userdata;
	if (function == profiled_cfunction) {
		*length = sizeof("visible.profiled") - 1u;
		return "visible.profiled";
	}
	*length = 0;
	return NULL;
}

static bool
read_varint(const unsigned char *data, size_t size, size_t *offset,
	uint64_t *value) {
	uint64_t result = 0;
	for (unsigned int shift = 0; shift < 70; shift += 7) {
		if (*offset >= size) {
			return false;
		}
		unsigned char byte = data[(*offset)++];
		if (shift == 63 && (byte & 0xfeu) != 0) {
			return false;
		}
		result |= (uint64_t)(byte & 0x7fu) << shift;
		if ((byte & 0x80u) == 0) {
			*value = result;
			return true;
		}
	}
	return false;
}

static bool
read_bytes(const unsigned char *data, size_t size, size_t *offset,
	const unsigned char **value, size_t *length) {
	uint64_t encoded = 0;
	if (!read_varint(data, size, offset, &encoded) ||
		encoded > SIZE_MAX || (size_t)encoded > size - *offset) {
		return false;
	}
	*value = data + *offset;
	*length = (size_t)encoded;
	*offset += *length;
	return true;
}

static bool
skip_field(const unsigned char *data, size_t size, size_t *offset,
	unsigned int wire) {
	uint64_t ignored;
	const unsigned char *bytes;
	size_t length;
	switch (wire) {
	case 0:
		return read_varint(data, size, offset, &ignored);
	case 1:
		if (size - *offset < 8) {
			return false;
		}
		*offset += 8;
		return true;
	case 2:
		return read_bytes(data, size, offset, &bytes, &length);
	case 5:
		if (size - *offset < 4) {
			return false;
		}
		*offset += 4;
		return true;
	default:
		return false;
	}
}

static bool
bytes_equal(const unsigned char *data, size_t size, const char *text) {
	return strlen(text) == size && memcmp(data, text, size) == 0;
}

static bool
bytes_contain(const unsigned char *data, size_t size, const char *text) {
	size_t wanted = strlen(text);
	if (wanted > size) {
		return false;
	}
	for (size_t i = 0; i <= size - wanted; ++i) {
		if (memcmp(data + i, text, wanted) == 0) {
			return true;
		}
	}
	return false;
}

static bool
parse_packed(const unsigned char *data, size_t size, size_t *count,
	uint64_t *maximum) {
	size_t offset = 0;
	while (offset < size) {
		uint64_t value;
		if (!read_varint(data, size, &offset, &value)) {
			return false;
		}
		(*count)++;
		if (maximum != NULL && value > *maximum) {
			*maximum = value;
		}
	}
	return offset == size;
}

static bool
parse_values(const unsigned char *data, size_t size, size_t *count,
	profile_summary *summary) {
	size_t offset = 0;
	while (offset < size) {
		uint64_t value;
		if (!read_varint(data, size, &offset, &value)) {
			return false;
		}
		if (*count < sizeof(summary->value_totals) /
			sizeof(summary->value_totals[0])) {
			summary->value_totals[*count] += value;
		}
		(*count)++;
	}
	return offset == size;
}

static bool
parse_sample(const unsigned char *data, size_t size,
	profile_summary *summary) {
	size_t offset = 0;
	size_t values = 0;
	size_t locations = 0;
	while (offset < size) {
		uint64_t key;
		if (!read_varint(data, size, &offset, &key)) {
			return false;
		}
		unsigned int field = (unsigned int)(key >> 3);
		unsigned int wire = (unsigned int)(key & 7u);
		if ((field == 1 || field == 2) && wire == 2) {
			const unsigned char *packed;
			size_t length;
			if (!read_bytes(data, size, &offset, &packed, &length)) {
				return false;
			}
			if (field == 1 && !parse_packed(packed, length, &locations,
				&summary->max_location_reference)) {
				return false;
			}
			if (field == 2 && !parse_values(packed, length, &values, summary)) {
				return false;
			}
		}
		else if (!skip_field(data, size, &offset, wire)) {
			return false;
		}
	}
	return locations != 0 && values == summary->expected_values;
}

static bool
parse_line(const unsigned char *data, size_t size,
	profile_summary *summary) {
	size_t offset = 0;
	while (offset < size) {
		uint64_t key;
		if (!read_varint(data, size, &offset, &key)) {
			return false;
		}
		unsigned int field = (unsigned int)(key >> 3);
		unsigned int wire = (unsigned int)(key & 7u);
		if (field == 1 && wire == 0) {
			uint64_t function;
			if (!read_varint(data, size, &offset, &function)) {
				return false;
			}
			if (function > summary->max_function_reference) {
				summary->max_function_reference = function;
			}
		}
		else if (!skip_field(data, size, &offset, wire)) {
			return false;
		}
	}
	return true;
}

static bool
parse_location(const unsigned char *data, size_t size,
	profile_summary *summary) {
	size_t offset = 0;
	bool saw_id = false;
	bool saw_line = false;
	while (offset < size) {
		uint64_t key;
		if (!read_varint(data, size, &offset, &key)) {
			return false;
		}
		unsigned int field = (unsigned int)(key >> 3);
		unsigned int wire = (unsigned int)(key & 7u);
		if (field == 1 && wire == 0) {
			uint64_t id;
			if (!read_varint(data, size, &offset, &id) || id == 0) {
				return false;
			}
			saw_id = true;
		}
		else if (field == 4 && wire == 2) {
			const unsigned char *line;
			size_t length;
			if (!read_bytes(data, size, &offset, &line, &length) ||
				!parse_line(line, length, summary)) {
				return false;
			}
			saw_line = true;
		}
		else if (!skip_field(data, size, &offset, wire)) {
			return false;
		}
	}
	return saw_id && saw_line;
}

static bool
parse_function(const unsigned char *data, size_t size) {
	size_t offset = 0;
	bool saw_id = false;
	bool saw_name = false;
	while (offset < size) {
		uint64_t key;
		if (!read_varint(data, size, &offset, &key)) {
			return false;
		}
		unsigned int field = (unsigned int)(key >> 3);
		unsigned int wire = (unsigned int)(key & 7u);
		if ((field == 1 || field == 2) && wire == 0) {
			uint64_t value;
			if (!read_varint(data, size, &offset, &value)) {
				return false;
			}
			saw_id |= field == 1 && value != 0;
			saw_name |= field == 2 && value != 0;
		}
		else if (!skip_field(data, size, &offset, wire)) {
			return false;
		}
	}
	return saw_id && saw_name;
}

static bool
parse_profile(const unsigned char *data, size_t size, size_t values,
	profile_summary *summary) {
	memset(summary, 0, sizeof(*summary));
	summary->expected_values = values;
	size_t offset = 0;
	while (offset < size) {
		uint64_t key;
		if (!read_varint(data, size, &offset, &key)) {
			return false;
		}
		unsigned int field = (unsigned int)(key >> 3);
		unsigned int wire = (unsigned int)(key & 7u);
		if ((field == 1 || field == 2 || field == 3 || field == 4 || field == 5 ||
			field == 6 || field == 11) && wire == 2) {
			const unsigned char *message;
			size_t length;
			if (!read_bytes(data, size, &offset, &message, &length)) {
				return false;
			}
			if (field == 1) {
				summary->sample_types++;
			}
			else if (field == 2) {
				summary->samples++;
				if (!parse_sample(message, length, summary)) {
					return false;
				}
			}
			else if (field == 4) {
				summary->locations++;
				if (!parse_location(message, length, summary)) {
					return false;
				}
			}
			else if (field == 3) {
				summary->mappings++;
			}
			else if (field == 5) {
				summary->functions++;
				if (!parse_function(message, length)) {
					return false;
				}
			}
			else if (field == 6) {
				if (summary->strings >= sizeof(summary->string_data) /
					sizeof(summary->string_data[0])) {
					return false;
				}
				summary->string_data[summary->strings] = message;
				summary->string_lengths[summary->strings] = length;
				if (summary->strings == 0 && length == 0) {
					summary->saw_empty_string_first = true;
				}
				summary->strings++;
				summary->saw_cpu |= bytes_equal(message, length, "cpu");
				summary->saw_alloc_space |=
					bytes_equal(message, length, "alloc_space");
				summary->saw_inuse_space |=
					bytes_equal(message, length, "inuse_space");
				summary->saw_cpu_source |=
					bytes_contain(message, length, "pprof_cpu.lua");
				summary->saw_memory_source |=
					bytes_contain(message, length, "pprof_memory.lua");
				summary->saw_cfunction |=
					bytes_contain(message, length, "profiled_cfunction");
				summary->saw_raw_cfunction |=
					bytes_contain(message, length, "lua_CFunction@0x1");
				summary->saw_post_tick_callback |=
					bytes_contain(message, length, "innocent_callback") ||
					bytes_contain(message, length, "later_callback");
			}
			else {
				summary->saw_period_type = true;
			}
		}
		else if ((field == 12 || field == 13 || field == 14) && wire == 0) {
			uint64_t value;
			if (!read_varint(data, size, &offset, &value)) {
				return false;
			}
			if (field == 12) {
				summary->saw_period = value != 0;
			}
			else if (field == 13) {
				if (summary->comment_count >= sizeof(summary->comments) /
					sizeof(summary->comments[0])) {
					return false;
				}
				summary->comments[summary->comment_count++] = value;
			}
			else {
				summary->default_sample_type = value;
			}
		}
		else if (!skip_field(data, size, &offset, wire)) {
			return false;
		}
	}
	for (size_t i = 0; i < summary->comment_count; ++i) {
		if (summary->comments[i] >= summary->strings) {
			return false;
		}
	}
	return summary->sample_types == values &&
		(summary->samples == 0 || (summary->locations != 0 &&
		 summary->functions != 0 && summary->mappings != 0)) &&
		summary->saw_empty_string_first && summary->saw_period_type &&
		summary->saw_period && summary->default_sample_type < summary->strings &&
		summary->max_location_reference <= summary->locations &&
		summary->max_function_reference <= summary->functions;
}

static void
assert_comment(const profile_summary *summary, const char *expected) {
	size_t matches = 0;
	for (size_t i = 0; i < summary->comment_count; ++i) {
		size_t index = (size_t)summary->comments[i];
		matches += bytes_equal(summary->string_data[index],
			summary->string_lengths[index], expected);
	}
	assert(matches == 1);
}

static void
assert_cpu_quality(const profile_summary *summary, const lp_result *result) {
	const lp_result_stats *stats = &result->stats;
	const struct {
		const char *name;
		uint64_t value;
	} fields[] = {
		{ "sample_hz", result->config.value.cpu.sample_hz },
		{ "samples", stats->samples },
		{ "overrun_events", stats->overrun_events },
		{ "overrun_ticks", stats->overrun_ticks },
		{ "dropped_events", stats->dropped_events },
		{ "unstable_events", stats->unstable_events },
		{ "profiler_overhead_events", stats->profiler_overhead_events },
		{ "stale_events", stats->stale_events },
		{ "timer_failures", stats->timer_failures },
		{ "stack_truncations", stats->stack_truncations },
		{ "aggregate_overflows", stats->aggregate_overflows },
		{ "symbol_overflows", stats->symbol_overflows },
		{ "scheduler_workers", stats->scheduler_workers },
	};
	assert(summary->comment_count == 15);
	assert_comment(summary, "luaprof CPU sampling profile");
	assert_comment(summary, "luaprof.metadata.version=1");
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
		char text[96];
		int length = snprintf(text, sizeof(text), "luaprof.cpu.%s=%" PRIu64,
			fields[i].name, fields[i].value);
		assert(length > 0 && (size_t)length < sizeof(text));
		assert_comment(summary, text);
	}
}

static unsigned char *
read_gzip(const char *path, size_t *size) {
	FILE *raw = fopen(path, "rb");
	assert(raw != NULL);
	assert(fgetc(raw) == 0x1f);
	assert(fgetc(raw) == 0x8b);
	assert(fclose(raw) == 0);
	gzFile file = gzopen(path, "rb");
	assert(file != NULL);
	size_t capacity = 4096;
	unsigned char *data = malloc(capacity);
	assert(data != NULL);
	*size = 0;
	for (;;) {
		if (*size == capacity) {
			capacity *= 2;
			data = realloc(data, capacity);
			assert(data != NULL);
		}
		int count = gzread(file, data + *size,
			(unsigned int)(capacity - *size));
		assert(count >= 0);
		if (count == 0) {
			break;
		}
		*size += (size_t)count;
	}
	assert(gzclose(file) == Z_OK);
	return data;
}

static char *
read_text(const char *path) {
	FILE *file = fopen(path, "rb");
	assert(file != NULL);
	assert(fseek(file, 0, SEEK_END) == 0);
	long length = ftell(file);
	assert(length >= 0);
	assert(fseek(file, 0, SEEK_SET) == 0);
	char *text = malloc((size_t)length + 1);
	assert(text != NULL);
	assert(fread(text, 1, (size_t)length, file) == (size_t)length);
	text[length] = '\0';
	assert(fclose(file) == 0);
	return text;
}

static void
write_text(const char *path, const char *text) {
	FILE *file = fopen(path, "wb");
	assert(file != NULL);
	assert(fwrite(text, 1, strlen(text), file) == strlen(text));
	assert(fclose(file) == 0);
}

static void
assert_text(const char *path, const char *expected) {
	char *actual = read_text(path);
	assert(strcmp(actual, expected) == 0);
	free(actual);
}

static void
assert_mode(const char *path, mode_t expected) {
	struct stat status;
	assert(stat(path, &status) == 0);
	assert((status.st_mode & 0777) == expected);
}

static void
assert_failed_export_preserves(const lp_result *result, const char *path,
	lp_export_format format) {
	const char sentinel[] = "existing-profile\n";
	write_text(path, sentinel);
	pid_t child = fork();
	assert(child >= 0);
	if (child == 0) {
		struct sigaction ignore = { 0 };
		ignore.sa_handler = SIG_IGN;
		assert(sigemptyset(&ignore.sa_mask) == 0);
		assert(sigaction(SIGXFSZ, &ignore, NULL) == 0);
		struct rlimit limit = { 0, 0 };
		assert(setrlimit(RLIMIT_FSIZE, &limit) == 0);
		char error[256];
		bool success = lp_export_result(result, path, format, NULL, error,
			sizeof(error));
		_exit(!success && error[0] != '\0' ? EXIT_SUCCESS : EXIT_FAILURE);
	}
	int status;
	assert(waitpid(child, &status, 0) == child);
	assert(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
	assert_text(path, sentinel);
}

static lp_result
cpu_result(void) {
	lp_runtime *runtime = lp_runtime_new(NULL, NULL, NULL);
	assert(runtime != NULL);
	lp_collector_config config = {
		.kind = LP_COLLECTOR_CPU,
		.value.cpu = { .sample_hz = 100 },
	};
	uint64_t generation;
	assert(lp_runtime_start(runtime, NULL, &config, &generation) == LP_OK);
	lp_stack_frame frame = {
		.kind = LP_FRAME_LUA,
		.function = runtime,
		.source = "@pprof_cpu.lua",
		.source_length = sizeof("@pprof_cpu.lua") - 1,
		.name = "cpu_work",
		.name_length = sizeof("cpu_work") - 1,
		.linedefined = 10,
		.currentline = 12,
	};
	lp_runtime_cpu_sample(runtime, generation, LP_VM_C, profiled_cfunction,
		&frame, 1, false);
	lp_runtime_cpu_sample(runtime, generation, LP_VM_C, unresolved_cfunction(),
		&frame, 1, false);
	lp_stack_frame callback_frames[] = {
		{
			.kind = LP_FRAME_LUA,
			.function = &callback_frames,
			.source = "@post_tick.lua",
			.source_length = sizeof("@post_tick.lua") - 1,
			.name = "innocent_callback",
			.name_length = sizeof("innocent_callback") - 1,
			.linedefined = 20,
			.currentline = 21,
		},
		{
			.kind = LP_FRAME_C,
			.cfunction = profiled_cfunction,
			.linedefined = -1,
			.currentline = -1,
		},
		{
			.kind = LP_FRAME_LUA,
			.function = runtime,
			.source = "@post_tick.lua",
			.source_length = sizeof("@post_tick.lua") - 1,
			.name = "outer_caller",
			.name_length = sizeof("outer_caller") - 1,
			.linedefined = 10,
			.currentline = 12,
		},
	};
	lp_runtime_cpu_sample(runtime, generation, LP_VM_C, profiled_cfunction,
		callback_frames, sizeof(callback_frames) / sizeof(callback_frames[0]),
		false);
	lp_stack_frame repeated_frames[] = {
		{
			.kind = LP_FRAME_LUA,
			.function = &repeated_frames,
			.source = "@repeated_c.lua",
			.source_length = sizeof("@repeated_c.lua") - 1,
			.name = "later_callback",
			.name_length = sizeof("later_callback") - 1,
			.linedefined = 30,
			.currentline = 31,
		},
		{
			.kind = LP_FRAME_C,
			.cfunction = profiled_cfunction,
			.linedefined = -1,
			.currentline = -1,
		},
		{
			.kind = LP_FRAME_LUA,
			.function = runtime,
			.source = "@repeated_c.lua",
			.source_length = sizeof("@repeated_c.lua") - 1,
			.name = "recursive_caller",
			.name_length = sizeof("recursive_caller") - 1,
			.linedefined = 20,
			.currentline = 22,
		},
		{
			.kind = LP_FRAME_C,
			.cfunction = profiled_cfunction,
			.linedefined = -1,
			.currentline = -1,
		},
		{
			.kind = LP_FRAME_LUA,
			.function = &frame,
			.source = "@repeated_c.lua",
			.source_length = sizeof("@repeated_c.lua") - 1,
			.name = "recursive_root",
			.name_length = sizeof("recursive_root") - 1,
			.linedefined = 10,
			.currentline = 12,
		},
	};
	lp_runtime_cpu_sample(runtime, generation, LP_VM_C, profiled_cfunction,
		repeated_frames, sizeof(repeated_frames) / sizeof(repeated_frames[0]),
		false);
	lp_runtime_cpu_sample(runtime, generation, LP_VM_GC, NULL, &frame, 1,
		false);
	lp_runtime_cpu_quality(runtime, generation, 0, 0, 0, 1, 3);
	lp_result result;
	assert(lp_runtime_stop(runtime, NULL, LP_COLLECTOR_CPU, generation,
		&result) == LP_OK);
	lp_runtime_delete(runtime);
	return result;
}

static void
record_allocation(lp_runtime *runtime, uint64_t generation, void *pointer,
	size_t size, lp_stack_frame *frame) {
	lp_runtime_allocation(runtime, generation, NULL, NULL, pointer, 0, size,
		true);
	uint64_t space;
	uint64_t objects;
	assert(lp_runtime_memory_sample_candidate(runtime, generation, NULL,
		pointer, size, true, &space, &objects));
	lp_runtime_memory_sample(runtime, generation, pointer, frame, 1, false,
		size, space, objects);
}

static lp_result
memory_result(void) {
	lp_runtime *runtime = lp_runtime_new(NULL, NULL, NULL);
	assert(runtime != NULL);
	lp_collector_config config = {
		.kind = LP_COLLECTOR_MEMORY,
		.value.memory = {
			.sample_bytes = 1,
			.track_free = true,
		},
	};
	uint64_t generation;
	assert(lp_runtime_start(runtime, NULL, &config, &generation) == LP_OK);
	lp_stack_frame frame = {
		.kind = LP_FRAME_LUA,
		.function = runtime,
		.source = "@pprof_memory.lua",
		.source_length = sizeof("@pprof_memory.lua") - 1,
		.name = "memory_work",
		.name_length = sizeof("memory_work") - 1,
		.linedefined = 20,
		.currentline = 22,
	};
	void *first = (void *)(uintptr_t)0x1000;
	void *second = (void *)(uintptr_t)0x2000;
	record_allocation(runtime, generation, first, 64, &frame);
	record_allocation(runtime, generation, second, 128, &frame);
	lp_runtime_allocation(runtime, generation, NULL, second, NULL, 128, 0,
		true);
	lp_result result;
	assert(lp_runtime_stop(runtime, NULL, LP_COLLECTOR_MEMORY, generation,
		&result) == LP_OK);
	lp_runtime_delete(runtime);
	return result;
}

static void
test_output_path(char *path, size_t capacity, const char *name) {
	const char *directory = getenv("LP_PPROF_OUTPUT_DIR");
	if (directory == NULL) {
		directory = "/tmp";
	}
	int length = snprintf(path, capacity, "%s/luaprof-pprof-%s", directory,
		name);
	assert(length > 0 && (size_t)length < capacity);
}

int
main(void) {
	char cpu_path[PATH_MAX], cpu_folded[PATH_MAX];
	char memory_path[PATH_MAX], memory_folded[PATH_MAX];
	char empty_path[PATH_MAX], large_path[PATH_MAX], zero_path[PATH_MAX];
	test_output_path(cpu_path, sizeof(cpu_path), "cpu.pb.gz");
	test_output_path(cpu_folded, sizeof(cpu_folded), "cpu.folded");
	test_output_path(memory_path, sizeof(memory_path), "memory.pb.gz");
	test_output_path(memory_folded, sizeof(memory_folded), "memory.folded");
	test_output_path(empty_path, sizeof(empty_path), "empty.pb.gz");
	test_output_path(large_path, sizeof(large_path), "large.pb.gz");
	test_output_path(zero_path, sizeof(zero_path), "zero.pb.gz");
	(void)unlink(cpu_path);
	(void)unlink(cpu_folded);
	(void)unlink(memory_path);
	(void)unlink(memory_folded);
	char error[256];
	lp_native_symbol native;
	memset(&native, 0xff, sizeof(native));
	assert(!lp_native_symbol_resolve(NULL, &native));
	assert(native.name_length == 0 && native.path_length == 0);
	assert(!native.has_mapping);
	assert(!lp_native_symbol_resolve((const void *)(uintptr_t)1, &native));
	assert(!lp_native_symbol_resolve(NULL, NULL));

	lp_result cpu = cpu_result();
	write_text(cpu_path, "old-profile\n");
	assert(chmod(cpu_path, 0640) == 0);
	assert(lp_export_result(&cpu, cpu_path, LP_EXPORT_PPROF, NULL, error,
		sizeof(error)));
	assert_mode(cpu_path, 0640);
	lp_export_symbols symbols = {
		.cfunction_name = cfunction_name,
	};
	assert(lp_export_result_with_symbols(&cpu, cpu_folded, LP_EXPORT_FOLDED,
		"samples", &symbols, error, sizeof(error)));
	assert_mode(cpu_folded, 0600);
	size_t size;
	unsigned char *data = read_gzip(cpu_path, &size);
	profile_summary summary;
	assert(parse_profile(data, size, 2, &summary));
	assert_cpu_quality(&summary, &cpu);
	assert(summary.saw_cpu);
	assert(summary.saw_cpu_source);
	assert(summary.saw_cfunction);
	assert(summary.saw_raw_cfunction);
	assert(!summary.saw_post_tick_callback);
	assert(summary.value_totals[0] == 5);
	assert(summary.value_totals[1] == UINT64_C(50000000));
	free(data);
	char *folded = read_text(cpu_folded);
	assert(strstr(folded, "cpu_work") != NULL);
	assert(strstr(folded,
		"visible.profiled [profiled_cfunction]") != NULL);
	assert(strstr(folded, "lua_CFunction@0x1") != NULL);
	assert(strstr(folded,
		"cpu_work;visible.profiled [profiled_cfunction] 1\n") != NULL);
	assert(strstr(folded, "cpu_work;lua_CFunction@0x1 1\n") != NULL);
	assert(strstr(folded, "innocent_callback") == NULL);
	assert(strstr(folded, "later_callback") == NULL);
	assert(strstr(folded,
		"outer_caller;visible.profiled [profiled_cfunction] 1\n") != NULL);
	assert(strstr(folded,
		"recursive_root;visible.profiled [profiled_cfunction];recursive_caller;"
		"visible.profiled [profiled_cfunction] 1\n") != NULL);
	free(folded);

	/* Metadata does not change either the CPU weights or folded stacks. */
	const lp_result_stats recorded_stats = cpu.stats;
	cpu.stats.overrun_events = 0;
	cpu.stats.overrun_ticks = 0;
	assert(lp_export_result(&cpu, zero_path, LP_EXPORT_PPROF, NULL, error,
		sizeof(error)));
	data = read_gzip(zero_path, &size);
	assert(parse_profile(data, size, 2, &summary));
	assert_cpu_quality(&summary, &cpu);
	assert(summary.value_totals[0] == 5);
	assert(summary.value_totals[1] == UINT64_C(50000000));
	free(data);
	folded = read_text(cpu_folded);
	assert(lp_export_result_with_symbols(&cpu, cpu_folded, LP_EXPORT_FOLDED,
		"samples", &symbols, error, sizeof(error)));
	assert_text(cpu_folded, folded);
	free(folded);
	cpu.stats = recorded_stats;

	lp_runtime *empty_runtime = lp_runtime_new(NULL, NULL, NULL);
	assert(empty_runtime != NULL);
	uint64_t empty_generation;
	lp_collector_config config = {
		.kind = LP_COLLECTOR_CPU,
		.value.cpu = { .sample_hz = 100 },
	};
	assert(lp_runtime_start(empty_runtime, NULL, &config, &empty_generation)
		== LP_OK);
	lp_runtime_cpu_quality(empty_runtime, empty_generation, 1, 0, 0, 1, 3);
	lp_result empty;
	assert(lp_runtime_stop(empty_runtime, NULL, LP_COLLECTOR_CPU,
		empty_generation, &empty) == LP_OK);
	lp_runtime_delete(empty_runtime);
	assert(lp_export_result(&empty, empty_path, LP_EXPORT_PPROF, NULL, error,
		sizeof(error)));
	data = read_gzip(empty_path, &size);
	assert(parse_profile(data, size, 2, &summary));
	assert_cpu_quality(&summary, &empty);
	assert(summary.samples == 0);
	free(data);
	lp_result_dispose(&empty);

	cpu.stats.overrun_ticks = UINT64_MAX;
	cpu.stats.profiler_overhead_events = UINT64_C(9223372036854775808);
	cpu.stats.dropped_events = 7;
	cpu.stats.unstable_events = 8;
	cpu.stats.stale_events = 9;
	cpu.stats.timer_failures = 10;
	cpu.stats.stack_truncations = 11;
	cpu.stats.aggregate_overflows = 12;
	cpu.stats.symbol_overflows = 13;
	cpu.stats.scheduler_workers = 2;
	assert(lp_export_result(&cpu, large_path, LP_EXPORT_PPROF, NULL, error,
		sizeof(error)));
	data = read_gzip(large_path, &size);
	assert(parse_profile(data, size, 2, &summary));
	assert_cpu_quality(&summary, &cpu);
	assert(summary.value_totals[0] == 5);
	assert(summary.value_totals[1] == UINT64_C(50000000));
	free(data);
	cpu.stats = recorded_stats;

	char failure_directory[] = "/tmp/luaprof-export-failure-XXXXXX";
	assert(mkdtemp(failure_directory) != NULL);
	char failed_pprof[256];
	char failed_folded[256];
	assert(snprintf(failed_pprof, sizeof(failed_pprof), "%s/profile.pb.gz",
		failure_directory) > 0);
	assert(snprintf(failed_folded, sizeof(failed_folded), "%s/profile.folded",
		failure_directory) > 0);
	assert_failed_export_preserves(&cpu, failed_pprof, LP_EXPORT_PPROF);
	assert_failed_export_preserves(&cpu, failed_folded, LP_EXPORT_FOLDED);
	assert(unlink(failed_pprof) == 0);
	assert(unlink(failed_folded) == 0);
	assert(rmdir(failure_directory) == 0);

	char symlink_directory[] = "/tmp/luaprof-export-symlink-XXXXXX";
	assert(mkdtemp(symlink_directory) != NULL);
	char symlink_target[256];
	char symlink_output[256];
	assert(snprintf(symlink_target, sizeof(symlink_target), "%s/target",
		symlink_directory) > 0);
	assert(snprintf(symlink_output, sizeof(symlink_output), "%s/output",
		symlink_directory) > 0);
	write_text(symlink_target, "symlink-target\n");
	assert(symlink(symlink_target, symlink_output) == 0);
	assert(lp_export_result(&cpu, symlink_output, LP_EXPORT_FOLDED, "samples",
		error, sizeof(error)));
	struct stat link_status;
	assert(lstat(symlink_output, &link_status) == 0);
	assert(S_ISREG(link_status.st_mode));
	assert_mode(symlink_output, 0600);
	assert_text(symlink_target, "symlink-target\n");
	assert(unlink(symlink_output) == 0);
	assert(unlink(symlink_target) == 0);
	assert(mkfifo(symlink_output, 0600) == 0);
	assert(!lp_export_result(&cpu, symlink_output, LP_EXPORT_FOLDED,
		"samples", error, sizeof(error)));
	assert(lstat(symlink_output, &link_status) == 0);
	assert(S_ISFIFO(link_status.st_mode));
	assert(unlink(symlink_output) == 0);
	assert(rmdir(symlink_directory) == 0);
	lp_result_dispose(&cpu);

	lp_result memory = memory_result();
	assert(lp_export_result(&memory, memory_path, LP_EXPORT_PPROF, NULL,
		error, sizeof(error)));
	assert(lp_export_result(&memory, memory_folded, LP_EXPORT_FOLDED,
		"alloc_space", error, sizeof(error)));
	assert(!lp_export_result(&memory, memory_folded, LP_EXPORT_FOLDED,
		"cpu", error, sizeof(error)));
	data = read_gzip(memory_path, &size);
	assert(parse_profile(data, size, 4, &summary));
	assert(summary.comment_count == 1);
	assert_comment(&summary, "luaprof memory sampling profile");
	assert(summary.saw_alloc_space);
	assert(summary.saw_inuse_space);
	assert(summary.saw_memory_source);
	free(data);
	folded = read_text(memory_folded);
	assert(strstr(folded, "memory_work") != NULL);
	assert(strstr(folded, " 192\n") != NULL);
	free(folded);
	lp_result_dispose(&memory);

	if (getenv("LP_KEEP_PPROF_FILES") == NULL) {
		assert(remove(cpu_path) == 0);
		assert(remove(cpu_folded) == 0);
		assert(remove(memory_path) == 0);
		assert(remove(memory_folded) == 0);
		assert(remove(empty_path) == 0);
		assert(remove(large_path) == 0);
		assert(remove(zero_path) == 0);
	}
	puts("luaprof pprof and folded exporter: ok");
	return EXIT_SUCCESS;
}
