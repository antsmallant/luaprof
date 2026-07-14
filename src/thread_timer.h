#ifndef LUAPROF_THREAD_TIMER_H
#define LUAPROF_THREAD_TIMER_H

#include "luaprof/runtime.h"

typedef struct lp_thread_timer lp_thread_timer;

typedef struct lp_tick_event {
	lua_State *state;
	lp_vm_state vm_state;
	lp_lua_cfunction cfunction;
	unsigned int weight;
} lp_tick_event;

lp_thread_timer *lp_thread_timer_new(void);
void lp_thread_timer_delete(lp_thread_timer *timer);
void lp_thread_timer_publish_state(lp_thread_timer *timer, lua_State *state,
	lp_vm_state vm_state, lp_lua_cfunction cfunction);
lp_status lp_thread_timer_arm(lp_thread_timer *timer, uint32_t sample_hz);
void lp_thread_timer_disarm(lp_thread_timer *timer);
void lp_thread_timer_begin_event_drain(lp_thread_timer *timer);
void lp_thread_timer_end_event_drain(lp_thread_timer *timer);
bool lp_thread_timer_next(lp_thread_timer *timer, lp_tick_event *event);
void lp_thread_timer_take_quality(lp_thread_timer *timer, uint64_t *dropped,
	uint64_t *unstable, uint64_t *profiler_overhead);

#endif
