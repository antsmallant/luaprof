#define _GNU_SOURCE

#include "luaprof/skynet_host.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <lua.h>

#define LP_SKYNET_RING_CAPACITY 4096u
#define LP_SKYNET_TARGET_CAPACITY 128u
#define LP_SKYNET_WORKER_CAPACITY 64u

enum lp_skynet_target_state {
	LP_SKYNET_TARGET_FREE = 0,
	LP_SKYNET_TARGET_ACTIVE = 1,
	LP_SKYNET_TARGET_STOPPING = 2
};

typedef struct lp_skynet_target {
	_Atomic int state;
	uint32_t handle;
	uint64_t token;
	uint64_t generation;
	uint32_t sample_hz;
	lua_State *main_state;
	lp_skynet_tick_event *events;
	_Atomic uint64_t write_sequence;
	_Atomic uint64_t read_sequence;
	_Atomic uint64_t dropped;
	_Atomic uint64_t unstable;
	_Atomic uint64_t profiler_overhead;
	_Atomic uint64_t stale;
	_Atomic uint64_t worker_mask;
	_Atomic bool draining_events;
} lp_skynet_target;

typedef struct lp_skynet_worker {
	timer_t timer_id;
	unsigned int worker_id;
	unsigned int armed_hz;
	int signal_slot;
	_Atomic bool active;
	_Atomic uint64_t slot_version;
	_Atomic(lp_skynet_target *) slot_target;
	_Atomic uint64_t slot_token;
	_Atomic(lua_State *) slot_state;
	_Atomic(lp_skynet_lua_cfunction) slot_cfunction;
	_Atomic int slot_vm_state;
} lp_skynet_worker;

_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2,
	"Skynet host requires lock-free 64-bit atomics");
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2,
	"Skynet host requires lock-free pointer atomics");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
	"Skynet host requires lock-free integer atomics");
_Static_assert((LP_SKYNET_RING_CAPACITY &
	(LP_SKYNET_RING_CAPACITY - 1u)) == 0,
	"Skynet event ring capacity must be a power of two");

static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
static lp_skynet_target targets[LP_SKYNET_TARGET_CAPACITY];
static _Atomic(lp_skynet_worker *) workers[LP_SKYNET_WORKER_CAPACITY];
static _Atomic unsigned int active_targets;
static _Atomic unsigned int active_sample_hz;
static _Atomic bool host_failed;
static uint64_t next_token;
static unsigned int signal_users;
static int host_signal;
static struct sigaction saved_signal_action;
static _Thread_local lp_skynet_worker *current_worker;
static _Thread_local uint32_t current_handle;

static uint64_t
saturating_add(uint64_t value, uint64_t increment) {
	return UINT64_MAX - value < increment ? UINT64_MAX : value + increment;
}

static void
add_quality(_Atomic uint64_t *counter, uint64_t value) {
	uint64_t old = atomic_load_explicit(counter, memory_order_relaxed);
	for (;;) {
		uint64_t next = saturating_add(old, value);
		if (atomic_compare_exchange_weak_explicit(counter, &old, next,
			memory_order_relaxed, memory_order_relaxed)) {
			return;
		}
	}
}

static unsigned int
event_weight(const siginfo_t *info) {
	int overrun = info == NULL ? 0 : info->si_overrun;
	return overrun < 0 ? 1u : (unsigned int)overrun + 1u;
}

static bool
registered_worker(lp_skynet_worker *worker) {
	for (size_t i = 0; i < LP_SKYNET_WORKER_CAPACITY; ++i) {
		if (atomic_load_explicit(&workers[i],
			memory_order_acquire) == worker) {
			return true;
		}
	}
	return false;
}

static void
timer_signal_handler(int signal_number, siginfo_t *info, void *context) {
	(void)signal_number;
	(void)context;
	int saved_errno = errno;
	if (info == NULL || info->si_code != SI_TIMER) {
		errno = saved_errno;
		return;
	}

	lp_skynet_worker *worker = info->si_value.sival_ptr;
	if (worker == NULL || !registered_worker(worker) ||
		!atomic_load_explicit(&worker->active, memory_order_acquire)) {
		errno = saved_errno;
		return;
	}

	unsigned int weight = event_weight(info);
	uint64_t version = atomic_load_explicit(&worker->slot_version,
		memory_order_acquire);
	if ((version & 1u) != 0) {
		errno = saved_errno;
		return;
	}
	lp_skynet_target *target = atomic_load_explicit(&worker->slot_target,
		memory_order_relaxed);
	uint64_t token = atomic_load_explicit(&worker->slot_token,
		memory_order_relaxed);
	lua_State *L = atomic_load_explicit(&worker->slot_state,
		memory_order_relaxed);
	lp_skynet_lua_cfunction cfunction = atomic_load_explicit(
		&worker->slot_cfunction, memory_order_relaxed);
	int vm_state = atomic_load_explicit(&worker->slot_vm_state,
		memory_order_relaxed);
	if (version != atomic_load_explicit(&worker->slot_version,
		memory_order_acquire)) {
		if (target != NULL) {
			add_quality(&target->unstable, weight);
		}
		errno = saved_errno;
		return;
	}
	if (target == NULL || L == NULL) {
		errno = saved_errno;
		return;
	}
	if (atomic_load_explicit(&target->state, memory_order_acquire) !=
		LP_SKYNET_TARGET_ACTIVE || target->token != token) {
		add_quality(&target->stale, weight);
		errno = saved_errno;
		return;
	}
	if (atomic_load_explicit(&target->draining_events, memory_order_acquire)) {
		add_quality(&target->profiler_overhead, weight);
		errno = saved_errno;
		return;
	}
	if (vm_state < 0 || vm_state > 3 ||
		(vm_state == 2 && cfunction == NULL)) {
		add_quality(&target->unstable, weight);
		errno = saved_errno;
		return;
	}

	uint64_t write = atomic_load_explicit(&target->write_sequence,
		memory_order_relaxed);
	uint64_t read = atomic_load_explicit(&target->read_sequence,
		memory_order_acquire);
	if (write - read >= LP_SKYNET_RING_CAPACITY) {
		add_quality(&target->dropped, weight);
	}
	else {
		lp_skynet_tick_event *event =
			&target->events[write & (LP_SKYNET_RING_CAPACITY - 1u)];
		event->state = L;
		event->vm_state = vm_state;
		event->cfunction = cfunction;
		event->weight = weight;
		if (worker->worker_id < 64) {
			atomic_fetch_or_explicit(&target->worker_mask,
				UINT64_C(1) << worker->worker_id,
				memory_order_relaxed);
		}
		atomic_store_explicit(&target->write_sequence, write + 1,
			memory_order_release);
		lua_profile_request(L, weight);
	}
	errno = saved_errno;
}

static int
acquire_signal(lp_skynet_worker *worker) {
	int status = 0;
	pthread_mutex_lock(&host_lock);
	if (signal_users == 0) {
		host_signal = SIGRTMAX - 3;
		if (host_signal <= SIGRTMIN) {
			status = -1;
		}
		else {
			struct sigaction current;
			if (sigaction(host_signal, NULL, &current) != 0 ||
				current.sa_handler != SIG_DFL) {
				status = -1;
			}
			else {
				struct sigaction action;
				memset(&action, 0, sizeof(action));
				sigemptyset(&action.sa_mask);
				action.sa_sigaction = timer_signal_handler;
				action.sa_flags = SA_SIGINFO | SA_RESTART;
				if (sigaction(host_signal, &action,
					&saved_signal_action) != 0) {
					status = -1;
				}
			}
		}
	}
	worker->signal_slot = -1;
	if (status == 0) {
		for (size_t i = 0; i < LP_SKYNET_WORKER_CAPACITY; ++i) {
			if (atomic_load_explicit(&workers[i],
				memory_order_relaxed) == NULL) {
				atomic_store_explicit(&workers[i], worker,
					memory_order_release);
				worker->signal_slot = (int)i;
				break;
			}
		}
		if (worker->signal_slot < 0) {
			status = -1;
			if (signal_users == 0) {
				(void)sigaction(host_signal, &saved_signal_action, NULL);
			}
		}
		else {
			signal_users++;
		}
	}
	pthread_mutex_unlock(&host_lock);
	return status;
}

static void
release_signal(lp_skynet_worker *worker) {
	pthread_mutex_lock(&host_lock);
	if (worker->signal_slot >= 0) {
		atomic_store_explicit(&workers[worker->signal_slot], NULL,
			memory_order_release);
		worker->signal_slot = -1;
	}
	if (signal_users > 0 && --signal_users == 0) {
		(void)sigaction(host_signal, &saved_signal_action, NULL);
	}
	pthread_mutex_unlock(&host_lock);
}

static void
publish_slot(lp_skynet_worker *worker, lp_skynet_target *target,
	uint64_t token, lua_State *state, int vm_state,
	lp_skynet_lua_cfunction cfunction) {
	if (worker == NULL) {
		return;
	}
	atomic_fetch_add_explicit(&worker->slot_version, 1,
		memory_order_acq_rel);
	atomic_store_explicit(&worker->slot_target, target,
		memory_order_relaxed);
	atomic_store_explicit(&worker->slot_token, token,
		memory_order_relaxed);
	atomic_store_explicit(&worker->slot_state, state,
		memory_order_relaxed);
	atomic_store_explicit(&worker->slot_cfunction, cfunction,
		memory_order_relaxed);
	atomic_store_explicit(&worker->slot_vm_state, vm_state,
		memory_order_relaxed);
	atomic_fetch_add_explicit(&worker->slot_version, 1,
		memory_order_release);
}

static bool
block_worker_signal(sigset_t *set, sigset_t *previous) {
	if (current_worker == NULL) {
		return false;
	}
	sigemptyset(set);
	sigaddset(set, host_signal);
	return pthread_sigmask(SIG_BLOCK, set, previous) == 0;
}

static void
discard_pending(lp_skynet_worker *worker, const sigset_t *set,
	lp_skynet_target *target) {
	for (;;) {
		struct timespec no_wait = { 0, 0 };
		siginfo_t info;
		int received = sigtimedwait(set, &info, &no_wait);
		if (received != host_signal) {
			return;
		}
		if (info.si_code == SI_TIMER &&
			info.si_value.sival_ptr == worker && target != NULL) {
			add_quality(&target->stale, event_weight(&info));
		}
	}
}

static void
sync_worker_timer(lp_skynet_worker *worker) {
	if (worker == NULL) {
		return;
	}
	unsigned int count = atomic_load_explicit(&active_targets,
		memory_order_acquire);
	unsigned int hz = atomic_load_explicit(&active_sample_hz,
		memory_order_acquire);
	if (count == 0 || hz == 0) {
		if (worker->armed_hz != 0) {
			struct itimerspec disabled = { 0 };
			if (timer_settime(worker->timer_id, 0, &disabled, NULL) != 0) {
				atomic_store_explicit(&host_failed, true,
					memory_order_release);
			}
			worker->armed_hz = 0;
		}
		return;
	}
	if (worker->armed_hz == hz) {
		return;
	}
	uint64_t interval_ns = UINT64_C(1000000000) / hz;
	struct itimerspec interval = {
		.it_interval = {
			.tv_sec = (time_t)(interval_ns / UINT64_C(1000000000)),
			.tv_nsec = (long)(interval_ns % UINT64_C(1000000000)),
		},
	};
	interval.it_value = interval.it_interval;
	if (timer_settime(worker->timer_id, 0, &interval, NULL) != 0) {
		atomic_store_explicit(&host_failed, true, memory_order_release);
		return;
	}
	worker->armed_hz = hz;
}

static lp_skynet_target *
find_active_handle(uint32_t handle) {
	for (size_t i = 0; i < LP_SKYNET_TARGET_CAPACITY; ++i) {
		lp_skynet_target *target = &targets[i];
		if (atomic_load_explicit(&target->state,
			memory_order_acquire) == LP_SKYNET_TARGET_ACTIVE &&
			target->handle == handle) {
			return target;
		}
	}
	return NULL;
}

static lp_skynet_target *
find_token(uint64_t token, bool allow_stopping) {
	for (size_t i = 0; i < LP_SKYNET_TARGET_CAPACITY; ++i) {
		lp_skynet_target *target = &targets[i];
		int state = atomic_load_explicit(&target->state,
			memory_order_acquire);
		if ((state == LP_SKYNET_TARGET_ACTIVE ||
			(allow_stopping && state == LP_SKYNET_TARGET_STOPPING)) &&
			target->token == token) {
			return target;
		}
	}
	return NULL;
}

void
lp_skynet_host_worker_start(unsigned int worker_id) {
	if (current_worker != NULL) {
		return;
	}
	lp_skynet_worker *worker = calloc(1, sizeof(*worker));
	if (worker == NULL) {
		atomic_store_explicit(&host_failed, true, memory_order_release);
		return;
	}
	worker->worker_id = worker_id;
	worker->signal_slot = -1;
	atomic_init(&worker->slot_vm_state, 0);
	if (acquire_signal(worker) != 0) {
		free(worker);
		atomic_store_explicit(&host_failed, true, memory_order_release);
		return;
	}

	struct sigevent event;
	memset(&event, 0, sizeof(event));
	event.sigev_notify = SIGEV_THREAD_ID;
	event.sigev_signo = host_signal;
	event.sigev_value.sival_ptr = worker;
	event._sigev_un._tid = (pid_t)syscall(SYS_gettid);
	if (timer_create(CLOCK_THREAD_CPUTIME_ID, &event, &worker->timer_id) != 0) {
		release_signal(worker);
		free(worker);
		atomic_store_explicit(&host_failed, true, memory_order_release);
		return;
	}
	atomic_store_explicit(&worker->active, true, memory_order_release);
	current_worker = worker;
	sync_worker_timer(worker);
}

void
lp_skynet_host_worker_stop(void) {
	lp_skynet_worker *worker = current_worker;
	if (worker == NULL) {
		return;
	}
	sigset_t set;
	sigset_t previous;
	sigemptyset(&set);
	sigaddset(&set, host_signal);
	(void)pthread_sigmask(SIG_BLOCK, &set, &previous);
	publish_slot(worker, NULL, 0, NULL, 0, NULL);
	atomic_store_explicit(&worker->active, false, memory_order_release);
	struct itimerspec disabled = { 0 };
	(void)timer_settime(worker->timer_id, 0, &disabled, NULL);
	(void)timer_delete(worker->timer_id);
	for (;;) {
		struct timespec no_wait = { 0, 0 };
		siginfo_t info;
		int received = sigtimedwait(&set, &info, &no_wait);
		if (received != host_signal) {
			break;
		}
	}
	current_worker = NULL;
	current_handle = 0;
	release_signal(worker);
	(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
	free(worker);
}

void
lp_skynet_host_dispatch_enter(uint32_t handle) {
	current_handle = handle;
	if (current_worker == NULL) {
		return;
	}
	sigset_t set;
	sigset_t previous;
	bool blocked = block_worker_signal(&set, &previous);
	lp_skynet_target *previous_target = atomic_load_explicit(
		&current_worker->slot_target, memory_order_acquire);
	publish_slot(current_worker, NULL, 0, NULL, 0, NULL);
	if (blocked) {
		discard_pending(current_worker, &set, previous_target);
	}
	sync_worker_timer(current_worker);
	lp_skynet_target *target = find_active_handle(handle);
	if (target == NULL) {
		if (blocked) {
			(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
		}
		return;
	}
	publish_slot(current_worker, target, target->token, target->main_state,
		0, NULL);
	if (blocked) {
		(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
	}
}

void
lp_skynet_host_dispatch_leave(void) {
	if (current_worker != NULL) {
		sigset_t set;
		sigset_t previous;
		bool blocked = block_worker_signal(&set, &previous);
		lp_skynet_target *target = atomic_load_explicit(
			&current_worker->slot_target, memory_order_acquire);
		publish_slot(current_worker, NULL, 0, NULL, 0, NULL);
		if (blocked) {
			discard_pending(current_worker, &set, target);
		}
		sync_worker_timer(current_worker);
		if (blocked) {
			(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
		}
	}
	current_handle = 0;
}

static uint32_t
api_current_handle(void) {
	return current_worker == NULL ? 0 : current_handle;
}

static int
api_target_start(uint32_t handle, lua_State *main_state,
	uint64_t generation, uint32_t sample_hz, uint64_t *token) {
	if (handle == 0 || main_state == NULL || generation == 0 ||
		sample_hz == 0 || token == NULL || current_worker == NULL ||
		current_handle != handle ||
		atomic_load_explicit(&host_failed, memory_order_acquire)) {
		return -1;
	}

	lp_skynet_target *selected = NULL;
	pthread_mutex_lock(&host_lock);
	unsigned int count = atomic_load_explicit(&active_targets,
		memory_order_relaxed);
	unsigned int hz = atomic_load_explicit(&active_sample_hz,
		memory_order_relaxed);
	if (count != 0 && hz != sample_hz) {
		pthread_mutex_unlock(&host_lock);
		return -1;
	}
	for (size_t i = 0; i < LP_SKYNET_TARGET_CAPACITY; ++i) {
		int state = atomic_load_explicit(&targets[i].state,
			memory_order_relaxed);
		if (state != LP_SKYNET_TARGET_FREE &&
			targets[i].handle == handle) {
			pthread_mutex_unlock(&host_lock);
			return -1;
		}
		if (selected == NULL && state == LP_SKYNET_TARGET_FREE) {
			selected = &targets[i];
		}
	}
	if (selected == NULL) {
		pthread_mutex_unlock(&host_lock);
		return -1;
	}
	selected->events = calloc(LP_SKYNET_RING_CAPACITY,
		sizeof(selected->events[0]));
	if (selected->events == NULL) {
		pthread_mutex_unlock(&host_lock);
		return -1;
	}
	uint64_t selected_token = ++next_token;
	if (selected_token == 0) {
		selected_token = ++next_token;
	}
	selected->handle = handle;
	selected->token = selected_token;
	selected->generation = generation;
	selected->sample_hz = sample_hz;
	selected->main_state = main_state;
	atomic_store_explicit(&selected->write_sequence, 0,
		memory_order_relaxed);
	atomic_store_explicit(&selected->read_sequence, 0,
		memory_order_relaxed);
	atomic_store_explicit(&selected->dropped, 0, memory_order_relaxed);
	atomic_store_explicit(&selected->unstable, 0, memory_order_relaxed);
	atomic_store_explicit(&selected->profiler_overhead, 0,
		memory_order_relaxed);
	atomic_store_explicit(&selected->stale, 0, memory_order_relaxed);
	atomic_store_explicit(&selected->worker_mask, 0,
		memory_order_relaxed);
	atomic_store_explicit(&selected->draining_events, false,
		memory_order_relaxed);
	atomic_store_explicit(&selected->state, LP_SKYNET_TARGET_ACTIVE,
		memory_order_release);
	atomic_store_explicit(&active_sample_hz, sample_hz,
		memory_order_release);
	atomic_store_explicit(&active_targets, count + 1,
		memory_order_release);
	pthread_mutex_unlock(&host_lock);

	sigset_t set;
	sigset_t previous;
	bool blocked = block_worker_signal(&set, &previous);
	publish_slot(current_worker, NULL, 0, NULL, 0, NULL);
	if (blocked) {
		discard_pending(current_worker, &set, selected);
	}
	sync_worker_timer(current_worker);
	publish_slot(current_worker, selected, selected_token, main_state, 0,
		NULL);
	if (blocked) {
		(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
	}
	*token = selected_token;
	return 0;
}

static int
api_target_quiesce(uint64_t token) {
	lp_skynet_target *target = NULL;
	pthread_mutex_lock(&host_lock);
	for (size_t i = 0; i < LP_SKYNET_TARGET_CAPACITY; ++i) {
		if (atomic_load_explicit(&targets[i].state,
			memory_order_relaxed) == LP_SKYNET_TARGET_ACTIVE &&
			targets[i].token == token) {
			target = &targets[i];
			break;
		}
	}
	if (target == NULL) {
		pthread_mutex_unlock(&host_lock);
		return -1;
	}
	atomic_store_explicit(&target->state, LP_SKYNET_TARGET_STOPPING,
		memory_order_release);
	unsigned int count = atomic_load_explicit(&active_targets,
		memory_order_relaxed);
	if (count > 0) {
		count--;
	}
	atomic_store_explicit(&active_targets, count, memory_order_release);
	if (count == 0) {
		atomic_store_explicit(&active_sample_hz, 0,
			memory_order_release);
	}
	pthread_mutex_unlock(&host_lock);

	sigset_t set;
	sigset_t previous;
	bool blocked = block_worker_signal(&set, &previous);
	if (current_worker != NULL) {
		lp_skynet_target *published = atomic_load_explicit(
			&current_worker->slot_target, memory_order_acquire);
		if (published == target) {
			publish_slot(current_worker, NULL, 0, NULL, 0, NULL);
		}
		if (blocked) {
			discard_pending(current_worker, &set, target);
		}
	}
	sync_worker_timer(current_worker);
	if (blocked) {
		(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
	}
	return 0;
}

static void
api_target_release(uint64_t token) {
	pthread_mutex_lock(&host_lock);
	lp_skynet_target *target = NULL;
	for (size_t i = 0; i < LP_SKYNET_TARGET_CAPACITY; ++i) {
		if (atomic_load_explicit(&targets[i].state,
			memory_order_relaxed) == LP_SKYNET_TARGET_STOPPING &&
			targets[i].token == token) {
			target = &targets[i];
			break;
		}
	}
	if (target != NULL) {
		free(target->events);
		target->events = NULL;
		target->handle = 0;
		target->token = 0;
		target->generation = 0;
		target->sample_hz = 0;
		target->main_state = NULL;
		atomic_store_explicit(&target->state, LP_SKYNET_TARGET_FREE,
			memory_order_release);
	}
	pthread_mutex_unlock(&host_lock);
}

static void
api_publish_state(uint64_t token, lua_State *state, int vm_state,
	lp_skynet_lua_cfunction cfunction) {
	lp_skynet_target *target = find_token(token, false);
	if (target == NULL || current_worker == NULL ||
		current_handle != target->handle) {
		return;
	}
	publish_slot(current_worker, target, token, state, vm_state, cfunction);
}

static void
api_begin_event_drain(uint64_t token) {
	lp_skynet_target *target = find_token(token, true);
	if (target != NULL) {
		atomic_store_explicit(&target->draining_events, true,
			memory_order_release);
	}
}

static void
api_end_event_drain(uint64_t token) {
	lp_skynet_target *target = find_token(token, true);
	if (target != NULL) {
		atomic_store_explicit(&target->draining_events, false,
			memory_order_release);
	}
}

static bool
api_next_event(uint64_t token, lp_skynet_tick_event *event) {
	lp_skynet_target *target = find_token(token, true);
	if (target == NULL || event == NULL) {
		return false;
	}
	uint64_t read = atomic_load_explicit(&target->read_sequence,
		memory_order_relaxed);
	uint64_t write = atomic_load_explicit(&target->write_sequence,
		memory_order_acquire);
	if (read == write) {
		return false;
	}
	*event = target->events[read & (LP_SKYNET_RING_CAPACITY - 1u)];
	atomic_store_explicit(&target->read_sequence, read + 1,
		memory_order_release);
	return true;
}

static void
api_take_quality(uint64_t token, lp_skynet_quality *quality) {
	if (quality == NULL) {
		return;
	}
	memset(quality, 0, sizeof(*quality));
	lp_skynet_target *target = find_token(token, true);
	if (target == NULL) {
		return;
	}
	quality->dropped = atomic_exchange_explicit(&target->dropped, 0,
		memory_order_relaxed);
	quality->unstable = atomic_exchange_explicit(&target->unstable, 0,
		memory_order_relaxed);
	quality->profiler_overhead = atomic_exchange_explicit(
		&target->profiler_overhead, 0, memory_order_relaxed);
	quality->stale = atomic_exchange_explicit(&target->stale, 0,
		memory_order_relaxed);
	quality->worker_mask = atomic_load_explicit(&target->worker_mask,
		memory_order_relaxed);
}

const lp_skynet_host_api *
lp_skynet_host_get_api(uint32_t abi_version) {
	static const lp_skynet_host_api api = {
		.abi_version = LP_SKYNET_HOST_ABI_VERSION,
		.current_handle = api_current_handle,
		.target_start = api_target_start,
		.target_quiesce = api_target_quiesce,
		.target_release = api_target_release,
		.publish_state = api_publish_state,
		.begin_event_drain = api_begin_event_drain,
		.end_event_drain = api_end_event_drain,
		.next_event = api_next_event,
		.take_quality = api_take_quality,
	};
	return abi_version == LP_SKYNET_HOST_ABI_VERSION ? &api : NULL;
}
