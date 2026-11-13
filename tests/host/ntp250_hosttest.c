/* Check the production NTP clock at 250 Hz: ntp_hosttest.c's rows with their
 * answers for a tick of 4 ms, and the clock's units.
 * host-deps: rach2/sys/ntp.c rach2/sys/tick.c
 * host-deps: tests/host/compat/spinlock_host.c
 */
#define main ntp_literal_rows
#include "ntp_hosttest.c"
#undef main

/** Check clock units, a frequency slew and the first PLL correction, then
 * run every row of ntp_hosttest.c at 250 Hz, whose last row leaves the PLL
 * at time constant 10 and a frequency set. */
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
    (void)ntp_literal_rows();
    printf("ntp250: %s: 4 ms ticks, raw time, frequency and PLL\n", fails ? "fail" : "pass");
    return fails != 0;
}
