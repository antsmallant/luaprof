#include "lua_symbols.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

static int
aliased_cfunction(lua_State *L) {
	(void)L;
	return 0;
}

static lp_lua_cfunction
global_cfunction(lua_State *L, const char *name) {
	lua_getglobal(L, name);
	assert(lua_iscfunction(L, -1));
	lp_lua_cfunction function = (lp_lua_cfunction)lua_tocfunction(L, -1);
	lua_pop(L, 1);
	return function;
}

static lp_lua_cfunction
field_cfunction(lua_State *L, const char *table, const char *field) {
	lua_getglobal(L, table);
	assert(lua_istable(L, -1));
	lua_getfield(L, -1, field);
	assert(lua_iscfunction(L, -1));
	lp_lua_cfunction function = (lp_lua_cfunction)lua_tocfunction(L, -1);
	lua_pop(L, 2);
	return function;
}

static void
expect_name(lp_lua_symbols *symbols, lp_lua_cfunction function,
	const char *expected) {
	size_t length = 0;
	const char *name = lp_lua_symbols_lookup(symbols, function, &length);
	assert(name != NULL);
	assert(length == strlen(expected));
	assert(memcmp(name, expected, length) == 0);
}

int
main(void) {
	size_t missing_length = 17;
	assert(lp_lua_symbols_count(NULL) == 0);
	assert(!lp_lua_symbols_overflowed(NULL));
	assert(lp_lua_symbols_lookup(NULL, NULL, &missing_length) == NULL);
	assert(missing_length == 0);
	lp_lua_symbols_delete(NULL);

	lua_State *L = luaL_newstate();
	assert(L != NULL);
	luaL_openlibs(L);
	assert(luaL_dostring(L,
		"package.loaded.symbol_demo = { nested = { convert = tostring } }\n") ==
		LUA_OK);
	lua_pushcfunction(L, aliased_cfunction);
	lua_setglobal(L, "zebra_alias");
	lua_pushcfunction(L, aliased_cfunction);
	lua_setglobal(L, "alpha_alias");

	lp_lua_cfunction tostring_function = global_cfunction(L, "tostring");
	lp_lua_cfunction clock_function = field_cfunction(L, "os", "clock");
	lp_lua_symbols *symbols = lp_lua_symbols_collect(L);
	assert(symbols != NULL);
	assert(lp_lua_symbols_count(symbols) > 20);
	expect_name(symbols, tostring_function, "tostring");
	expect_name(symbols, clock_function, "os.clock");
	expect_name(symbols, aliased_cfunction, "alpha_alias");
	assert(!lp_lua_symbols_overflowed(symbols));
	lp_lua_symbols_delete(symbols);

	char long_name[301];
	memset(long_name, 'x', sizeof(long_name) - 1u);
	long_name[sizeof(long_name) - 1u] = '\0';
	lua_pushcfunction(L, aliased_cfunction);
	lua_setglobal(L, long_name);
	symbols = lp_lua_symbols_collect(L);
	assert(symbols != NULL);
	expect_name(symbols, aliased_cfunction, "alpha_alias");
	assert(lp_lua_symbols_overflowed(symbols));
	lp_lua_symbols_delete(symbols);
	lua_close(L);
	puts("luaprof Lua-visible CFunction symbols: ok");
	return EXIT_SUCCESS;
}
