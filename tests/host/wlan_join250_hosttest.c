/* Run wlan_join_hosttest's rows at the shipped 250 Hz, where the beacon loss
 * and a channel switch's countdown round microseconds up to 4 ms ticks as
 * usecs_to_jiffies does at HZ=250.
 * host-deps: rach2/sys/network/wireless/wiphy.c
 * host-deps: rach2/sys/network/wireless/wlan.c
 * host-deps: rach2/sys/network/wireless/reg.c
 * host-deps: rach2/sys/network/wireless/sta.c
 * host-deps: rach2/sys/network/wireless/mlme.c
 * host-deps: rach2/sys/network/wireless/work.c
 * host-deps: rach2/sys/network/wireless/scan.c
 * host-deps: rach2/sys/network/wireless/bss.c
 * host-deps: rach2/sys/network/wireless/elems.c
 * host-deps: rach2/sys/network/wireless/data.c
 * host-deps: rach2/sys/network/wireless/tx.c
 * host-deps: rach2/sys/network/wireless/rx.c
 * host-deps: rach2/sys/network/wireless/key.c
 * host-deps: rach2/sys/network/wireless/agg_rx.c
 * host-deps: rach2/sys/network/wireless/rate.c
 * host-deps: rach2/sys/network/wireless/wsim.c
 * host-deps: rach2/sys/network/wireless/crypto/aes.c
 * host-deps: rach2/sys/network/wireless/crypto/ccmp.c
 * host-deps: rach2/sys/network/wireless/crypto/bip.c
 * host-deps: rach2/sys/network/wireless/crypto/tkip.c
 * host-deps: rach2/sys/network/socket/genetlink.c
 * host-deps: rach2/sys/rfkill.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/lib/kstd.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 * host-deps: rach2/sys/tick.c
 * host-deps: rach2/sys/lib/crc32.c
 */
#define main wlan_join_rows
#include "wlan_join_hosttest.c"
#undef main

/** Run every row of wlan_join_hosttest at 250 Hz. */
int main(void)
{
    if (TICK_HZ != 250 || wlan_join_rows() != 0) {
        printf("wlan_join250_hosttest: failed\n");
        return 1;
    }
    printf("wlan_join250_hosttest: ok\n");
    return 0;
}
