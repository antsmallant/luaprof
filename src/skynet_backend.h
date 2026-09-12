#ifndef LUAPROF_SKYNET_BACKEND_H
#define LUAPROF_SKYNET_BACKEND_H

#include "luaprof/skynet_host.h"

typedef enum lp_skynet_backend_status {
	LP_SKYNET_BACKEND_ABSENT = 0,
	LP_SKYNET_BACKEND_COMPATIBLE,
	LP_SKYNET_BACKEND_INCOMPATIBLE,
} lp_skynet_backend_status;

lp_skynet_backend_status lp_skynet_backend_resolve(
	const lp_skynet_host_api **api);

#endif
