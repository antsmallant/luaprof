#include <stdio.h>
#include <stdlib.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

static void
fail(lua_State *L, const char *message) {
	const char *detail = L == NULL ? NULL : lua_tostring(L, -1);
	fprintf(stderr, "%s%s%s\n", message, detail == NULL ? "" : ": ",
		detail == NULL ? "" : detail);
	if (L != NULL) {
		lua_close(L);
	}
	exit(EXIT_FAILURE);
}

int
main(void) {
	lua_State *L = luaL_newstate();
	if (L == NULL) {
		fail(NULL, "failed to create Lua state");
	}

	luaL_openlibs(L);
	if (luaL_loadstring(L, "return 6 * 7") != LUA_OK ||
		lua_pcall(L, 0, 1, 0) != LUA_OK) {
		fail(L, "failed to run Lua smoke chunk");
	}
	if (!lua_isinteger(L, -1) || lua_tointeger(L, -1) != 42) {
		fail(L, "unexpected Lua result");
	}

	puts("luaprof thread-per-VM smoke: ok");
	lua_close(L);
	return EXIT_SUCCESS;
}
