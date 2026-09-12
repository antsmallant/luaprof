#ifndef LUAPROF_SKYNET_HOST_TEST_H
#define LUAPROF_SKYNET_HOST_TEST_H

/* Available only when skynet_host.c is compiled with LUAPROF_TESTING. */
void lp_skynet_host_test_inject_transition_tick(int overrun);
void lp_skynet_host_test_inject_tick_now(int overrun);
void lp_skynet_host_test_reset_failure(void);

#endif
