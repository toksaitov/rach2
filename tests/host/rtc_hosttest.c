/**
 * Host test for the CMOS clock arithmetic and PIT interpolation.
 *
 * host-deps: rach2/sys/arch/x86/cmos.c rach2/sys/clock.c
 * host-deps: rach2/sys/arch/x86/pit.c tests/host/compat/spinlock_host.c
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
 * counter wraps, including one during its latch, without executing I/O, and
 * the microsecond clock on the boot processor and on another, which reads a
 * reference counter the test keeps.
 *
 * Returns the failed-check count (0 == all passed), capped so it cannot wrap
 * to a false 0.
 */

#include "rtc.h"
#include "pit.h"
#include "cmos.h"

static int fails;

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

volatile uint32_t kernel_ticks;
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
    CHECK(sec == 10 && ns == 9997000);
    pending = 1;
    status = 0x80;
    count = pit_reload - 100u;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 10 && ns == 10041000);
    pending = 0;
    kernel_second_ticks = 1;
    count -= 100u;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 10 && ns == 10083000);
    kernel_second_ticks = 99;
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
    CHECK(sec == 11 && ns == 19997000);
    count = pit_reload - 100u;
    status = 0x80;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 11 && ns == 19997000);
    pending = 0;
    kernel_second_ticks = 2;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 11 && ns == 20041000);
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
    rtc_set_realtime(1790000000LL, 500000000);
    rtc_realtime(&sec, &ns);
    CHECK(sec == 1790000000LL && ns == 500000000);
    kernel_second_ticks = 1;
    rtc_realtime(&sec, &ns);
    CHECK(sec == 1790000000LL && ns == 510000000);
    rtc_coarse_realtime(&sec, &ns);
    CHECK(sec == 1790000000LL && ns == 509959000);
    rtc_set_realtime(1000, 0);
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
    CHECK(sec == 50 && ns == 10041000);
    pending = 0;
    kernel_second_ticks = 99;
    status = 0;
    count = 2;
    wrap_on_pending = 2;
    rtc_monotonic(&sec, &ns);
    CHECK(sec == 51 && ns == 41000);
    pending = 0;
    kernel_uptime_seconds = 50;
    kernel_second_ticks = 99;
    status = 0x80;
    count = pit_reload - 100u;
    rtc_monotonic(&sec, &ns);
    CHECK(sec == 51 && ns == 41000);
    kernel_uptime_seconds = 51;
    kernel_second_ticks = 1;
    rtc_set_realtime(1000, 0);
    rtc_monotonic(&sec, &ns);
    CHECK(sec == 51 && ns == 10041000);
    CHECK(!irq_disabled);
}

/**
 * The microsecond clock on two processors around a tick of the boot
 * processor's at 41 us into its period. Another processor two and a half
 * periods on reads a microsecond short of the period after the tick, since
 * the boot processor has counted no period since; the boot processor, which
 * then reads half a period and 41 us into that period by its counter, gives
 * the other's reading again, never an earlier one; and once its counter has
 * wrapped it reads 41 us into the next period.
 */
static void cross_cpu_rows(void)
{
    uint32_t period = pit_period_us();
    uint32_t base, other, late, wrapped;

    ref_on = 1;
    ref_now = 5000000u;
    pending = 0;
    wrap_on_pending = 0;
    status = 0x80;
    count = pit_reload - 100u;
    timer_tick_clock();
    base = timer_now_us() - 41u;
    host_cpu = 1;
    ref_now += 5u * period / 2u;
    other = timer_now_us();
    host_cpu = 0;
    status = 0;
    late = timer_now_us();
    status = 0x80;
    count = pit_reload - 100u;
    wrapped = timer_now_us();
    CHECK(other == base + period - 1u);
    CHECK(late == other);
    CHECK(wrapped == base + period + 41u);
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
 * Part of a second survives as nanoseconds: 55 ticks is 550 ms. No anchor
 * at all is time since boot, which is what a machine with no readable clock
 * reports. An uptime of a whole tick cycle is why the anchor counts
 * seconds: 2^32 ticks after the anchor a difference of ticks is zero again,
 * and real time would jump back 497 days. And the seconds counter's own
 * wrap, 136 years out, still reads as elapsed time, not as a jump back.
 */
static void anchor_rows(void)
{
    long long sec = 0;
    long long nsec = 0;

    rtc_time_from_anchor(1000LL, 5u, 7u, 0u, &sec, &nsec);
    CHECK(sec == 1002LL);
    CHECK(nsec == 0LL);

    rtc_time_from_anchor(1000LL, 5u, 7u, 55u, &sec, &nsec);
    CHECK(sec == 1002LL);
    CHECK(nsec == 550000000LL);

    rtc_time_from_anchor(0LL, 0u, 2u, 50u, &sec, &nsec);
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

    rtc_time_of_seconds(0, &when);
    CHECK(when.year == 1970 && when.month == 1 && when.day == 1 && when.hour == 0 &&
          when.minute == 0 && when.second == 0);
    rtc_time_of_seconds(951782400u, &when);
    CHECK(when.year == 2000 && when.month == 2 && when.day == 29 && when.hour == 0);
    rtc_time_of_seconds(1790000000u, &when);
    CHECK(when.year == 2026 && when.month == 9 && when.day == 21 && when.hour == 14 &&
          when.minute == 13 && when.second == 20);
    rtc_time_of_seconds(4102444799u, &when);
    CHECK(when.year == 2099 && when.month == 12 && when.day == 31 && when.hour == 23 &&
          when.minute == 59 && when.second == 59);

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
    cross_cpu_rows();
    return fails > 100 ? 100 : fails;
}
