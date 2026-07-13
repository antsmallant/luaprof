#ifndef LUAPROF_LUA_SYMBOLS_H
#define LUAPROF_LUA_SYMBOLS_H

#include "luaprof/runtime.h"

typedef struct lp_lua_symbols lp_lua_symbols;

lp_lua_symbols *lp_lua_symbols_collect(lua_State *L);
void lp_lua_symbols_delete(lp_lua_symbols *symbols);
const char *lp_lua_symbols_lookup(void *userdata,
	lp_lua_cfunction function, size_t *length);
size_t lp_lua_symbols_count(const lp_lua_symbols *symbols);
bool lp_lua_symbols_overflowed(const lp_lua_symbols *symbols);

#endif
