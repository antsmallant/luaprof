#define _POSIX_C_SOURCE 200809L

#include "lua_bridge.h"
#include "luaprof/runtime.h"
#include "luaprof/skynet_host.h"

#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#define MASKED_TARGET_HANDLE UINT32_C(0x700)

typedef struct signal_mask_test {
	lua_State *L;
	lp_lua_bridge bridge;
	lp_runtime *runtime;
	lp_status start_status;
} signal_mask_test;

static void *
masked_worker(void *argument) {
	signal_mask_test *test = argument;
	sigset_t set;
	sigset_t previous;
	sigemptyset(&set);
	sigaddset(&set, SIGRTMAX - 3);
	assert(pthread_sigmask(SIG_BLOCK, &set, &previous) == 0);

	lp_skynet_host_worker_start(0);
	sigset_t current;
	assert(pthread_sigmask(SIG_SETMASK, NULL, &current) == 0);
	assert(sigismember(&current, SIGRTMAX - 3) == 1);

	lp_skynet_host_dispatch_enter(MASKED_TARGET_HANDLE);
	lp_collector_config config = {
		.kind = LP_COLLECTOR_CPU,
		.value.cpu = { .sample_hz = 1000 },
	};
	uint64_t generation = 0;
	test->start_status = lp_runtime_start(test->runtime, test->L, &config,
		&generation);
	lp_skynet_host_dispatch_leave();
	lp_skynet_host_worker_stop();
	assert(pthread_sigmask(SIG_SETMASK, &previous, NULL) == 0);
	return NULL;
}

int
main(void) {
	signal_mask_test test;
	memset(&test, 0, sizeof(test));
	test.L = luaL_newstate();
	assert(test.L != NULL);
	luaL_openlibs(test.L);
	lp_lua_bridge_init(&test.bridge, test.L);
	assert(test.bridge.scheduler_api != NULL);
	test.runtime = lp_runtime_new(test.L, lp_lua_bridge_host_ops(),
		&test.bridge);
	assert(test.runtime != NULL);
	lp_lua_bridge_bind(&test.bridge, test.runtime);

	pthread_t thread;
	assert(pthread_create(&thread, NULL, masked_worker, &test) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(test.start_status == LP_ERR_HOST);

	lp_runtime_delete(test.runtime);
	lua_close(test.L);
	puts("luaprof Skynet blocked signal rejection: ok");
	return EXIT_SUCCESS;
}
