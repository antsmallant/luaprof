#include "luaprof/runtime.h"
#include "pprof_exporter.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

typedef struct profile_summary {
	size_t sample_types;
	size_t samples;
	size_t locations;
	size_t functions;
	size_t strings;
	size_t expected_values;
	uint64_t max_location_reference;
	uint64_t max_function_reference;
	uint64_t default_sample_type;
	bool saw_period_type;
	bool saw_period;
	bool saw_empty_string_first;
	bool saw_cpu;
	bool saw_alloc_space;
	bool saw_inuse_space;
	bool saw_cpu_source;
	bool saw_memory_source;
	bool saw_cfunction;
} profile_summary;

static int
profiled_cfunction(lua_State *L) {
	(void)L;
	return 0;
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
			if (field == 2 && !parse_packed(packed, length, &values, NULL)) {
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
		if ((field == 1 || field == 2 || field == 4 || field == 5 ||
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
			else if (field == 5) {
				summary->functions++;
				if (!parse_function(message, length)) {
					return false;
				}
			}
			else if (field == 6) {
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
					bytes_contain(message, length, "lua_CFunction@0x");
			}
			else {
				summary->saw_period_type = true;
			}
		}
		else if ((field == 12 || field == 14) && wire == 0) {
			uint64_t value;
			if (!read_varint(data, size, &offset, &value)) {
				return false;
			}
			if (field == 12) {
				summary->saw_period = value != 0;
			}
			else {
				summary->default_sample_type = value;
			}
		}
		else if (!skip_field(data, size, &offset, wire)) {
			return false;
		}
	}
	return summary->sample_types == values && summary->samples != 0 &&
		summary->locations != 0 && summary->functions != 0 &&
		summary->saw_empty_string_first && summary->saw_period_type &&
		summary->saw_period && summary->default_sample_type < summary->strings &&
		summary->max_location_reference <= summary->locations &&
		summary->max_function_reference <= summary->functions;
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

static lp_result_meta
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
		.linedefined = 10,
		.currentline = 12,
	};
	lp_runtime_cpu_sample(runtime, generation, LP_VM_C, profiled_cfunction,
		&frame, 1, false, 3);
	lp_runtime_cpu_sample(runtime, generation, LP_VM_GC, NULL, &frame, 1,
		false, 2);
	lp_result_meta result;
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

static lp_result_meta
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
		.linedefined = 20,
		.currentline = 22,
	};
	void *first = (void *)(uintptr_t)0x1000;
	void *second = (void *)(uintptr_t)0x2000;
	record_allocation(runtime, generation, first, 64, &frame);
	record_allocation(runtime, generation, second, 128, &frame);
	lp_runtime_allocation(runtime, generation, NULL, second, NULL, 128, 0,
		true);
	lp_result_meta result;
	assert(lp_runtime_stop(runtime, NULL, LP_COLLECTOR_MEMORY, generation,
		&result) == LP_OK);
	lp_runtime_delete(runtime);
	return result;
}

int
main(void) {
	const char *cpu_path = "/tmp/luaprof-pprof-cpu.pb.gz";
	const char *cpu_folded = "/tmp/luaprof-pprof-cpu.folded";
	const char *memory_path = "/tmp/luaprof-pprof-memory.pb.gz";
	const char *memory_folded = "/tmp/luaprof-pprof-memory.folded";
	char error[256];

	lp_result_meta cpu = cpu_result();
	assert(lp_export_result(&cpu, cpu_path, LP_EXPORT_PPROF, NULL, error,
		sizeof(error)));
	assert(lp_export_result(&cpu, cpu_folded, LP_EXPORT_FOLDED, "samples",
		error, sizeof(error)));
	size_t size;
	unsigned char *data = read_gzip(cpu_path, &size);
	profile_summary summary;
	assert(parse_profile(data, size, 2, &summary));
	assert(summary.saw_cpu);
	assert(summary.saw_cpu_source);
	assert(summary.saw_cfunction);
	free(data);
	char *folded = read_text(cpu_folded);
	assert(strstr(folded, "pprof_cpu.lua") != NULL);
	assert(strstr(folded, "lua_CFunction@0x") != NULL);
	free(folded);
	lp_result_meta_dispose(&cpu);

	lp_result_meta memory = memory_result();
	assert(lp_export_result(&memory, memory_path, LP_EXPORT_PPROF, NULL,
		error, sizeof(error)));
	assert(lp_export_result(&memory, memory_folded, LP_EXPORT_FOLDED,
		"alloc_space", error, sizeof(error)));
	assert(!lp_export_result(&memory, memory_folded, LP_EXPORT_FOLDED,
		"cpu", error, sizeof(error)));
	data = read_gzip(memory_path, &size);
	assert(parse_profile(data, size, 4, &summary));
	assert(summary.saw_alloc_space);
	assert(summary.saw_inuse_space);
	assert(summary.saw_memory_source);
	free(data);
	folded = read_text(memory_folded);
	assert(strstr(folded, "pprof_memory.lua") != NULL);
	assert(strstr(folded, " 192\n") != NULL);
	free(folded);
	lp_result_meta_dispose(&memory);

	if (getenv("LP_KEEP_PPROF_FILES") == NULL) {
		assert(remove(cpu_path) == 0);
		assert(remove(cpu_folded) == 0);
		assert(remove(memory_path) == 0);
		assert(remove(memory_folded) == 0);
	}
	puts("luaprof pprof and folded exporter: ok");
	return EXIT_SUCCESS;
}
