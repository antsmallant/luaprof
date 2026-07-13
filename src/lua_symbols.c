#include "lua_symbols.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>

#define LP_LUA_SYMBOL_CAPACITY 4096u
#define LP_LUA_TABLE_CAPACITY 4096u
#define LP_LUA_SYMBOL_MAX_DEPTH 6u
#define LP_LUA_SYMBOL_MAX_NAME 255u

typedef struct lp_lua_symbol_entry {
	lp_lua_cfunction function;
	char name[LP_LUA_SYMBOL_MAX_NAME + 1u];
	uint16_t length;
	bool used;
} lp_lua_symbol_entry;

typedef struct lp_lua_table_entry {
	const void *table;
	char prefix[LP_LUA_SYMBOL_MAX_NAME + 1u];
	uint16_t length;
} lp_lua_table_entry;

struct lp_lua_symbols {
	lp_lua_symbol_entry *entries;
	lp_lua_table_entry *visited;
	size_t count;
	size_t visited_count;
	bool overflowed;
};

static size_t
pointer_hash(const void *pointer) {
	uint64_t value = (uint64_t)(uintptr_t)pointer;
	value ^= value >> 30;
	value *= UINT64_C(0xbf58476d1ce4e5b9);
	value ^= value >> 27;
	value *= UINT64_C(0x94d049bb133111eb);
	value ^= value >> 31;
	return (size_t)value;
}

static bool
better_name(const lp_lua_symbol_entry *entry, const char *name,
	size_t length) {
	return !entry->used || length < entry->length ||
		(length == entry->length && memcmp(name, entry->name, length) < 0);
}

static void
record_name(lp_lua_symbols *symbols, lp_lua_cfunction function,
	const char *name, size_t length) {
	if (function == NULL || name == NULL || length == 0) {
		return;
	}
	if (length > LP_LUA_SYMBOL_MAX_NAME) {
		length = LP_LUA_SYMBOL_MAX_NAME;
		symbols->overflowed = true;
	}
	for (size_t probe = 0; probe < LP_LUA_SYMBOL_CAPACITY; ++probe) {
		size_t index = (pointer_hash((const void *)function) + probe) &
			(LP_LUA_SYMBOL_CAPACITY - 1u);
		lp_lua_symbol_entry *entry = &symbols->entries[index];
		if (entry->used && entry->function != function) {
			continue;
		}
		if (better_name(entry, name, length)) {
			entry->function = function;
			memcpy(entry->name, name, length);
			entry->name[length] = '\0';
			entry->length = (uint16_t)length;
			if (!entry->used) {
				entry->used = true;
				symbols->count++;
			}
		}
		return;
	}
	symbols->overflowed = true;
}

static bool
visit_table(lp_lua_symbols *symbols, const void *table, const char *prefix,
	size_t prefix_length) {
	for (size_t i = 0; i < symbols->visited_count; ++i) {
		lp_lua_table_entry *entry = &symbols->visited[i];
		if (entry->table == table) {
			if (prefix_length > entry->length ||
				(prefix_length == entry->length &&
					memcmp(prefix, entry->prefix, prefix_length) >= 0)) {
				return false;
			}
			memcpy(entry->prefix, prefix, prefix_length);
			entry->prefix[prefix_length] = '\0';
			entry->length = (uint16_t)prefix_length;
			return true;
		}
	}
	if (symbols->visited_count == LP_LUA_TABLE_CAPACITY) {
		symbols->overflowed = true;
		return false;
	}
	lp_lua_table_entry *entry = &symbols->visited[symbols->visited_count++];
	entry->table = table;
	memcpy(entry->prefix, prefix, prefix_length);
	entry->prefix[prefix_length] = '\0';
	entry->length = (uint16_t)prefix_length;
	return true;
}

static bool
make_name(char *output, size_t *output_length, const char *prefix,
	size_t prefix_length, const char *key, size_t key_length) {
	size_t wanted = prefix_length + (prefix_length == 0 ? 0 : 1) + key_length;
	if (wanted > LP_LUA_SYMBOL_MAX_NAME) {
		return false;
	}
	if (prefix_length != 0) {
		memcpy(output, prefix, prefix_length);
		output[prefix_length] = '.';
	}
	memcpy(output + prefix_length + (prefix_length == 0 ? 0 : 1), key,
		key_length);
	output[wanted] = '\0';
	*output_length = wanted;
	return true;
}

static void
scan_table(lua_State *L, int index, lp_lua_symbols *symbols,
	const char *prefix, size_t prefix_length, unsigned int depth) {
	index = lua_absindex(L, index);
	const void *identity = lua_topointer(L, index);
	if (identity == NULL || !visit_table(symbols, identity, prefix,
		prefix_length)) {
		return;
	}
	lua_pushnil(L);
	while (lua_next(L, index) != 0) {
		if (lua_type(L, -2) == LUA_TSTRING) {
			size_t key_length = 0;
			const char *key = lua_tolstring(L, -2, &key_length);
			char name[LP_LUA_SYMBOL_MAX_NAME + 1u];
			size_t name_length = 0;
			if (make_name(name, &name_length, prefix, prefix_length, key,
				key_length)) {
				if (lua_iscfunction(L, -1)) {
					record_name(symbols,
						(lp_lua_cfunction)lua_tocfunction(L, -1), name,
						name_length);
				}
				else if (depth < LP_LUA_SYMBOL_MAX_DEPTH &&
					lua_istable(L, -1)) {
					scan_table(L, -1, symbols, name, name_length, depth + 1u);
				}
			}
			else {
				symbols->overflowed = true;
			}
		}
		lua_pop(L, 1);
	}
}

lp_lua_symbols *
lp_lua_symbols_collect(lua_State *L) {
	if (L == NULL) {
		return NULL;
	}
	lp_lua_symbols *symbols = calloc(1, sizeof(*symbols));
	if (symbols == NULL) {
		return NULL;
	}
	symbols->entries = calloc(LP_LUA_SYMBOL_CAPACITY,
		sizeof(symbols->entries[0]));
	symbols->visited = calloc(LP_LUA_TABLE_CAPACITY,
		sizeof(symbols->visited[0]));
	if (symbols->entries == NULL || symbols->visited == NULL) {
		lp_lua_symbols_delete(symbols);
		return NULL;
	}

	lua_pushglobaltable(L);
	scan_table(L, -1, symbols, "", 0, 0);
	lua_pop(L, 1);

	symbols->visited_count = 0;
	lua_getfield(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
	if (lua_istable(L, -1)) {
		scan_table(L, -1, symbols, "", 0, 0);
	}
	lua_pop(L, 1);
	return symbols;
}

void
lp_lua_symbols_delete(lp_lua_symbols *symbols) {
	if (symbols == NULL) {
		return;
	}
	free(symbols->entries);
	free(symbols->visited);
	free(symbols);
}

const char *
lp_lua_symbols_lookup(void *userdata, lp_lua_cfunction function,
	size_t *length) {
	lp_lua_symbols *symbols = userdata;
	if (length != NULL) {
		*length = 0;
	}
	if (symbols == NULL || function == NULL) {
		return NULL;
	}
	for (size_t probe = 0; probe < LP_LUA_SYMBOL_CAPACITY; ++probe) {
		size_t index = (pointer_hash((const void *)function) + probe) &
			(LP_LUA_SYMBOL_CAPACITY - 1u);
		const lp_lua_symbol_entry *entry = &symbols->entries[index];
		if (!entry->used) {
			return NULL;
		}
		if (entry->function == function) {
			if (length != NULL) {
				*length = entry->length;
			}
			return entry->name;
		}
	}
	return NULL;
}

size_t
lp_lua_symbols_count(const lp_lua_symbols *symbols) {
	return symbols == NULL ? 0 : symbols->count;
}

bool
lp_lua_symbols_overflowed(const lp_lua_symbols *symbols) {
	return symbols != NULL && symbols->overflowed;
}
