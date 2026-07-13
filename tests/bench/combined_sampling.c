#define _POSIX_C_SOURCE 200809L

#include "lua_bridge.h"
#include "luaprof/runtime.h"

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

enum {
	MODE_DISABLED,
	MODE_CPU,
	MODE_ALLOC_SPACE,
	MODE_INUSE,
	MODE_COMBINED,
	MODE_COUNT,
	ROUNDS = 11,
};

typedef struct benchmark {
	lua_State *L;
	lp_lua_bridge bridge;
	lp_runtime *runtime;
} benchmark;

typedef struct benchmark_stats {
	uint64_t cpu_ticks;
	uint64_t cpu_samples;
	uint64_t state_host;
	uint64_t state_lua;
	uint64_t state_c;
	uint64_t state_gc;
	uint64_t dropped;
	uint64_t unstable;
	uint64_t profiler_overhead;
	uint64_t memory_samples;
	uint64_t aggregate_overflows;
	uint64_t symbol_overflows;
	uint64_t live_map_overflows;
} benchmark_stats;

static const char *mode_names[MODE_COUNT] = {
	"disabled",
	"cpu",
	"alloc-space",
	"in-use",
	"cpu+in-use",
};

static uint64_t
thread_cpu_ns(void) {
	struct timespec now;
	assert(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) == 0);
	return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
		(uint64_t)now.tv_nsec;
}

static int
compare_u64(const void *left, const void *right) {
	uint64_t a = *(const uint64_t *)left;
	uint64_t b = *(const uint64_t *)right;
	return (a > b) - (a < b);
}

static void
open_benchmark(benchmark *bench) {
	memset(bench, 0, sizeof(*bench));
	bench->L = luaL_newstate();
	assert(bench->L != NULL);
	luaL_openlibs(bench->L);
	lp_lua_bridge_init(&bench->bridge, bench->L);
	bench->runtime = lp_runtime_new(bench->L, lp_lua_bridge_host_ops(),
		&bench->bridge);
	assert(bench->runtime != NULL);
	lp_lua_bridge_bind(&bench->bridge, bench->runtime);
	static const char source[] =
		"function benchmark_work()\n"
		"  local checksum = 0\n"
		"  for round = 1, 6 do\n"
		"    local values = {}\n"
		"    for i = 1, 12000 do values[i] = { i, tostring(i) } end\n"
		"    for i = 1, 1200000 do checksum = checksum + i end\n"
		"  end\n"
		"  return checksum\n"
		"end\n";
	assert(luaL_loadbufferx(bench->L, source, sizeof(source) - 1,
		"@combined_benchmark.lua", NULL) == LUA_OK);
	assert(lua_pcall(bench->L, 0, 0, 0) == LUA_OK);
}

static void
close_benchmark(benchmark *bench) {
	lp_runtime_delete(bench->runtime);
	lua_close(bench->L);
}

static uint64_t
start_cpu(benchmark *bench) {
	lp_collector_config config = {
		.kind = LP_COLLECTOR_CPU,
		.value.cpu = { .sample_hz = 1000 },
	};
	uint64_t generation;
	assert(lp_runtime_start(bench->runtime, bench->L, &config, &generation)
		== LP_OK);
	return generation;
}

static uint64_t
start_memory(benchmark *bench, bool track_free) {
	lp_collector_config config = {
		.kind = LP_COLLECTOR_MEMORY,
		.value.memory = {
			.sample_bytes = 512 * 1024,
			.track_free = track_free,
		},
	};
	uint64_t generation;
	assert(lp_runtime_start(bench->runtime, bench->L, &config, &generation)
		== LP_OK);
	return generation;
}

static lp_result_meta
stop(benchmark *bench, lp_collector_kind kind, uint64_t generation) {
	lp_result_meta result;
	assert(lp_runtime_stop(bench->runtime, bench->L, kind, generation,
		&result) == LP_OK);
	return result;
}

static uint64_t
run_mode(benchmark *bench, int mode, benchmark_stats *stats) {
	assert(lua_gc(bench->L, LUA_GCCOLLECT) == 0);
	uint64_t cpu_generation = 0;
	uint64_t memory_generation = 0;
	if (mode == MODE_CPU || mode == MODE_COMBINED) {
		cpu_generation = start_cpu(bench);
	}
	if (mode == MODE_ALLOC_SPACE || mode == MODE_INUSE ||
		mode == MODE_COMBINED) {
		memory_generation = start_memory(bench, mode != MODE_ALLOC_SPACE);
	}
	uint64_t begin = thread_cpu_ns();
	lua_getglobal(bench->L, "benchmark_work");
	assert(lua_pcall(bench->L, 0, 1, 0) == LUA_OK);
	assert(lua_isinteger(bench->L, -1));
	lua_pop(bench->L, 1);
	uint64_t elapsed = thread_cpu_ns() - begin;
	if (cpu_generation != 0) {
		lp_result_meta result = stop(bench, LP_COLLECTOR_CPU,
			cpu_generation);
		stats->cpu_ticks += result.stats.sample_weight;
		stats->cpu_samples += result.stats.samples;
		stats->state_host += result.stats.sample_host;
		stats->state_lua += result.stats.sample_lua;
		stats->state_c += result.stats.sample_c;
		stats->state_gc += result.stats.sample_gc;
		stats->dropped += result.stats.dropped_events;
		stats->unstable += result.stats.unstable_events;
		stats->profiler_overhead += result.stats.profiler_overhead_events;
		stats->aggregate_overflows += result.stats.aggregate_overflows;
		stats->symbol_overflows += result.stats.symbol_overflows;
		lp_result_meta_dispose(&result);
	}
	if (memory_generation != 0) {
		lp_result_meta result = stop(bench, LP_COLLECTOR_MEMORY,
			memory_generation);
		stats->memory_samples += result.stats.memory_samples;
		stats->aggregate_overflows += result.stats.aggregate_overflows;
		stats->symbol_overflows += result.stats.symbol_overflows;
		stats->live_map_overflows += result.stats.live_map_overflows;
		lp_result_meta_dispose(&result);
	}
	return elapsed;
}

int
main(void) {
	benchmark bench;
	open_benchmark(&bench);
	uint64_t elapsed[MODE_COUNT][ROUNDS];
	benchmark_stats stats[MODE_COUNT] = { 0 };
	(void)run_mode(&bench, MODE_DISABLED, &stats[MODE_DISABLED]);
	for (int round = 0; round < ROUNDS; ++round) {
		for (int offset = 0; offset < MODE_COUNT; ++offset) {
			int mode = (round + offset) % MODE_COUNT;
			elapsed[mode][round] = run_mode(&bench, mode, &stats[mode]);
		}
	}
	close_benchmark(&bench);
	uint64_t total_elapsed[MODE_COUNT] = { 0 };
	for (int mode = 0; mode < MODE_COUNT; ++mode) {
		for (int round = 0; round < ROUNDS; ++round) {
			total_elapsed[mode] += elapsed[mode][round];
		}
		qsort(elapsed[mode], ROUNDS, sizeof(elapsed[mode][0]), compare_u64);
	}
	double baseline = (double)elapsed[MODE_DISABLED][ROUNDS / 2];
	for (int mode = 0; mode < MODE_COUNT; ++mode) {
		double median_ms = (double)elapsed[mode][ROUNDS / 2] / 1000000.0;
		double overhead = (double)elapsed[mode][ROUNDS / 2] / baseline - 1.0;
		double effective_hz = total_elapsed[mode] == 0 ? 0.0 :
			(double)stats[mode].cpu_ticks * 1000000000.0 /
			(double)total_elapsed[mode];
		printf("%-12s %8.2f ms  %+6.2f%%  cpu=%" PRIu64 "/%" PRIu64
			" (%.0fHz L/C/G/H=%" PRIu64 "/%" PRIu64 "/%" PRIu64
			"/%" PRIu64 ", drop/unstable/overhead=%" PRIu64 "/%" PRIu64
			"/%" PRIu64 ") mem=%" PRIu64 " overflow=A/S/L=%" PRIu64
			"/%" PRIu64 "/%" PRIu64 "\n", mode_names[mode], median_ms,
			overhead * 100.0, stats[mode].cpu_ticks,
			stats[mode].cpu_samples, effective_hz, stats[mode].state_lua,
			stats[mode].state_c, stats[mode].state_gc,
			stats[mode].state_host, stats[mode].dropped,
			stats[mode].unstable, stats[mode].profiler_overhead,
			stats[mode].memory_samples, stats[mode].aggregate_overflows,
			stats[mode].symbol_overflows, stats[mode].live_map_overflows);
	}
	return EXIT_SUCCESS;
}
