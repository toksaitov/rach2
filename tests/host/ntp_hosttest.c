/**
 * Host test for the slew of real time by Linux's NTP state.
 *
 * host-deps: rach2/sys/clock.c rach2/sys/ntp.c rach2/sys/tick.c
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * The test keeps the monotonic clock itself, through the reading the vDSO's
 * clock would give (`vclock_read`), and moves it a millisecond at a time,
 * calling `rtc_seconds` at each second as the tick does. Real time is that
 * clock plus the course's offset (`rtc_offset`), read with no floor, so a
 * reading that went back or jumped would show. The rows: an offset the PLL
 * slews, forward and back; the frequency the PLL makes of a second offset;
 * a frequency, kept across a setting of the clock; adjtime's offset slewed
 * at 500 ppm and read back; and a shift of the clock, which steps it,
 * forgets the offset and cancels the timers that ask for it, where the
 * slews cancel nothing.
 *
 * The answers are Linux's arithmetic done by hand for a tick of 1000 Hz and
 * the time constant 2 (kernel/time/ntp.c:296-346, 400-495): the PLL slews a
 * sixteenth of what is left each second, 500 ms giving 31.25 ms, then
 * 29.296875 ms, then 27.4658203125 ms, all exact in the clock's rate, and
 * leaves 386238 us after four seconds; an offset of 1 ms four seconds after
 * the last makes a frequency of 1e6 * 4 << 20 nanoseconds a second times
 * 2^32, which adjtimex reads as 64000; 100 ppm reads as 6553600. Rates of
 * 100 and 500 ppm are not exact in units of 2^-45, and their rows allow the
 * nanosecond the truncation costs.
 *
 * Returns the failed-check count (0 == all passed), capped so it cannot wrap
 * to a false 0.
 */

#include "rtc.h"
#include "ntp.h"
#include "timer.h"
#include "vdso.h"

#if TICK_HZ != 1000
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

/* The monotonic clock, nanoseconds; the calls of `timerfd_clock_was_set`
 * that cancel and those that count again; and the last reading of real
 * time, which `advance` compares the next with. */
static int64_t mono_ns;
static int cancels, recounts;
static int64_t last_real;

/** The monotonic clock is the test's; real time is left to clock.c's
 * course, which is what is under test. */
int vclock_read(int real, int coarse, int64_t *sec, int64_t *nsec)
{
    (void)coarse;
    if (real) {
        return 0;
    }
    *sec = mono_ns / NS;
    *nsec = mono_ns % NS;
    return 1;
}

/** No vDSO page takes the course here. */
void vclock_set_course(const struct rtc_course *c, int step)
{
    (void)c;
    (void)step;
}

/** Count the timers' cancels and counts again. */
void timerfd_clock_was_set(int cancel)
{
    if (cancel) {
        cancels++;
    } else {
        recounts++;
    }
}

/** The monotonic clock never comes from the timer here; a call fails the
 * test. */
void timer_phase(int *pending, uint32_t *phase_ns)
{
    *pending = 0;
    *phase_ns = 0;
    CHECK(0);
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

/** Real time now, in nanoseconds, from the course alone. */
static int64_t real_now(void)
{
    int64_t sec;
    int32_t nsec;

    rtc_offset(&sec, &nsec);
    return mono_ns + sec * NS + nsec;
}

/** Move the monotonic clock on `ms` milliseconds, calling `rtc_seconds` at
 * each second; real time must move each millisecond by its length within
 * 3.2%, the largest slew a row makes, so that it never goes back or
 * jumps, a change of rate included. */
static void advance(int ms)
{
    int64_t r;

    while (ms-- > 0) {
        mono_ns += 1000000;
        if (mono_ns % NS == 0) {
            rtc_seconds(1);
        }
        r = real_now();
        if (r - last_real < 968000 || r - last_real > 1032000) {
            CHECK(r - last_real >= 968000 && r - last_real <= 1032000);
            printf("  at %lld ns: real time moved %lld ns\n", (long long)mono_ns,
                   (long long)(r - last_real));
        }
        last_real = r;
    }
}

/** Set real time to `sec` seconds and return it in nanoseconds. */
static int64_t set_clock(int64_t sec)
{
    CHECK(rtc_set_realtime(sec, 0) == 0);
    last_real = real_now();
    CHECK(last_real == sec * NS);
    return last_real;
}

/** adjtimex with `modes`, `status`, `offset` and `freq`, into `*tx`;
 * returns its state. */
static int32_t adj(struct ktimex *tx, uint32_t modes, int32_t status, int64_t offset, int64_t freq)
{
    tx->modes = modes;
    tx->status = status;
    tx->offset = offset;
    tx->freq = freq;
    tx->constant = 0;
    tx->tick = 0;
    return rtc_adjtimex(tx);
}

/** Turn the PLL off and on, which starts its interval at the time now. */
static void pll_restart(void)
{
    struct ktimex tx;

    adj(&tx, ADJ_STATUS, 0, 0, 0);
    CHECK(adj(&tx, ADJ_STATUS, STA_PLL, 0, 0) == TIME_OK);
}

/** An offset of 500 ms, the most the PLL takes, slews a sixteenth of what
 * is left each second from the second after, and steps nothing; and one of
 * -500 ms slows real time without its going back. */
static void offset_rows(void)
{
    struct ktimex tx;
    int64_t base;
    uint32_t steps;
    int c = cancels;

    base = set_clock(1700000000);
    c = cancels;
    pll_restart();
    steps = rtc_step_count();
    CHECK(adj(&tx, ADJ_OFFSET, 0, 500000, 0) == TIME_OK);
    CHECK(rtc_step_count() == steps);
    advance(1000);
    CHECK(real_now() - base == 1 * NS);
    advance(1000);
    CHECK(real_now() - base == 2 * NS + 31250000);
    advance(1000);
    CHECK(real_now() - base == 3 * NS + 60546875);
    advance(1000);
    CHECK(real_now() - base - 4 * NS >= 88012694 && real_now() - base - 4 * NS <= 88012695);
    adj(&tx, 0, 0, 0, 0);
    CHECK(tx.offset == 386238 && tx.freq == 0);
    CHECK(rtc_step_count() != steps && cancels == c);

    base = set_clock(1700000100);
    pll_restart();
    adj(&tx, ADJ_OFFSET, 0, -500000, 0);
    advance(1000);
    CHECK(real_now() - base == 1 * NS);
    advance(2000);
    CHECK(real_now() - base == 3 * NS - 60546875);
    adj(&tx, 0, 0, 0, 0);
    CHECK(tx.offset == -411987);
}

/** A second offset four seconds after the PLL's interval began becomes a
 * frequency, 64000 in adjtimex's units. */
static void pll_frequency_row(void)
{
    struct ktimex tx;

    set_clock(1700000200);
    pll_restart();
    advance(4000);
    CHECK(real_now() == 1700000204LL * NS);
    adj(&tx, ADJ_OFFSET, 0, 1000, 0);
    CHECK(tx.freq == 64000);
    adj(&tx, ADJ_STATUS | ADJ_FREQUENCY, 0, 0, 0);
    CHECK(tx.freq == 0);
}

/** A frequency of 100 ppm runs real time 100 us a second fast at once, and
 * a setting of the clock keeps it. */
static void frequency_row(void)
{
    struct ktimex tx;
    int64_t base, gain;
    int r = recounts;

    base = set_clock(1700000300);
    adj(&tx, ADJ_FREQUENCY, 0, 0, 100LL << 16);
    CHECK(tx.freq == 6553600 && recounts == r + 1);
    advance(10000);
    gain = real_now() - base - 10 * NS;
    CHECK(gain >= 999999 && gain <= 1000000);
    base = set_clock(1700000400);
    advance(1000);
    gain = real_now() - base - 1 * NS;
    CHECK(gain >= 99999 && gain <= 100000);
    adj(&tx, 0, 0, 0, 0);
    CHECK(tx.freq == 6553600);
    adj(&tx, ADJ_FREQUENCY, 0, 0, 0);
}

/** adjtime's 1.2 ms is slewed at 500 ppm from the second after, 500 us,
 * 500 us, then 200 us, and what is left reads back each second, while the
 * PLL's offset reads 0 and nothing cancels. */
static void singleshot_row(void)
{
    struct ktimex tx;
    int64_t base, gain;
    int c;

    base = set_clock(1700000500);
    c = cancels;
    adj(&tx, ADJ_OFFSET_SINGLESHOT, 0, 1200, 0);
    CHECK(tx.offset == 0);
    adj(&tx, ADJ_OFFSET_SS_READ, 0, 0, 0);
    CHECK(tx.offset == 1200);
    advance(1000);
    CHECK(real_now() - base == 1 * NS);
    adj(&tx, ADJ_OFFSET_SS_READ, 0, 0, 0);
    CHECK(tx.offset == 700);
    advance(1000);
    gain = real_now() - base - 2 * NS;
    CHECK(gain >= 499999 && gain <= 500000);
    adj(&tx, ADJ_OFFSET_SS_READ, 0, 0, 0);
    CHECK(tx.offset == 200);
    advance(1000);
    gain = real_now() - base - 3 * NS;
    CHECK(gain >= 999999 && gain <= 1000000);
    adj(&tx, ADJ_OFFSET_SS_READ, 0, 0, 0);
    CHECK(tx.offset == 0);
    advance(2000);
    gain = real_now() - base - 5 * NS;
    CHECK(gain >= 1199999 && gain <= 1200000);
    adj(&tx, 0, 0, 0, 0);
    CHECK(tx.offset == 0 && cancels == c);
}

/** ADJ_SETOFFSET's shift steps real time at once, forgets the PLL's offset,
 * which slews no more, and cancels; a shift before the monotonic clock is
 * refused, and still forgets and cancels, as Linux's does. */
static void setoffset_row(void)
{
    struct ktimex tx;
    int64_t before, base;
    int c;

    set_clock(1700000600);
    pll_restart();
    adj(&tx, ADJ_OFFSET, 0, 500000, 0);
    advance(1000);
    c = cancels;
    before = real_now();
    CHECK(rtc_shift_realtime(5, 0) == 0);
    CHECK(real_now() - before == 5 * NS && cancels == c + 1);
    base = last_real = real_now();
    advance(1000);
    CHECK(real_now() - base == 1 * NS);
    adj(&tx, 0, 0, 0, 0);
    CHECK(tx.offset == 0 && (tx.status & STA_UNSYNC));
    before = real_now();
    CHECK(rtc_shift_realtime(-1800000000, 0) == -1);
    CHECK(real_now() == before && cancels == c + 2);
}

int main(void)
{
    mono_ns = 100 * NS;
    offset_rows();
    pll_frequency_row();
    frequency_row();
    singleshot_row();
    setoffset_row();
    if (fails != 0) {
        printf("ntp_hosttest: %d failure(s)\n", fails);
    } else {
        printf("ntp_hosttest: ok\n");
    }
    return fails > 99 ? 99 : fails;
}
