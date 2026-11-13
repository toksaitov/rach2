/* Run every row of syscall_hosttest.c at the production tick of 250 Hz,
 * whose answers are in TICK_HZ and TICK_NS beside its 1000 Hz build.
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/network/socket/address.c rach2/sys/network/ip6/addr6.c
 * host-deps: rach2/sys/tick.c tests/host/compat/kheap_host.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 */
#include "syscall_hosttest.c"

#if TICK_HZ != 250
#error "this test checks the production tick of 250 Hz"
#endif
