#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

enum {
	ITERATIONS = 10000000,
	ROUNDS = 7,
};

static uint64_t
monotonic_ns(void) {
	struct timespec now;
	assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
		(uint64_t)now.tv_nsec;
}

static void
safe_point(void *userdata, lua_State *L, unsigned int pending) {
	(void)userdata;
	(void)L;
	(void)pending;
	assert(!"empty safe-point benchmark invoked its callback");
}

static uint64_t
run(lua_State *L) {
	uint64_t start = monotonic_ns();
	lua_getglobal(L, "work");
	lua_pushinteger(L, ITERATIONS);
	assert(lua_pcall(L, 1, 1, 0) == LUA_OK);
	assert(lua_tointeger(L, -1) == (lua_Integer)ITERATIONS);
	lua_pop(L, 1);
	return monotonic_ns() - start;
}

static int
compare_u64(const void *left, const void *right) {
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;
	return (a > b) - (a < b);
}

static uint64_t
median(uint64_t values[ROUNDS]) {
	qsort(values, ROUNDS, sizeof(values[0]), compare_u64);
	return values[ROUNDS / 2];
}

int
main(void) {
	static const char source[] =
		"function work(n)\n"
		"  local value = 0\n"
		"  for _ = 1, n do value = value + 1 end\n"
		"  return value\n"
		"end\n";
	lua_State *L = luaL_newstate();
	assert(L != NULL);
	assert(luaL_dostring(L, source) == LUA_OK);

	uint64_t disabled[ROUNDS];
	uint64_t safe_empty[ROUNDS];
	lua_ProfileHooks hooks = { .safe_point = safe_point };
	(void)run(L);
	for (int round = 0; round < ROUNDS; ++round) {
		if ((round & 1) == 0) {
			lua_setprofilehooks(L, NULL, NULL);
			disabled[round] = run(L);
			lua_setprofilehooks(L, &hooks, NULL);
			safe_empty[round] = run(L);
		}
		else {
			lua_setprofilehooks(L, &hooks, NULL);
			safe_empty[round] = run(L);
			lua_setprofilehooks(L, NULL, NULL);
			disabled[round] = run(L);
		}
	}
	lua_setprofilehooks(L, NULL, NULL);
	lua_close(L);

	double disabled_ns = (double)median(disabled) / ITERATIONS;
	double safe_empty_ns = (double)median(safe_empty) / ITERATIONS;
	double overhead = (safe_empty_ns / disabled_ns - 1.0) * 100.0;
	printf("disabled: %.3f ns/iteration\n", disabled_ns);
	printf("safe hook, no pending: %.3f ns/iteration\n", safe_empty_ns);
	printf("safe-empty overhead: %.2f%%\n", overhead);
	return 0;
}
