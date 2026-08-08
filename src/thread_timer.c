#define _GNU_SOURCE

#include "thread_timer.h"

#if defined(LUAPROF_TESTING)
#include "thread_timer_test.h"
#endif

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

#define LP_TICK_RING_CAPACITY 4096u
#define LP_ACTIVE_TIMER_CAPACITY 64u

_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2,
	"thread timer requires lock-free 64-bit atomics");
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2,
	"thread timer requires lock-free pointer atomics");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
	"thread timer requires lock-free integer atomics");

struct lp_thread_timer {
	timer_t timer_id;
	lp_tick_event *events;
	_Atomic uint64_t write_sequence;
	_Atomic uint64_t read_sequence;
	_Atomic uint64_t dropped;
	_Atomic uint64_t unstable;
	_Atomic uint64_t profiler_overhead;
	_Atomic uint64_t overrun_events;
	_Atomic uint64_t overrun_ticks;
	_Atomic uint64_t slot_version;
	_Atomic(lua_State *) slot_state;
	_Atomic(lp_lua_cfunction) slot_cfunction;
	_Atomic int slot_vm_state;
	_Atomic bool active;
	_Atomic bool draining_events;
	bool timer_created;
	bool signal_acquired;
	int signal_slot;
};

static pthread_mutex_t signal_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int signal_users;
static int timer_signal;
static struct sigaction saved_signal_action;
static _Thread_local lp_thread_timer *active_thread_timer;
static _Atomic(lp_thread_timer *) active_timers[LP_ACTIVE_TIMER_CAPACITY];

static unsigned int
event_overrun(const siginfo_t *info) {
	int overrun = info == NULL ? 0 : info->si_overrun;
	return overrun <= 0 ? 0u : (unsigned int)overrun;
}

static void
add_quality(_Atomic uint64_t *counter, uint64_t value) {
	uint64_t old = atomic_load_explicit(counter, memory_order_relaxed);
	for (;;) {
		uint64_t next = UINT64_MAX - old < value ? UINT64_MAX : old + value;
		if (atomic_compare_exchange_weak_explicit(counter, &old, next,
			memory_order_relaxed, memory_order_relaxed)) {
			return;
		}
	}
}

static void
record_overrun(lp_thread_timer *timer, const siginfo_t *info) {
	unsigned int overrun = event_overrun(info);
	if (overrun != 0) {
		add_quality(&timer->overrun_events, 1);
		add_quality(&timer->overrun_ticks, overrun);
	}
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
	lp_thread_timer *timer = info->si_value.sival_ptr;
	bool registered = false;
	for (size_t i = 0; i < LP_ACTIVE_TIMER_CAPACITY; ++i) {
		if (atomic_load_explicit(&active_timers[i],
			memory_order_acquire) == timer) {
			registered = true;
			break;
		}
	}
	if (!registered || timer == NULL ||
		!atomic_load_explicit(&timer->active, memory_order_acquire)) {
		errno = saved_errno;
		return;
	}

	record_overrun(timer, info);
	if (atomic_load_explicit(&timer->draining_events, memory_order_acquire)) {
		add_quality(&timer->profiler_overhead, 1);
		errno = saved_errno;
		return;
	}
	uint64_t version = atomic_load_explicit(&timer->slot_version,
		memory_order_acquire);
	if ((version & 1u) != 0) {
		add_quality(&timer->unstable, 1);
		errno = saved_errno;
		return;
	}
	lua_State *L = atomic_load_explicit(&timer->slot_state,
		memory_order_relaxed);
	lp_lua_cfunction cfunction = atomic_load_explicit(
		&timer->slot_cfunction, memory_order_relaxed);
	int vm_state = atomic_load_explicit(&timer->slot_vm_state,
		memory_order_relaxed);
	if (version != atomic_load_explicit(&timer->slot_version,
		memory_order_acquire) || L == NULL || vm_state < LP_VM_HOST ||
		vm_state > LP_VM_GC ||
		(vm_state == LP_VM_C && cfunction == NULL)) {
		add_quality(&timer->unstable, 1);
		errno = saved_errno;
		return;
	}

	uint64_t write = atomic_load_explicit(&timer->write_sequence,
		memory_order_relaxed);
	uint64_t read = atomic_load_explicit(&timer->read_sequence,
		memory_order_acquire);
	if (write - read >= LP_TICK_RING_CAPACITY) {
		add_quality(&timer->dropped, 1);
	}
	else {
		lp_tick_event *event =
			&timer->events[write & (LP_TICK_RING_CAPACITY - 1u)];
		event->state = L;
		event->vm_state = (lp_vm_state)vm_state;
		event->cfunction = cfunction;
		atomic_store_explicit(&timer->write_sequence, write + 1,
			memory_order_release);
	}
	lua_profile_request(L, 1);
	errno = saved_errno;
}

#if defined(LUAPROF_TESTING)
void
lp_thread_timer_test_inject_tick(lp_thread_timer *timer, int overrun) {
	siginfo_t info;
	memset(&info, 0, sizeof(info));
	info.si_code = SI_TIMER;
	info.si_overrun = overrun;
	info.si_value.sival_ptr = timer;
	timer_signal_handler(timer_signal, &info, NULL);
}
#endif

static lp_status
acquire_timer_signal(lp_thread_timer *timer, int *signal_number) {
	lp_status status = LP_OK;
	pthread_mutex_lock(&signal_lock);
	if (signal_users == 0) {
		timer_signal = SIGRTMAX - 2;
		if (timer_signal <= SIGRTMIN) {
			status = LP_ERR_HOST;
		}
		else {
			struct sigaction current;
			if (sigaction(timer_signal, NULL, &current) != 0 ||
				current.sa_handler != SIG_DFL) {
				status = LP_ERR_HOST;
			}
			else {
				struct sigaction action;
				memset(&action, 0, sizeof(action));
				sigemptyset(&action.sa_mask);
				action.sa_sigaction = timer_signal_handler;
				action.sa_flags = SA_SIGINFO | SA_RESTART;
				if (sigaction(timer_signal, &action,
					&saved_signal_action) != 0) {
					status = LP_ERR_HOST;
				}
			}
		}
	}
	if (status == LP_OK) {
		timer->signal_slot = -1;
		for (size_t i = 0; i < LP_ACTIVE_TIMER_CAPACITY; ++i) {
			if (atomic_load_explicit(&active_timers[i],
				memory_order_relaxed) == NULL) {
				atomic_store_explicit(&active_timers[i], timer,
					memory_order_release);
				timer->signal_slot = (int)i;
				break;
			}
		}
		if (timer->signal_slot < 0) {
			status = LP_ERR_HOST;
			if (signal_users == 0) {
				(void)sigaction(timer_signal, &saved_signal_action, NULL);
			}
		}
		else {
			signal_users++;
			*signal_number = timer_signal;
		}
	}
	pthread_mutex_unlock(&signal_lock);
	return status;
}

static void
release_timer_signal(lp_thread_timer *timer) {
	pthread_mutex_lock(&signal_lock);
	if (timer->signal_slot >= 0) {
		atomic_store_explicit(&active_timers[timer->signal_slot], NULL,
			memory_order_release);
		timer->signal_slot = -1;
	}
	if (--signal_users == 0) {
		(void)sigaction(timer_signal, &saved_signal_action, NULL);
	}
	pthread_mutex_unlock(&signal_lock);
}

lp_thread_timer *
lp_thread_timer_new(void) {
	lp_thread_timer *timer = calloc(1, sizeof(*timer));
	if (timer == NULL) {
		return NULL;
	}
	timer->events = calloc(LP_TICK_RING_CAPACITY,
		sizeof(timer->events[0]));
	if (timer->events == NULL) {
		free(timer);
		return NULL;
	}
	atomic_init(&timer->slot_vm_state, LP_VM_HOST);
	timer->signal_slot = -1;
	return timer;
}

void
lp_thread_timer_delete(lp_thread_timer *timer) {
	if (timer == NULL) {
		return;
	}
	lp_thread_timer_disarm(timer);
	free(timer->events);
	free(timer);
}

void
lp_thread_timer_publish_state(lp_thread_timer *timer, lua_State *state,
	lp_vm_state vm_state, lp_lua_cfunction cfunction) {
	if (timer == NULL) {
		return;
	}
	atomic_fetch_add_explicit(&timer->slot_version, 1,
		memory_order_acq_rel);
	atomic_store_explicit(&timer->slot_state, state, memory_order_relaxed);
	atomic_store_explicit(&timer->slot_cfunction, cfunction,
		memory_order_relaxed);
	atomic_store_explicit(&timer->slot_vm_state, vm_state,
		memory_order_relaxed);
	atomic_fetch_add_explicit(&timer->slot_version, 1,
		memory_order_release);
}

lp_status
lp_thread_timer_arm(lp_thread_timer *timer, uint32_t sample_hz) {
	if (timer == NULL || sample_hz == 0 || timer->timer_created) {
		return LP_ERR_ARGUMENT;
	}
	if (active_thread_timer != NULL) {
		return LP_ERR_HOST;
	}
	int signal_number;
	lp_status status = acquire_timer_signal(timer, &signal_number);
	if (status != LP_OK) {
		return status;
	}
	timer->signal_acquired = true;

	sigset_t set;
	sigset_t previous;
	sigemptyset(&set);
	sigaddset(&set, signal_number);
	if (pthread_sigmask(SIG_BLOCK, &set, &previous) != 0 ||
		sigismember(&previous, signal_number)) {
		release_timer_signal(timer);
		timer->signal_acquired = false;
		return LP_ERR_HOST;
	}

	struct sigevent event;
	memset(&event, 0, sizeof(event));
	event.sigev_notify = SIGEV_THREAD_ID;
	event.sigev_signo = signal_number;
	event.sigev_value.sival_ptr = timer;
	event._sigev_un._tid = (pid_t)syscall(SYS_gettid);
	if (timer_create(CLOCK_THREAD_CPUTIME_ID, &event, &timer->timer_id) != 0) {
		status = LP_ERR_HOST;
	}
	else {
		timer->timer_created = true;
		uint64_t interval_ns = UINT64_C(1000000000) / sample_hz;
		struct itimerspec interval = {
			.it_interval = {
				.tv_sec = (time_t)(interval_ns / UINT64_C(1000000000)),
				.tv_nsec = (long)(interval_ns % UINT64_C(1000000000)),
			},
		};
		interval.it_value = interval.it_interval;
		atomic_store_explicit(&timer->active, true, memory_order_release);
		if (timer_settime(timer->timer_id, 0, &interval, NULL) != 0) {
			atomic_store_explicit(&timer->active, false,
				memory_order_release);
			(void)timer_delete(timer->timer_id);
			timer->timer_created = false;
			status = LP_ERR_HOST;
		}
	}
	if (status != LP_OK) {
		release_timer_signal(timer);
		timer->signal_acquired = false;
	}
	else {
		active_thread_timer = timer;
	}
	(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
	return status;
}

void
lp_thread_timer_disarm(lp_thread_timer *timer) {
	if (timer == NULL || !timer->signal_acquired) {
		return;
	}
	int signal_number = timer_signal;
	sigset_t set;
	sigset_t previous;
	sigemptyset(&set);
	sigaddset(&set, signal_number);
	(void)pthread_sigmask(SIG_BLOCK, &set, &previous);
	atomic_store_explicit(&timer->active, false, memory_order_release);
	if (timer->timer_created) {
		struct itimerspec disabled = { 0 };
		(void)timer_settime(timer->timer_id, 0, &disabled, NULL);
		(void)timer_delete(timer->timer_id);
		timer->timer_created = false;
	}
	for (;;) {
		struct timespec no_wait = { 0, 0 };
		siginfo_t info;
		int received = sigtimedwait(&set, &info, &no_wait);
		if (received != signal_number) {
			break;
		}
		if (info.si_code == SI_TIMER && info.si_value.sival_ptr == timer) {
			record_overrun(timer, &info);
			add_quality(&timer->dropped, 1);
		}
	}
	if (active_thread_timer == timer) {
		active_thread_timer = NULL;
	}
	release_timer_signal(timer);
	timer->signal_acquired = false;
	(void)pthread_sigmask(SIG_SETMASK, &previous, NULL);
}

bool
lp_thread_timer_next(lp_thread_timer *timer, lp_tick_event *event) {
	if (timer == NULL || event == NULL) {
		return false;
	}
	uint64_t read = atomic_load_explicit(&timer->read_sequence,
		memory_order_relaxed);
	uint64_t write = atomic_load_explicit(&timer->write_sequence,
		memory_order_acquire);
	if (read == write) {
		return false;
	}
	*event = timer->events[read & (LP_TICK_RING_CAPACITY - 1u)];
	atomic_store_explicit(&timer->read_sequence, read + 1,
		memory_order_release);
	return true;
}

void
lp_thread_timer_begin_event_drain(lp_thread_timer *timer) {
	if (timer != NULL) {
		atomic_store_explicit(&timer->draining_events, true,
			memory_order_release);
	}
}

void
lp_thread_timer_end_event_drain(lp_thread_timer *timer) {
	if (timer != NULL) {
		atomic_store_explicit(&timer->draining_events, false,
			memory_order_release);
	}
}

void
lp_thread_timer_take_quality(lp_thread_timer *timer, uint64_t *dropped,
	uint64_t *unstable, uint64_t *profiler_overhead,
	uint64_t *overrun_events, uint64_t *overrun_ticks) {
	if (timer == NULL) {
		return;
	}
	if (dropped != NULL) {
		*dropped = atomic_exchange_explicit(&timer->dropped, 0,
			memory_order_relaxed);
	}
	if (unstable != NULL) {
		*unstable = atomic_exchange_explicit(&timer->unstable, 0,
			memory_order_relaxed);
	}
	if (profiler_overhead != NULL) {
		*profiler_overhead = atomic_exchange_explicit(
			&timer->profiler_overhead, 0, memory_order_relaxed);
	}
	if (overrun_events != NULL) {
		*overrun_events = atomic_exchange_explicit(&timer->overrun_events, 0,
			memory_order_relaxed);
	}
	if (overrun_ticks != NULL) {
		*overrun_ticks = atomic_exchange_explicit(&timer->overrun_ticks, 0,
			memory_order_relaxed);
	}
}
