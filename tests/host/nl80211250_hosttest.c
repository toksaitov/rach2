/* Run nl80211's rows at the shipped 250 Hz, where the station's inactive and
 * connected times, a BSS's age and the wait after a leave count 4 ms ticks as
 * cfg80211's jiffies_to_msecs and msecs_to_jiffies do at HZ=250.
 * host-deps: rach2/sys/network/socket/genetlink.c
 * host-deps: rach2/sys/network/wireless/work.c rach2/sys/network/wireless/bss.c
 * host-deps: rach2/sys/network/wireless/elems.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/park_host.c
 * host-deps: rach2/sys/tick.c
 */
#define main nl80211_rows
#include "nl80211_hosttest.c"
#undef main

/** Run every row of nl80211_hosttest at 250 Hz. */
int main(void)
{
    if (TICK_HZ != 250 || nl80211_rows() != 0) {
        printf("nl80211250_hosttest: failed\n");
        return 1;
    }
    printf("nl80211250_hosttest: ok\n");
    return 0;
}
