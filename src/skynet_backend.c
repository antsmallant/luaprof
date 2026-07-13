#define _GNU_SOURCE

#include "skynet_backend.h"

#include <dlfcn.h>
#include <pthread.h>

typedef const lp_skynet_host_api *(*lp_get_skynet_host_api)(uint32_t);

static pthread_once_t resolve_once = PTHREAD_ONCE_INIT;
static const lp_skynet_host_api *resolved_api;

static void
resolve_api(void) {
	dlerror();
	lp_get_skynet_host_api get_api =
		(lp_get_skynet_host_api)dlsym(RTLD_DEFAULT,
			"lp_skynet_host_get_api");
	if (get_api != NULL && dlerror() == NULL) {
		const lp_skynet_host_api *api =
			get_api(LP_SKYNET_HOST_ABI_VERSION);
		if (api != NULL && api->abi_version == LP_SKYNET_HOST_ABI_VERSION) {
			resolved_api = api;
		}
	}
}

const lp_skynet_host_api *
lp_skynet_backend_api(void) {
	(void)pthread_once(&resolve_once, resolve_api);
	return resolved_api;
}
