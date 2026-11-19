/* Run wlan_data_hosttest's rows at the shipped 250 Hz, where the data path's
 * BlockAck, fragment and rekey waits and the MLME's renewal waits count 4 ms
 * ticks as mac80211's do at HZ=250.
 * host-deps: rach2/sys/network/wireless/tx.c
 * host-deps: rach2/sys/network/wireless/rx.c
 * host-deps: rach2/sys/network/wireless/key.c
 * host-deps: rach2/sys/network/wireless/agg_rx.c
 * host-deps: rach2/sys/network/wireless/rate.c
 * host-deps: rach2/sys/network/wireless/elems.c
 * host-deps: rach2/sys/network/wireless/mlme.c
 * host-deps: rach2/sys/tick.c
 * host-deps: rach2/sys/network/wireless/crypto/aes.c
 * host-deps: rach2/sys/network/wireless/crypto/ccmp.c
 * host-deps: rach2/sys/network/wireless/crypto/bip.c
 * host-deps: rach2/sys/network/wireless/crypto/tkip.c
 * host-deps: rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-deps: rach2/sys/lib/crc32.c
 */
#define main wlan_data_rows
#include "wlan_data_hosttest.c"
#undef main

/** Run every row of wlan_data_hosttest at 250 Hz. */
int main(void)
{
    if (TICK_HZ != 250 || wlan_data_rows() != 0) {
        printf("wlan_data250_hosttest: failed\n");
        return 1;
    }
    printf("wlan_data250_hosttest: ok\n");
    return 0;
}
