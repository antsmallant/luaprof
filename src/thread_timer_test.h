#ifndef LUAPROF_THREAD_TIMER_TEST_H
#define LUAPROF_THREAD_TIMER_TEST_H

#include "thread_timer.h"

/* Available only when thread_timer.c is compiled with LUAPROF_TESTING. */
void lp_thread_timer_test_inject_tick(lp_thread_timer *timer, int overrun);

#endif
