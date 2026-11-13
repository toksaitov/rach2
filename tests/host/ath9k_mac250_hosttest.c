/* Run ath9k_mac_hosttest's rows at the shipped 250 Hz, where the hang check
 * comes every 250 ticks and a receive window of 1000 ticks judges frames a
 * second, as ath9k's do at HZ=250.
 * host-deps: rach2/sys/network/driver/ath9k/io.c
 * host-deps: rach2/sys/network/driver/ath9k/chips.c
 * host-deps: rach2/sys/network/driver/ath9k/mac.c
 * host-deps: rach2/sys/network/driver/ath9k/recv.c
 * host-deps: rach2/sys/network/driver/ath9k/xmit.c
 * host-deps: rach2/sys/network/driver/ath9k/key.c
 * host-deps: rach2/sys/network/driver/ath9k/intr.c
 * host-deps: rach2/sys/network/driver/ath9k/recover.c
 * host-deps: rach2/sys/network/driver/ath9k/ampdu.c
 * host-deps: tests/host/ath9k_model.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 */
#define main ath9k_mac_rows
#include "ath9k_mac_hosttest.c"
#undef main

/** Run every row of ath9k_mac_hosttest at 250 Hz. */
int main(void)
{
    if (TICK_HZ != 250 || ath9k_mac_rows() != 0) {
        printf("ath9k_mac250_hosttest: failed\n");
        return 1;
    }
    printf("ath9k_mac250_hosttest: ok\n");
    return 0;
}
