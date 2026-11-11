/**
 * Host test for the CMOS clock arithmetic and PIT interpolation.
 *
 * host-deps: rach2/sys/arch/x86/cmos.c rach2/sys/clock.c
 * host-deps: rach2/sys/arch/x86/pit.c tests/host/compat/spinlock_host.c
 * host-deps: rach2/sys/ntp.c rach2/sys/tick.c
 * host-cflags: -DRTC_HOST_TEST -DRACH2_HOST_CPU
 *
 * `rtc_decode`, `rtc_snapshots_agree` and `rtc_time_from_anchor` are the whole
 * of the clock's arithmetic and they touch no hardware, so they run natively
 * here over crafted samples: the two encodings, the 12-hour PM flag, a leap
 * year, the fields a dead battery reports, the two reads a boot compares before
 * it uses either, and the anchor a real time is measured from. The
 * expected seconds are literal, computed by hand, not by the code under
 * test, because a case that derives its answer the way the implementation does
 * proves only that the implementation is self-consistent.
 *
 * `rtc_time_of_seconds`, the way back, is checked over the same kind of
 * hand-worked instants.
 *
 * Port stubs also drive the real-time reader across pending interrupts and
 * counter wraps, including one during its latch, without executing I/O; and a
 * reference counter the test keeps drives the ticks each interrupt counts and
 * the clocks it keeps, on the boot processor and on another.
 *
 * The answers are for a tick of 1000 Hz: a reload of 1193, sweeps of 499 us
 * and a reference tick of 1000 counts of the 1 MHz counter.
 *
 * Returns the failed-check count (0 == all passed), capped so it cannot wrap
 * to a false 0.
 */

#include "rtc.h"
#include "pit.h"
#include "cmos.h"
#include "cmdline.h"
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

volatile uint64_t kernel_ticks;
volatile uint32_t kernel_uptime_seconds;
volatile uint32_t kernel_second_ticks;
/* The PIT: its count and the status byte of the latch, the byte of the latch
 * the next read of port 0x40 returns, whether its interrupt is pending, the
 * pending checks after which the counter wraps (0 for none), and the
 * modelled interrupt flag. */
static uint32_t count;
static uint8_t status;
static int port_byte;
static int pending;
static int wrap_on_pending;
static int irq_disabled;
/* The CMOS: its registers and the index the address port last chose. */
static uint8_t cmos[128];
static uint8_t cmos_index;

/** Model one atomic status/count latch, not two unrelated hardware reads, and
 * the CMOS's index and data ports, which are used with interrupts off. */
void outb(uint16_t port, uint8_t value)
{
    CHECK(irq_disabled);
    if (port == 0x70) {
        cmos_index = (uint8_t)(value & 0x7F);
        return;
    }
    if (port == 0x71) {
        cmos[cmos_index] = value;
        return;
    }
    CHECK(port == 0x43 && value == 0xC2);
    port_byte = 0;
}

/** Read the CMOS's data port, or the PIT latch: the status, then the
 * count's low and high bytes. Either needs interrupts off. */
uint8_t inb(uint16_t port)
{
    if (port == 0x71) {
        CHECK(irq_disabled);
        return cmos[cmos_index];
    }
    CHECK(irq_disabled && port == 0x40);
    if (port_byte++ == 0) {
        return status;
    }
    return port_byte == 2 ? (uint8_t)count : (uint8_t)(count >> 8);
}

/** Mask interrupts and return the mask the caller had. */
uint32_t kirq_save(void)
{
    uint32_t old = (uint32_t)irq_disabled;

    irq_disabled = 1;
    return old;
}

/** Restore the mask `flags`. */
void kirq_restore(uint32_t flags)
{
    irq_disabled = (int)flags;
}

/** Force the counter wrap between the latch and the second PIC read. */
int pic_irq_pending(uint8_t irq)
{
    CHECK(irq == 0 && irq_disabled);
    if (wrap_on_pending && --wrap_on_pending == 0) {
        pending = 1;
        count = pit_reload - 100u;
        status = 0x80;
    }
    return pending;
}

/** No row attaches the alarm's interrupt. */
void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
    CHECK(0);
}

/** No row unmasks a line; a call fails the test. */
void pic_unmask_irq(uint8_t irq)
{
    (void)irq;
    CHECK(0);
}

/** Discard the log. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/* The processor the host runs as (arch/i686/cpu.h): the boot processor,
 * which reads the counter under its tick, but in the row of another
 * processor's clock; and the reference counter of 1 MHz at `ref_now`, which
 * only that row turns on. No row waits on the tick's sequence count. */
unsigned int host_cpu;
static int ref_on;
static uint32_t ref_now;

/** The reference counter's name, NULL while it is off. */
const char *refclock_name(void)
{
    return ref_on ? "test" : 0;
}

/** The reference counter: `ref_now`. */
uint32_t refclock_read(void)
{
    CHECK(ref_on);
    return ref_now;
}

/** The reference counter's rate, 1 MHz. */
uint32_t refclock_hz(void)
{
    CHECK(ref_on);
    return 1000000u;
}

/** The reference counter's width, 32 bits. */
uint32_t refclock_mask(void)
{
    CHECK(ref_on);
    return 0xFFFFFFFFu;
}

/** `a` * `b` / `c` in 64 bits. */
uint32_t refclock_scale(uint32_t a, uint32_t b, uint32_t c)
{
    return (uint32_t)((uint64_t)a * b / c);
}

/** No row waits for another processor; a call fails the test. */
void smp_service(void)
{
    CHECK(0);
}

/** No row spins; a call fails the test. */
void arch_relax(void)
{
    CHECK(0);
}

/** The clock does not run from the TSC here, so clock.c keeps the time. */
int vclock_read(int which, int coarse, int64_t *sec, int64_t *nsec)
{
    (void)which;
    (void)coarse;
    (void)sec;
    (void)nsec;
    return 0;
}

/** Nor, so, is it the clock source pit.c names. */
int vclock_running(void)
{
    return 0;
}

/** No vDSO page takes the offset here. */
void vclock_set_wall(int64_t sec, int32_t nsec)
{
    (void)sec;
    (void)nsec;
}

/** No timer here waits on real time. */
void timerfd_clock_was_set(void)
{}

/** The command line here is empty. */
int kcmdline_has(const char *flag)
{
    (void)flag;
    return 0;
}

/* No row measures the time-stamp counter, which timercal_hosttest.c does;
 * a call of these fails the test. */
void kcpu_id(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    (void)leaf;
    *a = *b = *c = *d = 0;
    CHECK(0);
}

uint32_t pit_wait_counts(uint32_t counts)
{
    (void)counts;
    CHECK(0);
    return 0;
}

/* The wait pit.c gives `k_udelay` once the TSC keeps the clock, which no row
 * reaches. */
void (*udelay_wait)(uint32_t usec);

void host_tsc_read(uint32_t *lo, uint32_t *hi)
{
    *lo = *hi = 0;
    CHECK(0);
}

/** No row asks whether the processors' counters agree: a call fails the
 * test. */
int tsc_synced(int *cpu, uint64_t *behind)
{
    *cpu = 0;
    *behind = 0;
    CHECK(0);
    return 1;
}

/**
 * Literal answers exercise the same latch and real-time path as file writes.
 * A pending wrap must count once, including at the end of a second; dropping
 * the PIT fraction must fail even when the delivered tick has not changed.
 */
static void subtick_rows(void)
{
    int64_t sec, ns;

    kernel_uptime_seconds = 10;
    kernel_second_ticks = 0;
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
    CHECK(sec == 10 && ns == 998000);
    pending = 1;
    status = 0x80;
    count = pit_reload - 100u;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 10 && ns == 1041000);
    pending = 0;
    kernel_second_ticks = 1;
    count -= 100u;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 10 && ns == 1083000);
    kernel_second_ticks = 999;
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
    CHECK(sec == 11 && ns == 1998000);
    count = pit_reload - 100u;
    status = 0x80;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 11 && ns == 1998000);
    pending = 0;
    kernel_second_ticks = 2;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 11 && ns == 2041000);
    CHECK(!irq_disabled);
    kernel_second_ticks = 0;
    kernel_uptime_seconds = 42949672u;
    count = pit_reload - 100u;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 42949672LL && ns == 41000);
}

/**
 * Setting the clock from a time the anchor keeps of 20.000041 s, with no
 * clock read at boot: the reading is the time set, at the same hardware
 * state, and goes on from it at the ticks' rate; the coarse clock leaves out
 * the PIT's 41 us; a step back reads back too, though it is earlier than the
 * reading before it; and the boot epoch moves with the step.
 */
static void set_rows(void)
{
    int64_t sec, ns;

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
    CHECK(sec == 1790000000LL && ns == 501000000);
    rtc_coarse_realtime(&sec, &ns);
    CHECK(sec == 1790000000LL && ns == 500959000);
    CHECK(rtc_set_realtime(1000, 0) == 0);
    rtc_realtime(&sec, &ns);
    CHECK(sec == 1000 && ns == 0);
    CHECK(rtc_boot_epoch() == 979);
}

/**
 * Time since boot at the same hardware states as the real-time rows: the
 * PIT's place within the tick is added, a pending wrap counts once, also at
 * the end of a second, a reading is never earlier than the one before, and
 * setting the real time does not move it.
 */
static void monotonic_rows(void)
{
    int64_t sec, ns;

    wrap_on_pending = 0;
    pending = 0;
    kernel_uptime_seconds = 50;
    kernel_second_ticks = 0;
    count = pit_reload - 100u;
    status = 0x80;
    rtc_monotonic(&sec, &ns);
    CHECK(sec == 50 && ns == 41000);
    count -= 100u;
    rtc_monotonic(&sec, &ns);
    CHECK(sec == 50 && ns == 83000);
    pending = 1;
    count = pit_reload - 100u;
    rtc_monotonic(&sec, &ns);
    CHECK(sec == 50 && ns == 1041000);
    pending = 0;
    kernel_second_ticks = 999;
    status = 0;
    count = 2;
    wrap_on_pending = 2;
    rtc_monotonic(&sec, &ns);
    CHECK(sec == 51 && ns == 41000);
    pending = 0;
    kernel_uptime_seconds = 50;
    kernel_second_ticks = 999;
    status = 0x80;
    count = pit_reload - 100u;
    rtc_monotonic(&sec, &ns);
    CHECK(sec == 51 && ns == 41000);
    kernel_uptime_seconds = 51;
    kernel_second_ticks = 1;
    CHECK(rtc_set_realtime(1000, 0) == 0);
    rtc_monotonic(&sec, &ns);
    CHECK(sec == 51 && ns == 1041000);
    CHECK(!irq_disabled);
}

/**
 * The clock once the test's reference counter, 1 MHz and 32 bits, keeps it,
 * from a tick at 41 us into the PIT's period with the counter 4 Ki counts
 * short of its wrap: the microsecond clock goes on from the PIT's reading; an
 * interrupt on time counts one tick, the same one delivered again none, and
 * one seven and a half ticks late, past the counter's wrap, seven. Another
 * processor then three ticks and 10 us on reads three ticks pending and its
 * place, as the microsecond clock and the time since boot, to a second's
 * turn; the boot processor reads no earlier; and an interrupt seventy
 * seconds late counts a minute's ticks, the next the rest.
 */
static void reference_rows(void)
{
    const uint32_t start = 0xFFFFF000u;
    uint32_t base, phase_ns;
    int64_t sec, ns;
    int due;

    ref_on = 1;
    ref_now = start;
    pending = 0;
    wrap_on_pending = 0;
    status = 0x80;
    count = pit_reload - 100u;
    CHECK(timer_tick_clock() == 1u);
    base = timer_now_us();
    CHECK(base == pit_period_us() + 41u);
    ref_now = start + 1000u;
    CHECK(timer_tick_clock() == 1u);
    ref_now = start + 1010u;
    CHECK(timer_tick_clock() == 0u);
    ref_now = start + 8010u;
    CHECK(timer_tick_clock() == 7u);
    CHECK(timer_now_us() == base + 8010u);
    host_cpu = 1;
    ref_now = start + 10510u;
    timer_phase(&due, &phase_ns);
    CHECK(due == 3 && phase_ns == 10000u);
    CHECK(timer_now_us() == base + 10510u);
    kernel_uptime_seconds = 60;
    kernel_second_ticks = 998;
    rtc_monotonic(&sec, &ns);
    CHECK(sec == 61 && ns == 1010000);
    host_cpu = 0;
    CHECK(timer_now_us() == base + 10510u);
    ref_now += 70000000u;
    CHECK(timer_tick_clock() == 60000u);
    CHECK(timer_tick_clock() == 10003u);
    CHECK(!irq_disabled);
    ref_on = 0;
}

/** A BCD byte from a two-digit value, the way the CMOS has one. */
static unsigned char bcd(int value)
{
    return (unsigned char)(((value / 10) << 4) | (value % 10));
}

/**
 * The CMOS clock written and read back: in BCD with a 12-hour clock, as some
 * firmware keeps it, 23:05:09 on 29 February 2028 is held as 11 PM, a
 * Tuesday, with updates let go after the write; read back, it is the same
 * time; and a year the two digits cannot hold is rejected, the clock left
 * untouched.
 */
static void clock_write_rows(void)
{
    rtc_time_t t = {2028, 2, 29, 23, 5, 9}, back = {0, 0, 0, 0, 0, 0};
    rtc_time_t late = {2100, 1, 1, 0, 0, 0};
    int32_t weekday, yearday;

    cmos[0x0A] = RTC_STATUS_A_DIVIDER_NORMAL;
    cmos[0x0B] = 0;
    cmos[0x0D] = RTC_STATUS_D_VALID;
    CHECK(rtc_write_clock(&t) == 0);
    CHECK(cmos[0x04] == (0x11 | RTC_HOUR_PM) && cmos[0x00] == 0x09 && cmos[0x02] == 0x05);
    CHECK(cmos[0x07] == 0x29 && cmos[0x08] == 0x02 && cmos[0x09] == 0x28 && cmos[0x06] == 3);
    CHECK(cmos[0x0B] == 0);
    CHECK(rtc_read_clock(&back) == 0 && back.year == 2028 && back.month == 2 && back.day == 29 &&
          back.hour == 23 && back.minute == 5 && back.second == 9);
    rtc_week_and_year_day(&t, &weekday, &yearday);
    CHECK(weekday == 2 && yearday == 59);
    CHECK(rtc_write_clock(&late) == -1 && cmos[0x09] == 0x28);
}

/** Whether `a` and `b` are the same date and time, the year to the second. */
static int same_time(const rtc_time_t *a, const rtc_time_t *b)
{
    return a->year == b->year && a->month == b->month && a->day == b->day && a->hour == b->hour &&
           a->minute == b->minute && a->second == b->second;
}

/**
 * Check `rtc_time_of_seconds` and `rtc_seconds_of` on dates and times worked
 * out by hand, from 1970 to the calendar's last second, with the leap years
 * before a year y counted as y/4 - y/100 + y/400 of y - 1, which is not how
 * clock.c counts them.
 *
 * 2000-02-29 is 10957 days from 1970 and 59 into 2000, 951782400 seconds.
 * 2400-02-29 is 10957 days to 2000, a cycle of 146097 to 2400 and 59 into
 * the year: day 157113, 13574563200 seconds, and a Tuesday, as 2000-02-29
 * was, since a cycle is 20871 weeks. 2371-03-01 12:34:56 lies past 2370,
 * where the cycles clock.c counts from 1970 turn: 146097 days to 2370, 365
 * in 2370 and 59 into 2371, day 146521, and 12659459696 seconds, a Monday.
 * The last second, 2147483647-12-31 23:59:59, is 365 days for each of the
 * 2147481678 years from 1970 and one for each of their 520764307 leap years,
 * 784351576777 days, less a second: 67767976233532799. One second more is a
 * year past INT_MAX, which `rtc_time_of_seconds` refuses with -1, leaving
 * `*t` as it was.
 */
static void calendar_rows(void)
{
    static const long long seconds[] = {
        0LL,           951782400LL,   1790000000LL,       4102444799LL,
        12659459696LL, 13574563200LL, 67767976233532799LL};
    static const rtc_time_t times[] = {{1970, 1, 1, 0, 0, 0},
                                       {2000, 2, 29, 0, 0, 0},
                                       {2026, 9, 21, 14, 13, 20},
                                       {2099, 12, 31, 23, 59, 59},
                                       {2371, 3, 1, 12, 34, 56},
                                       {2400, 2, 29, 0, 0, 0},
                                       {2147483647L, 12, 31, 23, 59, 59}};
    rtc_time_t back, kept = {2026, 11, 11, 1, 2, 3};
    int32_t weekday, yearday;
    unsigned i;

    for (i = 0; i < sizeof seconds / sizeof seconds[0]; i++) {
        CHECK(rtc_time_of_seconds(seconds[i], &back) == 0 && same_time(&back, &times[i]));
        CHECK(rtc_seconds_of(&times[i]) == seconds[i]);
    }
    rtc_week_and_year_day(&times[4], &weekday, &yearday);
    CHECK(weekday == 1 && yearday == 59);
    rtc_week_and_year_day(&times[5], &weekday, &yearday);
    CHECK(weekday == 2 && yearday == 59);
    back = kept;
    CHECK(rtc_time_of_seconds(67767976233532800LL, &back) == -1 && same_time(&back, &kept));
}

/** Fill a BCD, 24-hour sample of a running clock with a good battery; `year`
 * is the two digits the clock keeps. */
static void bcd_sample(rtc_snapshot_t *raw, int year, int month, int day, int hour, int minute,
                       int second)
{
    raw->year = bcd(year);
    raw->month = bcd(month);
    raw->day = bcd(day);
    raw->hour = bcd(hour);
    raw->minute = bcd(minute);
    raw->second = bcd(second);
    raw->status_a = RTC_STATUS_A_DIVIDER_NORMAL;
    raw->status_b = RTC_STATUS_B_24_HOUR;
    raw->status_d = RTC_STATUS_D_VALID;
}

/**
 * Check `rtc_time_from_anchor`, which measures real time from the clock's
 * reading at boot.
 *
 * A clock read at tick 500 saying 1000, read again at tick 700, is 1002: two
 * seconds later, not seven. Adding every tick since boot would give 1007
 * here, and on a booted machine that error is under a second, which is why
 * it is stated as arithmetic, not left to a tolerance.
 *
 * Part of a second survives as nanoseconds: 550 ticks is 550 ms. No anchor
 * at all is time since boot, which is what a machine with no readable clock
 * reports. An uptime of 42949672 seconds, past 2^32 ticks, adds whole. And
 * the seconds counter's own wrap, 136 years out, still reads as elapsed
 * time, not as a jump back.
 */
static void anchor_rows(void)
{
    long long sec = 0;
    long long nsec = 0;

    rtc_time_from_anchor(1000LL, 5u, 7u, 0u, &sec, &nsec);
    CHECK(sec == 1002LL);
    CHECK(nsec == 0LL);

    rtc_time_from_anchor(1000LL, 5u, 7u, 550u, &sec, &nsec);
    CHECK(sec == 1002LL);
    CHECK(nsec == 550000000LL);

    rtc_time_from_anchor(0LL, 0u, 2u, 500u, &sec, &nsec);
    CHECK(sec == 2LL);
    CHECK(nsec == 500000000LL);

    rtc_time_from_anchor(1000LL, 0u, 42949672u, 0u, &sec, &nsec);
    CHECK(sec == 1000LL + 42949672LL);

    rtc_time_from_anchor(1000LL, 0xFFFFFFF0u, 0x00000009u, 0u, &sec, &nsec);
    CHECK(sec == 1000LL + 25LL);
}

/**
 * Run the rows. The exit status is the count of failed checks, at most 100.
 *
 * Seconds go back to the calendar the clock keeps, for the 11-minute copy:
 * the epoch, a leap day, a time of the century and its last second, each
 * worked out by hand. `timer_clock_check` runs the boot's self-check of the
 * clock's own fixed readings.
 *
 * The decode: the epoch of the century the driver assumes, 2000-01-01, is
 * 10957 days after 1970-01-01, and 10957 * 86400 is 946684800; the binary
 * encoding of the same instant reads the same. A leap day, 2024-02-29
 * 12:00:00 UTC, is 1709208000; a wrong leap rule either rejects the date or
 * lands a day out. 2100 is not a leap year and 2000 was, and since the
 * driver assumes the 2000s, 29 February of a year ending in 00 is reachable
 * only as 2000, which must be accepted. In the twelve-hour encoding 1 PM is
 * 13:00: the PM bit is the hour's high bit and must come off before the BCD
 * decode; 2026-08-14 13:05:07 UTC is 1786712707. 12 AM is hour 0 and 12 PM
 * is hour 12, which a plain "add twelve for PM" gets wrong both ways.
 *
 * Fields a clock with no battery reports are rejected, not corrected: a
 * clock that reads month 0 has no date to correct to. A nibble that is not
 * a digit, 0x1A as an unmaintained byte has, is not a BCD number. A
 * twelve-hour sample whose hour is 0 or 13 is not a twelve-hour time. A
 * NULL calendar is allowed, and the seconds still arrive. The status
 * registers are judged too: a sample whose date fields are all in range is
 * how a machine with a dead battery reports, stable, plausible and wrong.
 *
 * Two samples of one clock are what the boot read takes before it decodes
 * either. The registers advance while they are read, so a disagreement,
 * such as the turn of a minute, is a read to discard, and each status
 * register is part of the comparison: the decode judges the clock by them,
 * so a divider that was running in the first read and is not in the second
 * must not be accepted on the strength of the first. The update-in-progress
 * flag is left out: the hardware moves it once a second, so counting it
 * would make every second read disagree with itself. A battery that went
 * ends the read instead of earning another try: on some parts reading
 * register D sets the validity bit again, so a dead cell gives a pair that
 * disagrees (clear, then set) and then a pair that agrees, is in range, and
 * is whatever the dead cell left. The first sample is the one usable one,
 * and the driver stops on it.
 *
 * `rtc_pair_disposition` is the policy for a pair: a healthy pair is used,
 * a straddled update is read again, and a clock that says its battery went
 * ends the read in either order. A pair that both disagrees and says the
 * contents are lost must stop rather than be read again, which the
 * comparison alone does not say. The pair after it, which agrees and is in
 * range, is never taken, since the read has stopped.
 */
int main(void)
{
    rtc_snapshot_t raw;
    rtc_time_t when;
    long long seconds;

    calendar_rows();

    CHECK(timer_clock_check());

    bcd_sample(&raw, 0, 1, 1, 0, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);
    CHECK(seconds == 946684800LL);
    CHECK(when.year == 2000 && when.month == 1 && when.day == 1);

    raw.year = 0;
    raw.month = 1;
    raw.day = 1;
    raw.hour = 0;
    raw.minute = 0;
    raw.second = 0;
    raw.status_a = RTC_STATUS_A_DIVIDER_NORMAL;
    raw.status_b = RTC_STATUS_B_24_HOUR | RTC_STATUS_B_BINARY;
    raw.status_d = RTC_STATUS_D_VALID;
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);
    CHECK(seconds == 946684800LL);

    bcd_sample(&raw, 24, 2, 29, 12, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);
    CHECK(seconds == 1709208000LL);
    CHECK(when.day == 29 && when.hour == 12);

    bcd_sample(&raw, 0, 2, 29, 0, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);

    bcd_sample(&raw, 26, 8, 14, 1, 5, 7);
    raw.hour = (unsigned char)(bcd(1) | RTC_HOUR_PM);
    raw.status_b = 0; /* 12-hour, BCD */
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);
    CHECK(when.hour == 13);
    CHECK(seconds == 1786712707LL);

    bcd_sample(&raw, 26, 8, 14, 12, 0, 0);
    raw.status_b = 0; /* 12-hour, BCD */
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);
    CHECK(when.hour == 0);
    bcd_sample(&raw, 26, 8, 14, 12, 0, 0);
    raw.hour = (unsigned char)(bcd(12) | RTC_HOUR_PM);
    raw.status_b = 0; /* 12-hour, BCD */
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);
    CHECK(when.hour == 12);

    bcd_sample(&raw, 26, 0, 14, 0, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 13, 1, 0, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 2, 30, 0, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 8, 0, 0, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 8, 14, 24, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 8, 14, 0, 60, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);

    bcd_sample(&raw, 26, 8, 14, 0, 0, 0);
    raw.minute = 0x1A;
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);

    bcd_sample(&raw, 26, 8, 14, 0, 0, 0);
    raw.status_b = 0; /* 12-hour, BCD */
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 8, 14, 13, 0, 0);
    raw.status_b = 0; /* 12-hour, BCD */
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);

    bcd_sample(&raw, 0, 1, 1, 0, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, (rtc_time_t *)0) == 0);
    CHECK(seconds == 946684800LL);

    bcd_sample(&raw, 26, 8, 14, 12, 0, 0);
    raw.status_d = 0; /* VRT clear: the battery went */
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 8, 14, 12, 0, 0);
    raw.status_b |= RTC_STATUS_B_SET; /* updates inhibited: the clock is frozen */
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 8, 14, 12, 0, 0);
    raw.status_b |= RTC_STATUS_B_DSE; /* the clock shifts itself twice a year */
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 8, 14, 12, 0, 0);
    raw.status_a = 0; /* the divider is not the running one */
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);

    anchor_rows();

    {
        rtc_snapshot_t a;
        rtc_snapshot_t b;

        bcd_sample(&a, 26, 8, 14, 9, 31, 7);
        b = a;
        CHECK(rtc_snapshots_agree(&a, &b) == 1);

        b.second = bcd(8);
        CHECK(rtc_snapshots_agree(&a, &b) == 0);

        b = a;
        b.status_a = 0;
        CHECK(rtc_snapshots_agree(&a, &b) == 0);
        b = a;
        b.status_b = (unsigned char)(a.status_b | RTC_STATUS_B_SET);
        CHECK(rtc_snapshots_agree(&a, &b) == 0);
        b = a;
        b.status_d = 0;
        CHECK(rtc_snapshots_agree(&a, &b) == 0);

        b = a;
        b.status_a = (unsigned char)(a.status_a | RTC_STATUS_A_UPDATING);
        CHECK(rtc_snapshots_agree(&a, &b) == 1);

        CHECK(rtc_snapshots_agree(&a, 0) == 0);
        CHECK(rtc_snapshots_agree(0, &b) == 0);

        b = a;
        b.status_d = 0;
        CHECK(rtc_contents_lost(&b) == 1);
        CHECK(rtc_snapshots_agree(&b, &a) == 0);
        CHECK(rtc_contents_lost(&a) == 0);
        CHECK(rtc_contents_lost(0) == 1);
    }

    {
        rtc_snapshot_t a;
        rtc_snapshot_t b;

        bcd_sample(&a, 26, 8, 14, 9, 31, 7);
        b = a;
        CHECK(rtc_pair_disposition(&a, &b) == RTC_PAIR_ACCEPT);

        b.status_a = (unsigned char)(a.status_a | RTC_STATUS_A_UPDATING);
        CHECK(rtc_pair_disposition(&a, &b) == RTC_PAIR_ACCEPT);

        b = a;
        b.second = bcd(8);
        CHECK(rtc_pair_disposition(&a, &b) == RTC_PAIR_RETRY);

        b = a;
        b.status_d = 0;
        CHECK(rtc_pair_disposition(&b, &a) == RTC_PAIR_STOP);
        CHECK(rtc_pair_disposition(&a, &b) == RTC_PAIR_STOP);

        b.status_d = 0;
        CHECK(rtc_pair_disposition(&b, &b) == RTC_PAIR_STOP);
    }

    subtick_rows();
    set_rows();
    monotonic_rows();
    clock_write_rows();
    reference_rows();
    return fails > 100 ? 100 : fails;
}
