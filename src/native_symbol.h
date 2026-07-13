#ifndef LUAPROF_NATIVE_SYMBOL_H
#define LUAPROF_NATIVE_SYMBOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LP_NATIVE_NAME_CAPACITY 256u
#define LP_NATIVE_PATH_CAPACITY 4096u

typedef struct lp_native_symbol {
	char name[LP_NATIVE_NAME_CAPACITY];
	char path[LP_NATIVE_PATH_CAPACITY];
	size_t name_length;
	size_t path_length;
	uint64_t mapping_start;
	uint64_t mapping_limit;
	uint64_t mapping_offset;
	bool has_mapping;
} lp_native_symbol;

bool lp_native_symbol_resolve(const void *address, lp_native_symbol *symbol);

#endif
