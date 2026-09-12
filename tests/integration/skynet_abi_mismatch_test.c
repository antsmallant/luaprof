#include "lua_bridge.h"
#include "luaprof/runtime.h"
#include "luaprof/skynet_host.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <lua.h>
#include <lauxlib.h>

const lp_skynet_host_api *
lp_skynet_host_get_api(uint32_t abi_version) {
	assert(abi_version == LP_SKYNET_HOST_ABI_VERSION);
	return NULL; /* An older host does not provide the requested ABI. */
}

int
main(void) {
	lua_State *L = luaL_newstate();
	assert(L != NULL);
	lp_lua_bridge bridge;
	lp_lua_bridge_init(&bridge, L);
	assert(bridge.scheduler_api == NULL);
	assert(bridge.scheduler_incompatible);

	lp_runtime *runtime = lp_runtime_new(L, lp_lua_bridge_host_ops(), &bridge);
	assert(runtime != NULL);
	lp_lua_bridge_bind(&bridge, runtime);
	lp_collector_config config = {
		.kind = LP_COLLECTOR_CPU,
		.value.cpu = { .sample_hz = 100 },
	};
	uint64_t generation = 0;
	assert(lp_runtime_start(runtime, L, &config, &generation) == LP_ERR_HOST);
	assert(bridge.cpu_timer == NULL);
	assert(!bridge.cpu_active);

	lp_runtime_delete(runtime);
	lua_close(L);
	puts("luaprof Skynet ABI mismatch rejection: ok");
	return EXIT_SUCCESS;
}
