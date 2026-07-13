#include "luaprof/runtime.h"

#include <stdlib.h>
#include <string.h>

typedef struct lp_collector_slot {
	bool active;
	uint64_t generation;
	lp_collector_config config;
} lp_collector_slot;

struct lp_profile_model {
	uint64_t next_symbol_id;
	uint64_t next_stack_id;
};

struct lp_runtime {
	lua_State *main_state;
	lp_host_ops host_ops;
	void *host_userdata;
	uint64_t next_generation;
	bool closing;
	lp_profile_model model;
	lp_collector_slot collectors[LP_COLLECTOR_COUNT];
};

static bool
valid_kind(lp_collector_kind kind) {
	return kind >= LP_COLLECTOR_CPU && kind < LP_COLLECTOR_COUNT;
}

lp_runtime *
lp_runtime_new(lua_State *main_state, const lp_host_ops *host_ops,
	void *host_userdata) {
	lp_runtime *runtime = calloc(1, sizeof(*runtime));
	if (runtime == NULL) {
		return NULL;
	}

	runtime->main_state = main_state;
	runtime->host_userdata = host_userdata;
	runtime->model.next_symbol_id = 1;
	runtime->model.next_stack_id = 1;
	if (host_ops != NULL) {
		runtime->host_ops = *host_ops;
	}
	return runtime;
}

void
lp_runtime_delete(lp_runtime *runtime) {
	if (runtime == NULL) {
		return;
	}

	runtime->closing = true;
	for (int i = 0; i < LP_COLLECTOR_COUNT; ++i) {
		lp_collector_slot *slot = &runtime->collectors[i];
		if (!slot->active) {
			continue;
		}
		slot->active = false;
		if (runtime->host_ops.stop_collector != NULL) {
			runtime->host_ops.stop_collector(runtime->host_userdata, runtime,
				(lp_collector_kind)i, slot->generation);
		}
	}
	free(runtime);
}

lp_status
lp_runtime_start(lp_runtime *runtime, const lp_collector_config *config,
	uint64_t *generation) {
	if (runtime == NULL || config == NULL || generation == NULL ||
		!valid_kind(config->kind)) {
		return LP_ERR_ARGUMENT;
	}
	if (config->kind == LP_COLLECTOR_MEMORY &&
		config->value.memory.sample_bytes == 0) {
		return LP_ERR_ARGUMENT;
	}
	if (runtime->closing) {
		return LP_ERR_CLOSED;
	}

	lp_collector_slot *slot = &runtime->collectors[config->kind];
	if (slot->active) {
		return LP_ERR_BUSY;
	}

	uint64_t next = ++runtime->next_generation;
	if (next == 0) {
		next = ++runtime->next_generation;
	}
	slot->active = true;
	slot->generation = next;
	slot->config = *config;

	if (runtime->host_ops.start_collector != NULL) {
		lp_status status = runtime->host_ops.start_collector(
			runtime->host_userdata, runtime, next, config);
		if (status != LP_OK) {
			memset(slot, 0, sizeof(*slot));
			return status == LP_ERR_NOMEM ? status : LP_ERR_HOST;
		}
	}

	*generation = next;
	return LP_OK;
}

lp_status
lp_runtime_stop(lp_runtime *runtime, lp_collector_kind kind,
	uint64_t generation, lp_result_meta *result) {
	if (runtime == NULL || result == NULL || !valid_kind(kind) ||
		generation == 0) {
		return LP_ERR_ARGUMENT;
	}
	if (runtime->closing) {
		return LP_ERR_CLOSED;
	}

	lp_collector_slot *slot = &runtime->collectors[kind];
	if (!slot->active) {
		return LP_ERR_INACTIVE;
	}
	if (slot->generation != generation) {
		return LP_ERR_STALE;
	}

	result->kind = kind;
	result->generation = generation;
	result->config = slot->config;

	/* Reject new events before the host synchronously removes callbacks. */
	slot->active = false;
	if (runtime->host_ops.stop_collector != NULL) {
		runtime->host_ops.stop_collector(runtime->host_userdata, runtime,
			kind, generation);
	}
	memset(slot, 0, sizeof(*slot));
	return LP_OK;
}

bool
lp_runtime_active(const lp_runtime *runtime, lp_collector_kind kind) {
	return runtime != NULL && valid_kind(kind) &&
		runtime->collectors[kind].active;
}

uint64_t
lp_runtime_generation(const lp_runtime *runtime, lp_collector_kind kind) {
	if (runtime == NULL || !valid_kind(kind)) {
		return 0;
	}
	return runtime->collectors[kind].generation;
}

lua_State *
lp_runtime_main_state(const lp_runtime *runtime) {
	return runtime == NULL ? NULL : runtime->main_state;
}

lp_profile_model *
lp_runtime_model(lp_runtime *runtime) {
	return runtime == NULL ? NULL : &runtime->model;
}

const char *
lp_status_string(lp_status status) {
	switch (status) {
	case LP_OK:
		return "ok";
	case LP_ERR_ARGUMENT:
		return "invalid argument";
	case LP_ERR_BUSY:
		return "collector is already running";
	case LP_ERR_INACTIVE:
		return "collector is not running";
	case LP_ERR_STALE:
		return "stale recorder generation";
	case LP_ERR_HOST:
		return "host failed to start collector";
	case LP_ERR_CLOSED:
		return "profile runtime is closed";
	case LP_ERR_NOMEM:
		return "out of memory";
	default:
		return "unknown error";
	}
}
