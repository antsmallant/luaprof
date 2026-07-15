#define _POSIX_C_SOURCE 200809L

#include "lua_bridge.h"
#include "luaprof/runtime.h"
#include "luaprof/skynet_host.h"
#include "skynet_host_test.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#define TARGET_HANDLE UINT32_C(0x100)
#define OTHER_HANDLE UINT32_C(0x200)
#define TRANSITION_HANDLE UINT32_C(0x500)
#define REUSE_HANDLE UINT32_C(0x600)
#define REUSE_ROUNDS 1000u

typedef struct scheduler_test {
	lua_State *L;
	lp_lua_bridge bridge;
	lp_runtime *runtime;
	uint64_t generation;
	lp_result result;
} scheduler_test;

typedef struct concurrent_test {
	scheduler_test *test;
	uint32_t handle;
	unsigned int worker_id;
} concurrent_test;

typedef struct reuse_test {
	scheduler_test target;
	pthread_barrier_t barrier;
	_Atomic bool done;
} reuse_test;

static void
run_chunk(lua_State *L, const char *source, const char *name) {
	assert(luaL_loadbufferx(L, source, strlen(source), name, NULL) == LUA_OK);
	if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
		fprintf(stderr, "%s\n", lua_tostring(L, -1));
		assert(0);
	}
}

static void
run_target_work(lua_State *L) {
	run_chunk(L,
		"local value = 0\n"
		"for i = 1, 30000000 do value = value + i end\n"
		"assert(value > 0)\n",
		"@scheduler_migration.lua");
}

static void
open_test(scheduler_test *test) {
	memset(test, 0, sizeof(*test));
	test->L = luaL_newstate();
	assert(test->L != NULL);
	luaL_openlibs(test->L);
	lp_lua_bridge_init(&test->bridge, test->L);
	assert(test->bridge.scheduler_api != NULL);
	test->runtime = lp_runtime_new(test->L, lp_lua_bridge_host_ops(),
		&test->bridge);
	assert(test->runtime != NULL);
	lp_lua_bridge_bind(&test->bridge, test->runtime);
}

static void
close_test(scheduler_test *test) {
	lp_result_dispose(&test->result);
	if (test->runtime != NULL) {
		lp_runtime_delete(test->runtime);
	}
	lua_close(test->L);
}

static int
result_has_source(const lp_result *result, const char *source) {
	size_t source_length = strlen(source);
	for (size_t i = 0; i < lp_result_cpu_sample_count(result); ++i) {
		lp_cpu_sample_view sample;
		assert(lp_result_cpu_sample(result, i, &sample));
		for (size_t j = 0; j < sample.depth; ++j) {
			lp_cpu_frame_view frame;
			assert(lp_result_cpu_frame(result, i, j, &frame));
			if (frame.source != NULL &&
				frame.source_length == source_length &&
				memcmp(frame.source, source, source_length) == 0) {
				return 1;
			}
		}
	}
	return 0;
}

static void *
first_worker(void *argument) {
	scheduler_test *test = argument;
	lp_skynet_host_worker_start(0);
	lp_skynet_host_dispatch_enter(TARGET_HANDLE);
	lp_collector_config config = {
		.kind = LP_COLLECTOR_CPU,
		.value.cpu = { .sample_hz = 1000 },
	};
	assert(lp_runtime_start(test->runtime, test->L, &config,
		&test->generation) == LP_OK);
	run_target_work(test->L);
	sigset_t set;
	sigset_t previous;
	sigemptyset(&set);
	sigaddset(&set, SIGRTMAX - 3);
	assert(pthread_sigmask(SIG_BLOCK, &set, &previous) == 0);
	run_target_work(test->L);
	lp_skynet_host_dispatch_leave();
	assert(pthread_sigmask(SIG_SETMASK, &previous, NULL) == 0);
	lp_skynet_host_worker_stop();
	return NULL;
}

static void *
second_worker(void *argument) {
	scheduler_test *test = argument;
	lp_skynet_host_worker_start(1);

	/* An unrelated dispatch must not publish the profiled VM. */
	lp_skynet_host_dispatch_enter(OTHER_HANDLE);
	lua_State *other = luaL_newstate();
	assert(other != NULL);
	run_chunk(other,
		"local value = 0\n"
		"for i = 1, 10000000 do value = value + i end\n",
		"@scheduler_unrelated.lua");
	lua_close(other);
	lp_skynet_host_dispatch_leave();

	lp_skynet_host_dispatch_enter(TARGET_HANDLE);
	run_target_work(test->L);
	assert(lp_runtime_stop(test->runtime, test->L, LP_COLLECTOR_CPU,
		test->generation, &test->result) == LP_OK);
	lp_skynet_host_dispatch_leave();
	lp_skynet_host_worker_stop();
	return NULL;
}

static void
test_migration(void) {
	scheduler_test test;
	open_test(&test);
	pthread_t thread;
	assert(pthread_create(&thread, NULL, first_worker, &test) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(pthread_create(&thread, NULL, second_worker, &test) == 0);
	assert(pthread_join(thread, NULL) == 0);

	assert(test.result.stats.sample_lua >= 20);
	assert(test.result.stats.scheduler_workers == 2);
	assert(test.result.stats.stale_events > 0);
	assert(result_has_source(&test.result, "@scheduler_migration.lua"));
	assert(!result_has_source(&test.result, "@scheduler_unrelated.lua"));
	close_test(&test);
}

static void *
concurrent_worker(void *argument) {
	concurrent_test *concurrent = argument;
	scheduler_test *test = concurrent->test;
	lp_skynet_host_worker_start(concurrent->worker_id);
	lp_skynet_host_dispatch_enter(concurrent->handle);
	lp_collector_config config = {
		.kind = LP_COLLECTOR_CPU,
		.value.cpu = { .sample_hz = 1000 },
	};
	assert(lp_runtime_start(test->runtime, test->L, &config,
		&test->generation) == LP_OK);
	run_target_work(test->L);
	assert(lp_runtime_stop(test->runtime, test->L, LP_COLLECTOR_CPU,
		test->generation, &test->result) == LP_OK);
	lp_skynet_host_dispatch_leave();
	lp_skynet_host_worker_stop();
	return NULL;
}

static void
test_concurrent_targets(void) {
	scheduler_test tests[2];
	concurrent_test arguments[2];
	pthread_t threads[2];
	for (size_t i = 0; i < 2; ++i) {
		open_test(&tests[i]);
		arguments[i].test = &tests[i];
		arguments[i].handle = UINT32_C(0x300) + (uint32_t)i;
		arguments[i].worker_id = (unsigned int)i + 2;
		assert(pthread_create(&threads[i], NULL, concurrent_worker,
			&arguments[i]) == 0);
	}
	for (size_t i = 0; i < 2; ++i) {
		assert(pthread_join(threads[i], NULL) == 0);
		assert(tests[i].result.stats.sample_lua >= 10);
		assert(tests[i].result.stats.scheduler_workers == 1);
		assert(tests[i].result.stats.stale_events == 0);
		close_test(&tests[i]);
	}
}

static void *
destroy_active_worker(void *argument) {
	scheduler_test *test = argument;
	lp_skynet_host_worker_start(4);
	lp_skynet_host_dispatch_enter(UINT32_C(0x400));
	lp_collector_config config = {
		.kind = LP_COLLECTOR_CPU,
		.value.cpu = { .sample_hz = 1000 },
	};
	assert(lp_runtime_start(test->runtime, test->L, &config,
		&test->generation) == LP_OK);
	run_target_work(test->L);
	lp_runtime_delete(test->runtime);
	test->runtime = NULL;
	lp_skynet_host_dispatch_leave();
	lp_skynet_host_worker_stop();
	return NULL;
}

static void
test_destroy_active_runtime(void) {
	scheduler_test test;
	open_test(&test);
	pthread_t thread;
	assert(pthread_create(&thread, NULL, destroy_active_worker, &test) == 0);
	assert(pthread_join(thread, NULL) == 0);
	close_test(&test);
}

static void *
transition_tick_worker(void *argument) {
	scheduler_test *test = argument;
	lp_skynet_host_worker_start(5);
	lp_skynet_host_dispatch_enter(TRANSITION_HANDLE);
	lp_collector_config config = {
		.kind = LP_COLLECTOR_CPU,
		.value.cpu = { .sample_hz = 1000 },
	};
	assert(lp_runtime_start(test->runtime, test->L, &config,
		&test->generation) == LP_OK);
	lp_skynet_host_test_inject_transition_tick();
	lp_skynet_host_dispatch_leave();
	lp_skynet_host_dispatch_enter(TRANSITION_HANDLE);
	assert(lp_runtime_stop(test->runtime, test->L, LP_COLLECTOR_CPU,
		test->generation, &test->result) == LP_OK);
	lp_skynet_host_dispatch_leave();
	lp_skynet_host_worker_stop();
	return NULL;
}

static void
test_transition_tick_accounting(void) {
	scheduler_test test;
	open_test(&test);
	pthread_t thread;
	assert(pthread_create(&thread, NULL, transition_tick_worker, &test) == 0);
	assert(pthread_join(thread, NULL) == 0);
	assert(test.result.stats.unstable_events >= 1);
	close_test(&test);
}

static void
wait_reuse_barrier(pthread_barrier_t *barrier) {
	int status = pthread_barrier_wait(barrier);
	assert(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void *
reuse_owner_worker(void *argument) {
	reuse_test *test = argument;
	lp_skynet_host_worker_start(6);
	wait_reuse_barrier(&test->barrier);
	for (unsigned int i = 0; i < REUSE_ROUNDS; ++i) {
		lp_skynet_host_dispatch_enter(REUSE_HANDLE);
		lp_collector_config config = {
			.kind = LP_COLLECTOR_CPU,
			.value.cpu = { .sample_hz = 1000 },
		};
		uint64_t generation = 0;
		assert(lp_runtime_start(test->target.runtime, test->target.L, &config,
			&generation) == LP_OK);
		lp_result result = { 0 };
		assert(lp_runtime_stop(test->target.runtime, test->target.L,
			LP_COLLECTOR_CPU, generation, &result) == LP_OK);
		lp_result_dispose(&result);
		lp_skynet_host_dispatch_leave();
	}
	atomic_store_explicit(&test->done, true, memory_order_release);
	lp_skynet_host_worker_stop();
	return NULL;
}

static void *
reuse_scanner_worker(void *argument) {
	reuse_test *test = argument;
	lp_skynet_host_worker_start(7);
	wait_reuse_barrier(&test->barrier);
	uint32_t sequence = 0;
	while (!atomic_load_explicit(&test->done, memory_order_acquire)) {
		lp_skynet_host_dispatch_enter(OTHER_HANDLE + (sequence++ & 255u));
		lp_skynet_host_dispatch_leave();
		if ((sequence & 255u) == 0) {
			(void)sched_yield();
		}
	}
	lp_skynet_host_worker_stop();
	return NULL;
}

static void
test_target_reuse_with_concurrent_dispatch(void) {
	reuse_test test;
	memset(&test, 0, sizeof(test));
	open_test(&test.target);
	assert(pthread_barrier_init(&test.barrier, NULL, 2) == 0);
	pthread_t owner;
	pthread_t scanner;
	assert(pthread_create(&owner, NULL, reuse_owner_worker, &test) == 0);
	assert(pthread_create(&scanner, NULL, reuse_scanner_worker, &test) == 0);
	assert(pthread_join(owner, NULL) == 0);
	assert(pthread_join(scanner, NULL) == 0);
	assert(pthread_barrier_destroy(&test.barrier) == 0);
	close_test(&test.target);
}

int
main(void) {
	test_migration();
	test_concurrent_targets();
	test_destroy_active_runtime();
	test_transition_tick_accounting();
	test_target_reuse_with_concurrent_dispatch();
	puts("luaprof scheduler CPU sampling: ok");
	return EXIT_SUCCESS;
}
