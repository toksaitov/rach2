/* Run ath9k_phy_hosttest's rows at the shipped 250 Hz, where the calibrations
 * and ANI come in 4 ms ticks, as ath9k's msecs_to_jiffies and jiffies_to_msecs
 * give them at HZ=250.
 * host-deps: tests/host/ath9k_model.c
 * host-deps: rach2/sys/network/driver/ath9k/io.c
 * host-deps: rach2/sys/network/driver/ath9k/chips.c
 * host-deps: rach2/sys/network/driver/ath9k/phy.c
 * host-deps: rach2/sys/network/driver/ath9k/calib.c
 * host-deps: rach2/sys/network/driver/ath9k/ani.c
 * host-deps: rach2/sys/network/wireless/work.c
 * host-deps: rach2/sys/cmdline.c rach2/sys/lib/kstd.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 */
#define main ath9k_phy_rows
#include "ath9k_phy_hosttest.c"
#undef main

/** Run every row of ath9k_phy_hosttest at 250 Hz. */
int main(void)
{
    if (TICK_HZ != 250 || ath9k_phy_rows() != 0) {
        printf("ath9k_phy250_hosttest: failed\n");
        return 1;
    }
    printf("ath9k_phy250_hosttest: ok\n");
    return 0;
}
