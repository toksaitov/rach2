/* Run ath9k_ampdu_hosttest's rows at the shipped 250 Hz, where a refused or
 * unanswered ADDBA waits 3750 and 250 ticks, as HT_AGG_RETRIES_PERIOD and
 * ADDBA_RESP_INTERVAL are at HZ=250.
 * host-deps: rach2/sys/network/driver/ath9k/io.c
 * host-deps: rach2/sys/network/driver/ath9k/chips.c
 * host-deps: rach2/sys/network/driver/ath9k/mac.c
 * host-deps: rach2/sys/network/driver/ath9k/recv.c
 * host-deps: rach2/sys/network/driver/ath9k/xmit.c
 * host-deps: rach2/sys/network/driver/ath9k/ampdu.c
 * host-deps: tests/host/ath9k_model.c
 * host-deps: rach2/sys/network/wireless/tx.c rach2/sys/network/wireless/rx.c
 * host-deps: rach2/sys/network/wireless/key.c
 * host-deps: rach2/sys/network/wireless/agg_rx.c
 * host-deps: rach2/sys/network/wireless/rate.c
 * host-deps: rach2/sys/network/wireless/rc_minstrel.c
 * host-deps: rach2/sys/network/wireless/elems.c
 * host-deps: rach2/sys/network/wireless/crypto/aes.c
 * host-deps: rach2/sys/network/wireless/crypto/ccmp.c
 * host-deps: rach2/sys/network/wireless/crypto/bip.c
 * host-deps: rach2/sys/network/wireless/crypto/tkip.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/spinlock_host.c
 */
#define main ath9k_ampdu_rows
#include "ath9k_ampdu_hosttest.c"
#undef main

/** Run every row of ath9k_ampdu_hosttest at 250 Hz. */
int main(void)
{
    if (TICK_HZ != 250 || ath9k_ampdu_rows() != 0) {
        printf("ath9k_ampdu250_hosttest: failed\n");
        return 1;
    }
    printf("ath9k_ampdu250_hosttest: ok\n");
    return 0;
}
