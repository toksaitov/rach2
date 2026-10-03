/**
 * Host test for the CMOS clock arithmetic and PIT interpolation.
 *
 * host-deps: rach2/sys/arch/x86/cmos.c rach2/sys/clock.c rach2/sys/arch/x86/pit.c
 * host-cflags: -DRTC_HOST_TEST
 *
 * `rtc_decode`, `rtc_snapshots_agree` and `rtc_time_from_anchor` are the whole
 * of the clock's arithmetic and they touch no hardware, so they run natively
 * here over crafted samples: the two encodings, the 12-hour PM flag, a leap
 * year, the fields a dead battery reports, the two reads a boot compares before
 * it believes either, and the anchor a real time is measured from. The
 * expected seconds are literal, computed by hand, not by the code under
 * test, because a case that derives its answer the way the implementation does
 * proves only that the implementation is self-consistent.
 *
 * `rtc_time_of_seconds`, the way back, is checked over the same kind of
 * hand-worked instants.
 *
 * Port stubs also drive the real-time reader across pending interrupts and
 * counter wraps, including one during its latch, without executing I/O.
 *
 * Returns the failed-check count (0 == all passed), capped so it cannot wrap
 * to a false 0.
 */

#include "rtc.h"
#include "pit.h"

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

uint32_t kirq_save(void)
{
    uint32_t old = (uint32_t)irq_disabled;

    irq_disabled = 1;
    return old;
}

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

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
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

/*
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

/*
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

/** A BCD byte from a two-digit value, the way the CMOS has one. */
static unsigned char bcd(int value)
{
    return (unsigned char)(((value / 10) << 4) | (value % 10));
}

/** Fill a BCD, 24-hour sample. */
/**
 * The CMOS clock written and read back: in BCD with a 12-hour clock, as some
 * firmware keeps it, 23:05:09 on 29 February 2028 is held as 11 PM, a
 * Tuesday, with updates let go after the write; read back, it is the same
 * time; and a year the two digits cannot hold is refused untouched.
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

int main(void)
{
    rtc_snapshot_t raw;
    rtc_time_t when;
    long long seconds;

    /* Seconds back to the calendar the clock keeps, for the 11-minute
     * copy: the epoch, a leap day, a time of the century, and its last
     * second, each worked out by hand. */
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

    /* The epoch itself, in the century this driver assumes: 2000-01-01 is
     * 10957 days after 1970-01-01, and 10957 * 86400 is 946684800. */
    bcd_sample(&raw, 0, 1, 1, 0, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);
    CHECK(seconds == 946684800LL);
    CHECK(when.year == 2000 && when.month == 1 && when.day == 1);

    /* Binary encoding of the same instant reads the same. */
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

    /* A leap day: 2024-02-29 12:00:00 UTC is 1709208000. A wrong leap rule
     * either rejects the date or lands a day out. */
    bcd_sample(&raw, 24, 2, 29, 12, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);
    CHECK(seconds == 1709208000LL);
    CHECK(when.day == 29 && when.hour == 12);

    /* 2100 is not a leap year, and 2000 was: the century rules differ. The
     * driver assumes the 2000s, so February 29 of a year ending in 00 is
     * reachable only as 2000, which must be accepted. */
    bcd_sample(&raw, 0, 2, 29, 0, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);

    /* Twelve-hour encoding. 1 PM is 13:00; the PM bit rides in the hour's high
     * bit and must come off before the BCD decode, or the hour reads as a
     * nonsense digit. 2026-08-14 13:05:07 UTC is 1786712707. */
    bcd_sample(&raw, 26, 8, 14, 1, 5, 7);
    raw.hour = (unsigned char)(bcd(1) | RTC_HOUR_PM);
    raw.status_b = 0; /* 12-hour, BCD */
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);
    CHECK(when.hour == 13);
    CHECK(seconds == 1786712707LL);

    /* 12 AM is hour 0 and 12 PM is hour 12, which is the case a plain
     * "add twelve for PM" gets wrong in both directions. */
    bcd_sample(&raw, 26, 8, 14, 12, 0, 0);
    raw.status_b = 0; /* 12-hour, BCD */
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);
    CHECK(when.hour == 0);
    bcd_sample(&raw, 26, 8, 14, 12, 0, 0);
    raw.hour = (unsigned char)(bcd(12) | RTC_HOUR_PM);
    raw.status_b = 0; /* 12-hour, BCD */
    CHECK(rtc_decode(&raw, &seconds, &when) == 0);
    CHECK(when.hour == 12);

    /* Fields a clock with no battery reports. Each must be refused, not
     * corrected: a machine that believes it is month 0 is a machine that would
     * build everything or nothing. */
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

    /* A nibble that is not a digit is not a BCD number. 0x1A is what a byte
     * nobody maintains looks like, and it must fail, not decode. */
    bcd_sample(&raw, 26, 8, 14, 0, 0, 0);
    raw.minute = 0x1A;
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);

    /* A twelve-hour sample whose hour is 0 or 13 is not a twelve-hour time. */
    bcd_sample(&raw, 26, 8, 14, 0, 0, 0);
    raw.status_b = 0; /* 12-hour, BCD */
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);
    bcd_sample(&raw, 26, 8, 14, 13, 0, 0);
    raw.status_b = 0; /* 12-hour, BCD */
    CHECK(rtc_decode(&raw, &seconds, &when) != 0);

    /* A NULL out-struct is allowed; the seconds still arrive. */
    bcd_sample(&raw, 0, 1, 1, 0, 0, 0);
    CHECK(rtc_decode(&raw, &seconds, (rtc_time_t *)0) == 0);
    CHECK(seconds == 946684800LL);

    /* What the clock says about ITSELF. Each of these is a sample whose date
     * fields are perfectly in range, which is exactly how a machine with a dead
     * battery reports: stable, plausible and wrong. */
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

    /* The anchor. A clock read at tick 500 saying 1000, asked again at tick
     * 700, is 1002: two seconds later, not seven. Adding every tick since boot
     * would answer 1007 here, and on a real boot that error is under a second,
     * which is why it is stated as arithmetic, not left to a tolerance.
     */
    {
        long long sec = 0;
        long long nsec = 0;

        rtc_time_from_anchor(1000LL, 5u, 7u, 0u, &sec, &nsec);
        CHECK(sec == 1002LL);
        CHECK(nsec == 0LL);

        /* Part of a second survives as nanoseconds: 55 ticks is 550 ms. */
        rtc_time_from_anchor(1000LL, 5u, 7u, 55u, &sec, &nsec);
        CHECK(sec == 1002LL);
        CHECK(nsec == 550000000LL);

        /* No anchor at all is time since boot, which is what a machine with no
         * readable clock reports and what this system reported before. */
        rtc_time_from_anchor(0LL, 0u, 2u, 50u, &sec, &nsec);
        CHECK(sec == 2LL);
        CHECK(nsec == 500000000LL);

        /* An uptime of a whole tick cycle. This is the case that made the
         * anchor count seconds, not ticks: 2^32 ticks after the anchor,
         * a difference of TICKS is zero again and real time jumps back 497
         * days. Counted in seconds, 42949672 seconds later is 42949672 seconds
         * later. */
        rtc_time_from_anchor(1000LL, 0u, 42949672u, 0u, &sec, &nsec);
        CHECK(sec == 1000LL + 42949672LL);

        /* And the seconds counter's own wrap, 136 years out, still reads as
         * elapsed time, not as a jump backwards. */
        rtc_time_from_anchor(1000LL, 0xFFFFFFF0u, 0x00000009u, 0u, &sec, &nsec);
        CHECK(sec == 1000LL + 25LL);
    }

    /* Two samples of one clock, which is what the boot read takes before it
     * decodes either. The registers advance while they are being read, so a
     * disagreement is a read to discard, and the status registers are part of
     * that: the decode judges the clock by what they say, so a divider that was
     * the running one in the first read and is not in the second must not be
     * accepted on the strength of the first. */
    {
        rtc_snapshot_t a;
        rtc_snapshot_t b;

        bcd_sample(&a, 26, 8, 14, 9, 31, 7);
        b = a;
        CHECK(rtc_snapshots_agree(&a, &b) == 1);

        /* The turn of a minute, which is the disagreement this is here for. */
        b.second = bcd(8);
        CHECK(rtc_snapshots_agree(&a, &b) == 0);

        /* Each status register in turn, since each is a reason to refuse. */
        b = a;
        b.status_a = 0;
        CHECK(rtc_snapshots_agree(&a, &b) == 0);
        b = a;
        b.status_b = (unsigned char)(a.status_b | RTC_STATUS_B_SET);
        CHECK(rtc_snapshots_agree(&a, &b) == 0);
        b = a;
        b.status_d = 0;
        CHECK(rtc_snapshots_agree(&a, &b) == 0);

        /* The update-in-progress flag is the exception, and has to be: it is
         * the one bit in those registers the hardware moves on its own, and it
         * moves once a second, so counting it would make every second read
         * disagree with itself for no reason. */
        b = a;
        b.status_a = (unsigned char)(a.status_a | RTC_STATUS_A_UPDATING);
        CHECK(rtc_snapshots_agree(&a, &b) == 1);

        CHECK(rtc_snapshots_agree(&a, 0) == 0);
        CHECK(rtc_snapshots_agree(0, &b) == 0);

        /* A battery that went ends the read instead of earning another try.
         * On some parts reading register D SETS the validity bit again, so the
         * pair a dead cell produces is a disagreement (clear, then set) and the
         * pair after it agrees, is in range, and is whatever the dead cell
         * left. The first sample of that sequence is the only usable one, and
         * this is what the driver stops on. */
        b = a;
        b.status_d = 0;
        CHECK(rtc_contents_lost(&b) == 1);
        CHECK(rtc_snapshots_agree(&b, &a) == 0);
        CHECK(rtc_contents_lost(&a) == 0);
        CHECK(rtc_contents_lost(0) == 1);
    }

    /* What the boot read does with a pair, which is the policy, not the
     * comparison: a healthy pair is used, a straddled update is read again, and
     * a clock that says its battery went ends the read in either order. That
     * last one is why this is a function and not two lines in the loop: a pair
     * that both disagrees AND says the contents are lost must stop rather than
     * be read again, and nothing in the comparison alone says so. */
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

        /* Clear then set, which is the pair a dead cell hands back when reading
         * register D sets the bit; and set then clear, the same fact arriving
         * in the other order. Both stop, and neither is retried. */
        b = a;
        b.status_d = 0;
        CHECK(rtc_pair_disposition(&b, &a) == RTC_PAIR_STOP);
        CHECK(rtc_pair_disposition(&a, &b) == RTC_PAIR_STOP);

        /* And the pair after it, which agrees and is in range and is whatever
         * the dead cell left: stopping on the valid pair is what keeps this
         * one from ever being taken. */
        b.status_d = 0;
        CHECK(rtc_pair_disposition(&b, &b) == RTC_PAIR_STOP);
    }

    subtick_rows();
    set_rows();
    monotonic_rows();
    clock_write_rows();
    return fails > 100 ? 100 : fails;
}
