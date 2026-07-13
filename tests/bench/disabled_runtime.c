#define _POSIX_C_SOURCE 200809L

#include "luaprof/runtime.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define ITERATIONS UINT64_C(10000000)

static uint64_t
nanoseconds(struct timespec value) {
	return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
		(uint64_t)value.tv_nsec;
}

int
main(void) {
	lp_runtime *runtime = lp_runtime_new(NULL, NULL, NULL);
	if (runtime == NULL) {
		return 1;
	}

	struct timespec begin;
	struct timespec end;
	volatile uint64_t active_count = 0;
	clock_gettime(CLOCK_MONOTONIC, &begin);
	for (uint64_t i = 0; i < ITERATIONS; ++i) {
		active_count += lp_runtime_active(runtime, LP_COLLECTOR_CPU);
	}
	clock_gettime(CLOCK_MONOTONIC, &end);

	uint64_t elapsed = nanoseconds(end) - nanoseconds(begin);
	printf("disabled runtime check: %.2f ns/op (%" PRIu64 " active)\n",
		(double)elapsed / (double)ITERATIONS, active_count);
	lp_runtime_delete(runtime);
	return active_count == 0 ? 0 : 1;
}
