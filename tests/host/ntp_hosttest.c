/**
 * Host test for the slew of the clocks by Linux's NTP state, and its leap
 * seconds.
 *
 * host-deps: rach2/sys/ntp.c rach2/sys/tick.c
 * host-cflags: -DTICK_HZ=1000
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * The test includes clock.c whole, so that each row starts its clocks from
 * a tick's edge, and drives it as the timer pass does (kernel.c): one raw
 * tick a pass, the tick length read before it, `rtc_tick_advance` within
 * the tick's sequence count, and `rtc_seconds` for each second of real time
 * begun. The vDSO's clock does not run, so every reading is the tick's. The
 * monotonic clock is read with no floor (`rtc_tick_phase`) and real time as
 * that clock plus the offset (`rtc_offset`), so that a reading that went
 * back or jumped would show. The rows: an offset the PLL slews, forward and
 * back, the monotonic clock with it, with the offset, the boot time and the
 * step count unmoved and no timer told; the frequency the PLL makes of a
 * second offset, counted from the locked clock's seconds even when the
 * fine reply time has crossed the next second; an FLL interval past 32
 * bits, made by a clock setting without advancing its ticks; a frequency,
 * kept across a setting of the clock, with the raw clock unslewed and the
 * place within the tick slewed at 100 and 500 ppm; adjtime's offset slewed
 * at 500 ppm and read back; a shift of the clock, which steps it, forgets
 * the offset, counts a step and tells the
 * timers, and is refused, though it forgets and tells them still and counts
 * no step, for a time before the monotonic clock or before 1970, as is a
 * shift of nothing; and a leap second inserted at midnight,
 * as adjtimex reads it before and after the tick that inserts it, and one
 * deleted.
 *
 * The answers are Linux's arithmetic done by hand for a tick of 1000 Hz and
 * the time constant 2 (kernel/time/ntp.c:296-346, 400-495,
 * kernel/time/timekeeping.c:2115-2193): each raw tick adds the tick length,
 * and the NTP state's second comes at each second of real time, whose tick
 * length the next tick takes. The PLL slews a sixteenth of what is left
 * each second: 31.25 us a tick from the first second, so the second second
 * ends after 970 ticks, and 29.296875 us from there; the clock reads
 * 2.031191406 s after 2000 ticks and 4.087701988 s after 4000, with 386238
 * us of the offset left. An offset of 1 ms four seconds after the last
 * makes a frequency of 1e6 * 4 << 20 nanoseconds a second times 2^32, which
 * adjtimex reads as 64000; 100 ppm reads as 6553600 and adds exactly 100 ns
 * to each tick after the one in progress. adjtime's 1.2 ms adds 500 ns to
 * each of the first second's 1000 ticks and the second's 1000, and 200 ns
 * to the third's 999, 1.1998 ms in all. A rate of 100 ppm is 100 * 2^45 /
 * 10^6 units of 2^-45, cut to 3518437208, which makes 2.5 ms of the raw
 * clock 2500249 ns of the monotonic clock, the truncation costing under a
 * nanosecond; 500 ppm, 17592186044, makes it 2501249 ns.
 *
 * Returns the failed-check count (0 == all passed), capped so it cannot wrap
 * to a false 0.
 */

#include "clock.c"

#if TICK_HZ != 1000 && !defined(NTP250_MODEL)
#error "the literal answers are for TICK_HZ 1000"
#endif

extern int printf(const char *fmt, ...);

static int fails;

/** Count a failure when `cond` is false, and print its file, line and
 *  condition. */
#define CHECK(cond)                                                   \
    do {                                                              \
        if (!(cond)) {                                                \
            fails++;                                                  \
            printf("  FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                             \
    } while (0)

#define NS 1000000000LL

volatile uint64_t kernel_ticks;
volatile uint32_t kernel_uptime_seconds;
volatile uint32_t kernel_second_ticks;

/* The calls of `timerfd_clock_was_set`; the ticks and the place within the
 * tick the timer has not counted, which `timer_phase` gives; and the last
 * readings of the monotonic clock and real time, which `pass` compares the
 * next with. */
static int cancels;
static int phase_pending;
static uint32_t phase_ns;
static int64_t last_mono, last_real;

/** The vDSO's clock does not run here, so the tick keeps every clock. */
int vclock_read(int which, int coarse, int64_t *sec, int64_t *nsec)
{
    (void)which;
    (void)coarse;
    (void)sec;
    (void)nsec;
    return 0;
}

/** No vDSO page takes the offset here. */
void vclock_set_wall(int64_t sec, int32_t nsec)
{
    (void)sec;
    (void)nsec;
}

/** Count the calls that tell the timers of a setting, whose cancels
 * timerfd.c decides by real time's offset. */
void timerfd_clock_was_set(void)
{
    cancels++;
}

/** The raw ticks the handler has not counted and the place within the
 * tick, which a row sets; none between the rows' passes. */
void timer_phase(int *pending, uint32_t *phase)
{
    *pending = phase_pending;
    *phase = phase_ns;
}

/** Mask interrupts, which here means nothing. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Restore the mask `flags`. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/** No row spins; a call fails the test. */
void arch_relax(void)
{
    CHECK(0);
}

/** No row waits for another processor; a call fails the test. */
void smp_service(void)
{
    CHECK(0);
}

/** The monotonic clock now in nanoseconds, with no floor. */
static int64_t mono_now(void)
{
    uint64_t ticks;
    uint32_t phase;

    rtc_tick_phase(&ticks, &phase);
    return (int64_t)ticks * TICK_NS + phase;
}

/** Real time now in nanoseconds, the monotonic clock plus the offset. */
static int64_t real_now(void)
{
    int64_t sec;
    int32_t nsec;

    rtc_offset(&sec, &nsec);
    return mono_now() + sec * NS + nsec;
}

/** The raw clock now in nanoseconds. */
static int64_t raw_now(void)
{
    int64_t sec, nsec;

    rtc_raw_monotonic(&sec, &nsec);
    return sec * NS + nsec;
}

/** `n` timer passes of a raw tick each, as kernel.c's makes them; each must
 * move the monotonic clock by a tick within 3.2%, the largest slew a row
 * makes, and real time with it, so that neither goes back or jumps, but by
 * a leap second's step. */
static void pass(int n)
{
    int64_t length, m, r, leap;
    uint32_t seconds, steps;

    while (n-- > 0) {
        steps = rtc_step_count();
        length = rtc_tick_length();
        tick_write_begin();
        seconds = rtc_tick_advance(1, length);
        tick_write_end();
        if (seconds != 0) {
            rtc_seconds(seconds);
        }
        m = mono_now();
        r = real_now();
        leap = rtc_step_count() != steps ? r - m - (last_real - last_mono) : 0;
        if (m - last_mono < TICK_NS * 968u / 1000u || m - last_mono > TICK_NS * 1032u / 1000u ||
            r - last_real - leap != m - last_mono) {
            CHECK(m - last_mono >= TICK_NS * 968u / 1000u &&
                  m - last_mono <= TICK_NS * 1032u / 1000u);
            CHECK(r - last_real - leap == m - last_mono);
            printf("  at %lld ns: the clock moved %lld ns, real time %lld ns\n", (long long)m,
                   (long long)(m - last_mono), (long long)(r - last_real));
        }
        last_mono = m;
        last_real = r;
    }
}

/** Start the clocks at a tick's edge, 100 s after boot, with no floor an
 * earlier row's readings raised (`rtc_forget_floors`), set real time to
 * `sec` seconds there, which takes the NTP state's tick length at once,
 * and return real time in nanoseconds. */
static int64_t restart(int64_t sec)
{
    rtc_forget_floors();
    kernel_ticks = 100u * TICK_HZ;
    kernel_uptime_seconds = 100;
    kernel_second_ticks = 0;
    rtc_part = 0;
    CHECK(rtc_set_realtime(sec, 0) == 0);
    rtc_applied = rtc_tick_length();
    rtc_rate = rtc_rate_of(rtc_applied);
    last_mono = mono_now();
    last_real = real_now();
    CHECK(last_mono == 100 * NS && last_real == sec * NS);
    return last_real;
}

/** adjtimex with `modes`, `status`, `offset` and `freq`, the maximum error
 * 0 so that the clock stays synchronized, into `*tx`; returns its state. */
static int32_t adj(struct ktimex *tx, uint32_t modes, int32_t status, int64_t offset, int64_t freq)
{
    static const struct ktimex zero;

    *tx = zero;
    tx->modes = modes;
    tx->status = status;
    tx->offset = offset;
    tx->freq = freq;
    return rtc_adjtimex(tx);
}

/** Turn the PLL off and on, which starts its interval at the time now. */
static void pll_restart(void)
{
    struct ktimex tx;

    adj(&tx, ADJ_STATUS, 0, 0, 0);
    CHECK(adj(&tx, ADJ_STATUS | ADJ_MAXERROR, STA_PLL, 0, 0) == TIME_OK);
}

/** An offset of 500 ms, the most the PLL takes, slews a sixteenth of what
 * is left each second from the second after, the monotonic clock and real
 * time alike, with real time's offset, the boot time and the step count
 * unmoved and no timer told; and one of -500 ms slows them, never back,
 * with the same four unmoved. */
static void offset_rows(void)
{
    struct ktimex tx;
    int64_t base, offset_sec, epoch;
    int32_t offset_nsec, nsec;
    uint32_t steps;
    int c;

    base = restart(1700000000);
    pll_restart();
    rtc_offset(&offset_sec, &offset_nsec);
    epoch = rtc_boot_epoch();
    steps = rtc_step_count();
    c = cancels;
    CHECK(adj(&tx, ADJ_OFFSET, 0, 500000, 0) == TIME_OK);
    pass(1000);
    CHECK(real_now() - base == 1 * NS);
    pass(1000);
    CHECK(real_now() - base == 2031191406);
    CHECK(mono_now() - 100 * NS == 2031191406);
    pass(2000);
    CHECK(real_now() - base == 4087701988);
    adj(&tx, 0, 0, 0, 0);
    CHECK(tx.offset == 386238 && tx.freq == 0);
    rtc_offset(&base, &nsec);
    CHECK(base == offset_sec && nsec == offset_nsec && rtc_boot_epoch() == epoch);
    CHECK(rtc_step_count() == steps && cancels == c);

    base = restart(1700000100);
    pll_restart();
    rtc_offset(&offset_sec, &offset_nsec);
    epoch = rtc_boot_epoch();
    steps = rtc_step_count();
    c = cancels;
    adj(&tx, ADJ_OFFSET, 0, -500000, 0);
    pass(1000);
    CHECK(real_now() - base == 1 * NS);
    pass(2000);
    CHECK(real_now() - base == 2939388671);
    CHECK(mono_now() - 100 * NS == 2939388671);
    adj(&tx, 0, 0, 0, 0);
    CHECK(tx.offset == -439453);
    rtc_offset(&base, &nsec);
    CHECK(base == offset_sec && nsec == offset_nsec && rtc_boot_epoch() == epoch);
    CHECK(rtc_step_count() == steps && cancels == c);
}

/** A second offset four seconds after the PLL's interval began becomes a
 * frequency, 64000 in adjtimex's units. */
static void pll_frequency_row(void)
{
    struct ktimex tx;

    restart(1700000200);
    pll_restart();
    pass(4000);
    CHECK(real_now() == 1700000204LL * NS);
    adj(&tx, ADJ_OFFSET, 0, 1000, 0);
    CHECK(tx.freq == 64000);
    adj(&tx, ADJ_STATUS | ADJ_FREQUENCY, 0, 0, 0);
    CHECK(tx.freq == 0);
}

/**
 * The fine reply clock crosses second 104 before the tick after 103.999:
 * the PLL counts three seconds since its reference at 100, so a 1 ms
 * offset reads frequency 48000, not 64000. Enabling the PLL in that window
 * also starts its reference at 103. Linux 6.12.115 uses the locked
 * timekeeper's seconds for both (kernel/time/ntp.c:321-325, 708-709), and
 * the earlier fine sample for the reply (ntp.c:829-832).
 */
static void pll_coarse_rows(void)
{
    struct ktimex tx;

    restart(100);
    pll_restart();
    pass(3999);
    phase_pending = 1;
    CHECK(adj(&tx, ADJ_OFFSET, 0, 1000, 0) == TIME_OK);
    CHECK(tx.sec == 104 && tx.usec == 0 && tx.freq == 48000);
    phase_pending = 0;
    adj(&tx, ADJ_STATUS | ADJ_FREQUENCY, 0, 0, 0);

    restart(100);
    pass(3999);
    phase_pending = 1;
    pll_restart();
    CHECK(adj(&tx, ADJ_OFFSET, 0, 1000, 0) == TIME_OK);
    CHECK(tx.sec == 104 && tx.freq == 0);
    phase_pending = 0;
    pass(1001);
    CHECK(adj(&tx, ADJ_OFFSET, 0, 1000, 0) == TIME_OK);
    CHECK(tx.freq == 32000);
    adj(&tx, ADJ_STATUS | ADJ_FREQUENCY, 0, 0, 0);
}

/**
 * A permitted step 8,000,000,000 seconds ahead preserves the PLL reference.
 * At time constant 10, a 400 ms offset reads frequency 800000: the PLL
 * adds 800000 and the FLL less than one readback unit. Dividing by a capped
 * 32-bit interval gave 800001. Linux 6.12.115's div64_long uses the full
 * interval on x86-64 (kernel/time/ntp.c:293); no ticks through it are needed.
 */
static void fll_long_row(void)
{
    struct ktimex tx = {0};

    restart(100);
    pll_restart();
    CHECK(rtc_set_realtime(8000000100LL, 0) == 0);
    tx.modes = ADJ_STATUS | ADJ_NANO | ADJ_TIMECONST | ADJ_FREQUENCY | ADJ_OFFSET;
    tx.status = STA_PLL;
    tx.constant = 10;
    tx.offset = 400000000;
    CHECK(rtc_adjtimex(&tx) == TIME_OK);
    CHECK(tx.freq == 800000 && (tx.status & STA_MODE));
}

/** A frequency of 100 ppm adds 100 ns to each tick from the next, which no
 * timer is told of, and a setting of the clock keeps it; the raw clock runs
 * on unslewed, and the place within the tick and the ticks the handler has
 * not counted are slewed at 100 ppm, and at 500 ppm. */
static void frequency_row(void)
{
    struct ktimex tx;
    int64_t base, raw;
    uint64_t ticks;
    uint32_t phase;
    int c;

    base = restart(1700000300);
    c = cancels;
    adj(&tx, ADJ_FREQUENCY, 0, 0, 100LL << 16);
    CHECK(tx.freq == 6553600 && cancels == c);
    pass(10000);
    CHECK(real_now() - base - 10 * NS == 999900);
    base = restart(1700000400);
    raw = raw_now();
    pass(1000);
    CHECK(real_now() - base - 1 * NS == 100000);
    CHECK(raw_now() - raw == 1 * NS);
    adj(&tx, 0, 0, 0, 0);
    CHECK(tx.freq == 6553600);

    restart(1700000500);
    CHECK(rtc_mono_rate() == 3518437208LL);
    CHECK(rtc_slewed(2500000, rtc_mono_rate()) == 2500249);
    phase_pending = 2;
    phase_ns = 500000;
    rtc_tick_phase(&ticks, &phase);
    CHECK(ticks == 100002 && phase == 500249);
    phase_pending = 0;
    phase_ns = 0;

    adj(&tx, ADJ_FREQUENCY, 0, 0, 500LL << 16);
    restart(1700000600);
    CHECK(rtc_mono_rate() == 17592186044LL);
    phase_pending = 2;
    phase_ns = 500000;
    rtc_tick_phase(&ticks, &phase);
    CHECK(ticks == 100002 && phase == 501249);
    phase_pending = 0;
    phase_ns = 0;
    adj(&tx, ADJ_FREQUENCY, 0, 0, 0);
}

/** adjtime's 1.2 ms is slewed at 500 ppm from the second after, 500 us,
 * 500 us, then 200 us a second, and what is left reads back each second,
 * while the PLL's offset reads 0 and nothing cancels. */
static void singleshot_row(void)
{
    struct ktimex tx;
    int64_t base;
    int c;

    base = restart(1700000700);
    c = cancels;
    adj(&tx, ADJ_OFFSET_SINGLESHOT, 0, 1200, 0);
    CHECK(tx.offset == 0);
    adj(&tx, ADJ_OFFSET_SS_READ, 0, 0, 0);
    CHECK(tx.offset == 1200);
    pass(1000);
    CHECK(real_now() - base == 1 * NS);
    adj(&tx, ADJ_OFFSET_SS_READ, 0, 0, 0);
    CHECK(tx.offset == 700);
    pass(1000);
    CHECK(real_now() - base == 2000500000);
    adj(&tx, ADJ_OFFSET_SS_READ, 0, 0, 0);
    CHECK(tx.offset == 200);
    pass(1000);
    CHECK(real_now() - base == 3001000000LL);
    adj(&tx, ADJ_OFFSET_SS_READ, 0, 0, 0);
    CHECK(tx.offset == 0);
    pass(2000);
    CHECK(real_now() - base == 5001199800LL);
    adj(&tx, 0, 0, 0, 0);
    CHECK(tx.offset == 0 && cancels == c);
}

/** ADJ_SETOFFSET's shift steps real time at once, the boot time with it,
 * forgets the PLL's offset, whose tick length the tick in progress keeps,
 * counts a step and tells the timers; a shift to a time before the
 * monotonic clock, though after 1970, is refused, and so is one before
 * 1970, and each still forgets and tells the timers, as Linux's does
 * (kernel/time/timekeeping.c:1409-1426), but counts no step, since the
 * offset is as it was; a shift of nothing counts none either. */
static void setoffset_row(void)
{
    struct ktimex tx;
    int64_t before, base, epoch;
    uint32_t steps;
    int c;

    restart(1700000800);
    pll_restart();
    adj(&tx, ADJ_OFFSET, 0, 500000, 0);
    pass(1000);
    c = cancels;
    epoch = rtc_boot_epoch();
    before = real_now();
    steps = rtc_step_count();
    CHECK(rtc_shift_realtime(5, 0) == 0);
    CHECK(real_now() - before == 5 * NS && cancels == c + 1 && rtc_boot_epoch() == epoch + 5);
    CHECK(rtc_step_count() != steps);
    base = last_real = real_now();
    pass(1000);
    CHECK(real_now() - base == 1000031250);
    adj(&tx, 0, 0, 0, 0);
    CHECK(tx.offset == 0 && (tx.status & STA_UNSYNC));
    before = real_now();
    steps = rtc_step_count();
    CHECK(rtc_shift_realtime(50 - 1700000806, 0) == -1);
    CHECK(real_now() == before && cancels == c + 2);
    CHECK(rtc_shift_realtime(-1800000000, 0) == -1);
    CHECK(real_now() == before && cancels == c + 3);
    CHECK(rtc_shift_realtime(0, 0) == 0);
    CHECK(cancels == c + 4 && rtc_step_count() == steps);
}

/** STA_INS announces a leap second at the end of the day, 1700006400 s,
 * a midnight in UTC: the second before it is TIME_INS; adjtimex at
 * midnight before the tick that inserts it reads TIME_OOP, 23:59:59 and a
 * TAI offset one more; the tick steps real time back to 23:59:59, which
 * the boot time, the floor and the step count follow, and cancels; the
 * second after is TIME_WAIT until STA_INS is cleared, and then TIME_OK.
 * STA_DEL deletes 23:59:59 the same way, real time stepping on to
 * midnight. */
static void leap_rows(void)
{
    struct ktimex tx;
    int64_t base, epoch, sec, nsec;
    uint32_t steps;
    int c;

    base = restart(1700006398);
    CHECK(adj(&tx, ADJ_STATUS | ADJ_MAXERROR, STA_INS, 0, 0) == TIME_OK);
    pass(1000);
    CHECK(adj(&tx, 0, 0, 0, 0) == TIME_INS && tx.sec == 1700006399);
    pass(999);
    phase_pending = 1;
    CHECK(adj(&tx, 0, 0, 0, 0) == TIME_OOP && tx.sec == 1700006399 && tx.tai == 1);
    phase_pending = 0;
    c = cancels;
    steps = rtc_step_count();
    epoch = rtc_boot_epoch();
    pass(1);
    CHECK(real_now() - base == 1 * NS && cancels == c + 1 && rtc_step_count() != steps);
    CHECK(rtc_boot_epoch() == epoch - 1);
    rtc_realtime(&sec, &nsec);
    CHECK(sec == 1700006399 && nsec == 0);
    CHECK(adj(&tx, 0, 0, 0, 0) == TIME_OOP && tx.tai == 1);
    pass(1000);
    CHECK(real_now() == 1700006400LL * NS);
    CHECK(adj(&tx, 0, 0, 0, 0) == TIME_WAIT);
    adj(&tx, ADJ_STATUS, 0, 0, 0);
    pass(1000);
    CHECK(adj(&tx, 0, 0, 0, 0) == TIME_OK);

    base = restart(1700006397);
    CHECK(adj(&tx, ADJ_STATUS | ADJ_MAXERROR | ADJ_TAI, STA_DEL, 0, 0) == TIME_OK);
    pass(1000);
    CHECK(adj(&tx, 0, 0, 0, 0) == TIME_DEL);
    c = cancels;
    pass(1000);
    CHECK(real_now() == 1700006400LL * NS && cancels == c + 1);
    CHECK(adj(&tx, 0, 0, 0, 0) == TIME_WAIT && tx.tai == -1);
    adj(&tx, ADJ_STATUS, 0, 0, 0);
    pass(1000);
    CHECK(adj(&tx, 0, 0, 0, 0) == TIME_OK);
    (void)base;
}

int main(void)
{
    offset_rows();
    pll_frequency_row();
    pll_coarse_rows();
    frequency_row();
    singleshot_row();
    setoffset_row();
    leap_rows();
    fll_long_row();
    if (fails != 0) {
        printf("ntp_hosttest: %d failure(s)\n", fails);
    } else {
        printf("ntp_hosttest: ok\n");
    }
    return fails > 99 ? 99 : fails;
}
