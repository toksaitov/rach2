/* Run every row of timerfd_hosttest.c at the production tick of 250 Hz,
 * whose answers are in TICK_HZ and NS_PER_TICK beside its 1000 Hz build.
 * host-deps: tests/host/compat/spinlock_host.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 * host-deps: rach2/sys/tick.c
 */
#include "timerfd_hosttest.c"

#if TICK_HZ != 250
#error "this test checks the production tick of 250 Hz"
#endif
