#define _GNU_SOURCE

#include "skynet_backend.h"

#include <dlfcn.h>
#include <pthread.h>

typedef const lp_skynet_host_api *(*lp_get_skynet_host_api)(uint32_t);

static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static const lp_skynet_host_api *resolved_api;
static lp_skynet_backend_status resolved_status = LP_SKYNET_BACKEND_ABSENT;

static void
resolve_api(void) {
	dlerror();
	lp_get_skynet_host_api get_api =
		(lp_get_skynet_host_api)dlsym(RTLD_DEFAULT,
			"lp_skynet_host_get_api");
	if (get_api != NULL && dlerror() == NULL) {
		resolved_status = LP_SKYNET_BACKEND_INCOMPATIBLE;
		const lp_skynet_host_api *api =
			get_api(LP_SKYNET_HOST_ABI_VERSION);
		if (api != NULL && api->abi_version == LP_SKYNET_HOST_ABI_VERSION) {
			resolved_api = api;
			resolved_status = LP_SKYNET_BACKEND_COMPATIBLE;
		}
	}
}

lp_skynet_backend_status
lp_skynet_backend_resolve(const lp_skynet_host_api **api) {
	(void)pthread_once(&resolve_once, resolve_api);
	if (api != NULL) {
		*api = resolved_api;
	}
	return resolved_status;
}
