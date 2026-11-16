/**
 * Check the lock counters' cycle conversion against native division.
 *
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/atomic_host.c
 *
 * Reuse the timer calibration's device stubs without running its rows.
 * Whole and fractional ticks, counts above 32 bits and a full-width
 * divisor must convert exactly. Returns 0 when every conversion agrees.
 */
#define main calibration_rows_main
#include "timercal_hosttest.c"
#undef main

/** Compare fixed readings against the host's independent native division. */
int main(void)
{
    static const uint32_t rates[] = {1u, 1000u, 4191900u, 0xFFFFFFFFu};
    static const uint64_t counts[] = {0, 1, 999, 4191899, 0x100000001ULL, 0x3FFFFFFFFFFFFFULL};
    unsigned int i, j;

    tsc_per_tick = 0;
    CHECK(timer_cycles_us(counts[5]) == 0, "no calibrated rate gives zero");
    for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        tsc_per_tick = rates[i];
        for (j = 0; j < sizeof(counts) / sizeof(counts[0]); j++) {
            uint64_t expected =
                (counts[j] / rates[i]) * TICK_US + ((counts[j] % rates[i]) * TICK_US) / rates[i];

            CHECK(timer_cycles_us(counts[j]) == expected, "cycles convert exactly");
        }
    }
    printf("cycleus-hosttest: result: %s\n", fails ? "fail" : "pass");
    return fails != 0;
}
