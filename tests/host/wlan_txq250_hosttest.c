/* Run wlan_txq_hosttest's rows at the shipped 250 Hz, where a transmit
 * session's ADDBA response, retry and DELBA waits count 4 ms ticks as
 * mac80211's do at HZ=250.
 * host-deps: rach2/sys/network/wireless/data.c
 * host-deps: rach2/sys/network/wireless/tx.c
 * host-deps: rach2/sys/network/wireless/rx.c
 * host-deps: rach2/sys/network/wireless/key.c
 * host-deps: rach2/sys/network/wireless/agg_rx.c
 * host-deps: rach2/sys/network/wireless/rate.c
 * host-deps: rach2/sys/network/wireless/elems.c
 * host-deps: rach2/sys/network/wireless/wlan.c
 * host-deps: rach2/sys/network/wireless/crypto/aes.c
 * host-deps: rach2/sys/network/wireless/crypto/ccmp.c
 * host-deps: rach2/sys/network/wireless/crypto/bip.c
 * host-deps: rach2/sys/network/wireless/crypto/tkip.c
 * host-deps: rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c
 * host-deps: tests/host/compat/spinlock_host.c
 */
#define main wlan_txq_rows
#include "wlan_txq_hosttest.c"
#undef main

/** Run every row of wlan_txq_hosttest at 250 Hz. */
int main(void)
{
    if (TICK_HZ != 250 || wlan_txq_rows() != 0) {
        printf("wlan_txq250_hosttest: failed\n");
        return 1;
    }
    printf("wlan_txq250_hosttest: ok\n");
    return 0;
}
