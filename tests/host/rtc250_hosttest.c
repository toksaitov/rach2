/**
 * Check the clock's PIT interpolation, its setting, the reference counter's
 * ticks and the coarse clocks at the production tick of 250 Hz, beside the
 * 1000 Hz literals of rtc_hosttest.c.
 *
 * host-deps: rach2/sys/arch/x86/cmos.c rach2/sys/clock.c
 * host-deps: rach2/sys/arch/x86/pit.c tests/host/compat/spinlock_host.c
 * host-deps: rach2/sys/ntp.c rach2/sys/tick.c
 * host-cflags: -DRTC_HOST_TEST -DRACH2_HOST_CPU
 *
 * The rows are rtc_hosttest.c's clock rows with their answers worked by hand
 * for a tick of 4 ms. The PIT's reload is Linux's PIT_LATCH at HZ=250,
 * (1193182 + 125) / 250 = 4773 (include/linux/i8253.h:21), whose half
 * period, a sweep in mode 3, reads as 4773 * 419 / 1000 = 1999 us, so a
 * period reads as 3998 us; 100 counts into a sweep read as 41 us, and 2
 * counts short of the second sweep's end as 4771 * 419 / 1000 + 1999 =
 * 3998 us. A tick of the 1 MHz reference is 4000 counts, and the reference
 * starts with its edge half a tick, 2000 counts, before the counter
 * (`ref_start`); a minute's ticks, the most one interrupt counts, are 15000.
 */
#define RTC250_MODEL 1
#define main         rtc_literal_rows
#include "rtc_hosttest.c"
#undef main

#if TICK_HZ != 250
#error "this test checks the production tick of 250 Hz"
#endif

/** The PIT's place within the tick at 4773 counts a period: a pending wrap
 * counts once, also at the end of a second, and a reading is never earlier
 * than the one before. */
static void subtick250_rows(void)
{
    int64_t sec, ns;

    CHECK(pit_reload == 4773u && pit_period_us() == 3998u);
    rtc_forget_floors();
    kernel_uptime_seconds = 10;
    kernel_second_ticks = 0;
    pending = 0;
    wrap_on_pending = 0;
    count = pit_reload - 100u;
    status = 0x80;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 10 && ns == 41000);
    count -= 100u;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 10 && ns == 83000);
    status = 0;
    count = 2;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 10 && ns == 3998000);
    pending = 1;
    status = 0x80;
    count = pit_reload - 100u;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 10 && ns == 4041000);
    pending = 0;
    kernel_second_ticks = 1;
    count -= 100u;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 10 && ns == 4083000);
    kernel_second_ticks = 249;
    status = 0;
    count = 2;
    wrap_on_pending = 2;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 11 && ns == 41000);
    pending = 0;
    kernel_uptime_seconds = 11;
    kernel_second_ticks = 0;
    count -= 100u;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 11 && ns == 83000);
    count = pit_reload + 5u;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 11 && ns == 83000);
    pending = 1;
    count = 2;
    status = 0;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 11 && ns == 7998000);
    count = pit_reload - 100u;
    status = 0x80;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 11 && ns == 7998000);
    pending = 0;
    kernel_second_ticks = 2;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 11 && ns == 8041000);
    CHECK(!irq_disabled);
}

/** A setting of the clock at 20.000041 s goes on at 4 ms a tick, the coarse
 * clock leaves out the PIT's 41 us, and the boot epoch moves with a step
 * back to 1000 s at 20.004041 s, to 979.995959 s, whose seconds are 979. */
static void set250_rows(void)
{
    int64_t sec, ns;

    rtc_forget_floors();
    kernel_uptime_seconds = 20;
    kernel_second_ticks = 0;
    pending = 0;
    wrap_on_pending = 0;
    status = 0x80;
    count = pit_reload - 100u;
    CHECK(rtc_set_realtime(1790000000LL, 500000000) == 0);
    rtc_realtime(&sec, &ns);
    CHECK(sec == 1790000000LL && ns == 500000000);
    kernel_second_ticks = 1;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 1790000000LL && ns == 504000000);
    rtc_coarse_realtime(&sec, &ns);
    CHECK(sec == 1790000000LL && ns == 503959000);
    CHECK(rtc_set_realtime(1000, 0) == 0);
    rtc_realtime(&sec, &ns);
    CHECK(sec == 1000 && ns == 0);
    CHECK(rtc_boot_epoch() == 979);
}

/** Time since boot at the same states: a pending tick adds 4 ms, a wrap at
 * the second's last tick counts once, the floor holds, and a setting of real
 * time does not move it. */
static void monotonic250_rows(void)
{
    int64_t sec, ns;

    rtc_forget_floors();
    wrap_on_pending = 0;
    pending = 0;
    kernel_uptime_seconds = 50;
    kernel_second_ticks = 0;
    count = pit_reload - 100u;
    status = 0x80;
    rtc_monotonic(&sec, &ns);
    CHECK(sec == 50 && ns == 41000);
    pending = 1;
    rtc_monotonic(&sec, &ns);
    CHECK(sec == 50 && ns == 4041000);
    pending = 0;
    kernel_second_ticks = 249;
    status = 0;
    count = 2;
    wrap_on_pending = 2;
    rtc_monotonic(&sec, &ns);
    CHECK(sec == 51 && ns == 41000);
    pending = 0;
    kernel_second_ticks = 249;
    status = 0x80;
    count = pit_reload - 100u;
    rtc_monotonic(&sec, &ns);
    CHECK(sec == 51 && ns == 41000);
    kernel_uptime_seconds = 51;
    kernel_second_ticks = 1;
    CHECK(rtc_set_realtime(1000, 0) == 0);
    rtc_monotonic(&sec, &ns);
    CHECK(sec == 51 && ns == 4041000);
    CHECK(!irq_disabled);
}

/** The reference counter's ticks of 4000 counts, from its edge 2000 counts
 * before the start: one on time, none for the same delivered again, seven
 * for one seven and a half ticks late past the counter's wrap; another
 * processor three ticks and 10 us on reads three pending, and time since
 * boot two ticks before a second's turn plus those 12 ms and 10 us; and an
 * interrupt seventy seconds late counts 15000 ticks, the next the 2503 left:
 * the edges from 34000 counts to 70042010, (70042010 - 34000) / 4000 + 1. */
static void reference250_rows(void)
{
    const uint32_t start = 0xFFFFF000u;
    uint32_t base, phase_ns;
    int64_t sec, ns;
    int due;

    rtc_forget_floors();
    ref_on = 1;
    ref_now = start;
    pending = 0;
    wrap_on_pending = 0;
    status = 0x80;
    count = pit_reload - 100u;
    CHECK(timer_tick_clock() == 1u);
    base = timer_now_us();
    CHECK(base == 3998u + 41u);
    ref_now = start + 4000u;
    CHECK(timer_tick_clock() == 1u);
    ref_now = start + 4040u;
    CHECK(timer_tick_clock() == 0u);
    ref_now = start + 32040u;
    CHECK(timer_tick_clock() == 7u);
    CHECK(timer_now_us() == base + 32040u);
    host_cpu = 1;
    ref_now = start + 42010u;
    timer_phase(&due, &phase_ns);
    CHECK(due == 3 && phase_ns == 10000u);
    CHECK(timer_now_us() == base + 42010u);
    kernel_uptime_seconds = 60;
    kernel_second_ticks = 248;
    rtc_monotonic(&sec, &ns);
    CHECK(sec == 61 && ns == 4010000);
    host_cpu = 0;
    CHECK(timer_now_us() == base + 42010u);
    ref_now += 70000000u;
    CHECK(timer_tick_clock() == 15000u);
    CHECK(timer_tick_clock() == 2503u);
    CHECK(!irq_disabled);
    ref_on = 0;
}

/** The coarse clock after the vDSO's clock stops, ten ticks into second 100,
 * reads 40 ms, within that tick of 4 ms. */
static void coarse250_rows(void)
{
    int64_t sec, ns;

    rtc_forget_floors();
    kernel_uptime_seconds = 100;
    kernel_second_ticks = 0;
    vclock_coarse_ns = 100005000000ULL;
    vclock_on = 1;
    rtc_vclock_change(vclock_model_stop);
    CHECK(!vclock_on);
    kernel_second_ticks = 10;
    rtc_coarse_monotonic(&sec, &ns);
    CHECK(sec == 100 && ns >= 40000000 && ns < 44000000);
    CHECK(!irq_disabled);
}

/** Run the rows; the failed-check count, capped below 256. */
int main(void)
{
    CHECK(timer_clock_check());
    subtick250_rows();
    set250_rows();
    monotonic250_rows();
    reference250_rows();
    coarse250_rows();
    printf("rtc250: %s: 4 ms ticks, PIT_LATCH 4773, the reference and the coarse clocks\n",
           fails ? "fail" : "pass");
    return fails > 255 ? 255 : fails;
}
