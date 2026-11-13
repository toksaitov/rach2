/* Check the production NTP clock at 250 Hz beside the 1000 Hz literals.
 * host-deps: rach2/sys/ntp.c rach2/sys/tick.c
 * host-deps: tests/host/compat/spinlock_host.c
 */
#define NTP250_MODEL 1
#define main         ntp_literal_rows
#include "ntp_hosttest.c"
#undef main

/** Check clock units, a frequency slew and the first PLL correction. */
int main(void)
{
    struct ktimex tx;
    int64_t base, raw;

    CHECK(TICK_HZ == 250 && TICK_NS == 4000000u);
    base = restart(1700000000);
    raw = raw_now();
    adj(&tx, ADJ_FREQUENCY, 0, 0, 100LL << 16);
    pass(2500);
    CHECK(real_now() - base == 10000999600LL);
    CHECK(raw_now() - raw == 10 * NS);
    CHECK(rtc_mono_rate() == 3518437208LL);
    adj(&tx, ADJ_FREQUENCY, 0, 0, 0);
    base = restart(1700000100);
    pll_restart();
    CHECK(adj(&tx, ADJ_OFFSET, 0, 500000, 0) == TIME_OK);
    pass(250);
    CHECK(real_now() - base == NS);
    pass(1);
    CHECK(real_now() - base == NS + 4125000);
    printf("ntp250: %s: 4 ms ticks, raw time, frequency and PLL\n", fails ? "fail" : "pass");
    return fails != 0;
}
