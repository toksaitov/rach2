#include "portio.h"
#include "../../klog.h"
#include "cmos.h"

/*
 * The century is inferred, not read. Register 0x32 has one on machines
 * whose firmware maintains it and has whatever was in that CMOS byte on
 * machines whose firmware does not, and an unrelated byte decodes to a
 * plausible-looking century as easily as to an absurd one. A two-digit year
 * plus this base is wrong in 2100; reading a byte nobody wrote is wrong now.
 */
#define RTC_CENTURY_BASE 2000

/** Decode one packed-BCD byte, or -1 if either nibble is not a digit. */
static int32_t rtc_from_bcd(uint8_t value)
{
    uint8_t high = (uint8_t)(value >> 4);
    uint8_t low = (uint8_t)(value & 0x0F);

    if (high > 9 || low > 9) {
        return -1;
    }

    return (int32_t)high * 10 + (int32_t)low;
}

/**
 * Whether this sample says the clock lost its own contents.
 *
 * Register D's validity bit is clear once the battery has gone, and on some
 * parts READING that register sets it again, so the bit tells the truth once.
 * A sample that sees it clear therefore ends the read: taking another pair
 * would find them agreeing, in range, and holding whatever the dead cell left,
 * which is exactly the clock this guard exists to refuse. A missing sample is
 * counted the same way, since the answer here decides whether to believe a
 * clock.
 *
 * @return 1 when the contents are not to be believed, 0 otherwise.
 */
int rtc_contents_lost(const rtc_snapshot_t *raw)
{
    if (!raw) {
        return 1;
    }

    return (raw->status_d & RTC_STATUS_D_VALID) == 0;
}

/**
 * Whether two samples of the clock describe the same instant and the same
 * clock.
 *
 * The caller takes two samples and decodes neither unless this says they agree,
 * because the registers advance while they are being read. Every field is
 * compared, the three status registers included: the decode judges the clock by
 * what status A, B and D say, and a sample whose status changed between the two
 * reads is one where that judgement would rest on a register the other read did
 * not see.
 *
 * The update-in-progress bit is the one exception and is masked off both sides.
 * It is the only bit in those registers that the hardware moves on its own, it
 * moves once a second, and it says nothing about the values themselves.
 *
 * @return 1 when the two agree, 0 otherwise.
 */
int rtc_snapshots_agree(const rtc_snapshot_t *a, const rtc_snapshot_t *b)
{
    if (!a || !b) {
        return 0;
    }

    return a->second == b->second && a->minute == b->minute && a->hour == b->hour &&
           a->day == b->day && a->month == b->month && a->year == b->year &&
           (uint8_t)(a->status_a & ~RTC_STATUS_A_UPDATING) ==
               (uint8_t)(b->status_a & ~RTC_STATUS_A_UPDATING) &&
           a->status_b == b->status_b && a->status_d == b->status_d;
}

/**
 * What a pair of samples is worth: use it, take another, or stop reading.
 *
 * The whole policy of the boot read is here, not in the loop that reads,
 * so that it can be stated once and tested. Two rules, and the order of them is
 * the point. A clock that says it lost its contents STOPS the read, even though
 * the pair also disagrees, because taking another pair is exactly the mistake:
 * reading register D can set the validity bit again, so the only usable sample
 * is the one already in hand and every later one is a fiction that agrees with
 * itself. Any other disagreement is a read that straddled an update, which is
 * worth taking again.
 *
 * @return `RTC_PAIR_ACCEPT`, `RTC_PAIR_RETRY` or `RTC_PAIR_STOP`.
 */
int rtc_pair_disposition(const rtc_snapshot_t *a, const rtc_snapshot_t *b)
{
    if (rtc_contents_lost(a) || rtc_contents_lost(b)) {
        return RTC_PAIR_STOP;
    }
    if (!rtc_snapshots_agree(a, b)) {
        return RTC_PAIR_RETRY;
    }

    return RTC_PAIR_ACCEPT;
}

/**
 * Convert a raw CMOS sample to seconds since the Unix epoch, in UTC.
 *
 * This is the whole of the clock's arithmetic and it touches no hardware, so
 * `tests/host/rtc_hosttest.c` runs it natively over crafted samples: the two
 * encodings, the 12-hour PM flag, a leap year, and the out-of-range fields a
 * machine with a dead battery reports.
 *
 * The order of operations is not free. The PM flag rides in the high bit of the
 * hour register, so it is taken off BEFORE the value is decoded; a BCD decode
 * of an hour with that bit still set reads as a nonsense digit and would be
 * rejected, not corrected. A field outside its range fails the whole
 * sample: a clock that reports month 0 is not a clock to base a build on.
 *
 * @param raw the six registers and status B, as read.
 * @param seconds set to the decoded time when the sample is good.
 * @param out when not NULL, the same instant as fields, which is what the
 *        console line prints and what the host test reads to tell a wrong hour
 *        from a wrong total.
 * @return 0 on success, -1 if any field is out of range.
 */
int rtc_decode(const rtc_snapshot_t *raw, int64_t *seconds, rtc_time_t *out)
{
    int32_t second;
    int32_t minute;
    int32_t hour;
    int32_t day;
    int32_t month;
    int32_t year;
    rtc_time_t when;
    int binary;
    int pm = 0;
    uint8_t hour_reg;

    if (!raw || !seconds) {
        return -1;
    }

    /* What the clock says about itself, before anything it says about the time.
     * A machine whose battery went reports VRT clear and has whatever was in
     * those bytes, which can be stable and in range and completely wrong; a
     * clock with SET raised or a divider that is not the running one is not
     * advancing; and DSE means the clock shifts itself twice a year, which this
     * system will not model. Each is a reason to keep the epoch instead. */
    if (!(raw->status_d & RTC_STATUS_D_VALID)) {
        return -1;
    }
    if (raw->status_b & RTC_STATUS_B_SET) {
        return -1;
    }
    if (raw->status_b & RTC_STATUS_B_DSE) {
        return -1;
    }
    if ((raw->status_a & RTC_STATUS_A_DIVIDER) != RTC_STATUS_A_DIVIDER_NORMAL) {
        return -1;
    }

    binary = (raw->status_b & RTC_STATUS_B_BINARY) != 0;

    hour_reg = raw->hour;
    if (!(raw->status_b & RTC_STATUS_B_24_HOUR)) {
        pm = (hour_reg & RTC_HOUR_PM) != 0;
        hour_reg = (uint8_t)(hour_reg & ~RTC_HOUR_PM);
    }

    if (binary) {
        second = raw->second;
        minute = raw->minute;
        hour = hour_reg;
        day = raw->day;
        month = raw->month;
        year = raw->year;
    } else {
        second = rtc_from_bcd(raw->second);
        minute = rtc_from_bcd(raw->minute);
        hour = rtc_from_bcd(hour_reg);
        day = rtc_from_bcd(raw->day);
        month = rtc_from_bcd(raw->month);
        year = rtc_from_bcd(raw->year);
        if (second < 0 || minute < 0 || hour < 0 || day < 0 || month < 0 || year < 0) {
            return -1;
        }
    }

    if (!(raw->status_b & RTC_STATUS_B_24_HOUR)) {
        if (hour < 1 || hour > 12) {
            return -1;
        }
        /* 12 AM is hour 0 and 12 PM is hour 12; every other PM hour adds 12. */
        hour = hour % 12;
        if (pm) {
            hour += 12;
        }
    }

    year += RTC_CENTURY_BASE;
    if (second > 59 || minute > 59 || hour > 23 || month < 1 || month > 12 || year < 1970) {
        return -1;
    }
    if (day < 1 || day > rtc_days_in_month(year, month)) {
        return -1;
    }

    when.year = year;
    when.month = month;
    when.day = day;
    when.hour = hour;
    when.minute = minute;
    when.second = second;
    *seconds = rtc_seconds_of(&when);

    if (out) {
        *out = when;
    }

    return 0;
}

#if !defined(RACH2_HOSTTEST) || defined(RTC_HOST_TEST)

#include "../../lib/kirq.h"
#include "pic.h"
#include "pit.h"

/* Kernel-side only, like krandom.c's hardware half (see kio.c / kernel.c). */
extern volatile uint32_t kernel_uptime_seconds;
extern volatile uint32_t kernel_second_ticks;

#define RTC_ADDRESS_PORT 0x70
#define RTC_DATA_PORT    0x71

#define RTC_REG_SECOND   0x00
#define RTC_REG_MINUTE   0x02
#define RTC_REG_HOUR     0x04
#define RTC_REG_WEEKDAY  0x06
#define RTC_REG_DAY      0x07
#define RTC_REG_MONTH    0x08
#define RTC_REG_YEAR     0x09
#define RTC_REG_STATUS_A 0x0A
#define RTC_REG_STATUS_B 0x0B
#define RTC_REG_STATUS_C 0x0C
#define RTC_REG_STATUS_D 0x0D

#define RTC_REG_SECOND_ALARM 0x01
#define RTC_REG_MINUTE_ALARM 0x03
#define RTC_REG_HOUR_ALARM   0x05
#define RTC_STATUS_B_AIE     0x20 /* set: the alarm raises the interrupt */
#define RTC_STATUS_C_AF      0x20 /* set: the alarm has come since C was read */

/*
 * How long to wait for an update to finish. An RTC update takes under two
 * milliseconds and each read here is a pair of port accesses, so this bound is
 * orders of magnitude past the wait and is a bound on a WEDGED clock rather
 * than a slow one. It is a spin count and not a tick count deliberately: this
 * runs at boot and must not depend on the timer already being live.
 */
#define RTC_UPDATE_SPINS 1000000

/* How many times to re-read a sample that changed under us before giving up. */
#define RTC_SAMPLE_TRIES 4

/** Read one CMOS register. Bit 7 of the address port is left clear, so NMI
 *  stays enabled: this reads the clock, it does not change the machine. */
static uint8_t rtc_register(uint8_t reg)
{
    outb(RTC_ADDRESS_PORT, reg);

    return inb(RTC_DATA_PORT);
}

/** Wait for the update-in-progress flag to clear. Returns 0, or -1 on a clock
 *  that never finishes updating. */
static int rtc_wait_ready(void)
{
    uint32_t spins;

    for (spins = 0; spins < RTC_UPDATE_SPINS; spins++) {
        if (!(rtc_register(RTC_REG_STATUS_A) & RTC_STATUS_A_UPDATING)) {
            return 0;
        }
    }

    return -1;
}

/** Read the six registers and status A, B and D once, after waiting out an
 *  update. */
static int rtc_sample(rtc_snapshot_t *out)
{
    if (rtc_wait_ready() != 0) {
        return -1;
    }
    out->second = rtc_register(RTC_REG_SECOND);
    out->minute = rtc_register(RTC_REG_MINUTE);
    out->hour = rtc_register(RTC_REG_HOUR);
    out->day = rtc_register(RTC_REG_DAY);
    out->month = rtc_register(RTC_REG_MONTH);
    out->year = rtc_register(RTC_REG_YEAR);
    out->status_a = rtc_register(RTC_REG_STATUS_A);
    out->status_b = rtc_register(RTC_REG_STATUS_B);
    out->status_d = rtc_register(RTC_REG_STATUS_D);

    return 0;
}

/** Write `value`'s last two decimal digits at `out`, without a terminator. */
static void rtc_two(char *out, int32_t value)
{
    out[0] = (char)('0' + (value / 10) % 10);
    out[1] = (char)('0' + value % 10);
}

/**
 * Read the CMOS clock once and anchor the system's idea of real time to it.
 *
 * The read is taken twice and accepted only when the two agree, because the
 * registers advance while they are being read: a sample that straddles the
 * turn of a minute reports the old minute with the new second, which is a time
 * an hour wrong once an hour. Waiting out the update-in-progress flag makes
 * that rare and does not make it impossible, since the flag clears a little
 * before the registers change.
 *
 * A machine with no working clock is not a machine that should refuse to boot.
 * A failed read leaves the anchor unset, real time falls back to time since
 * boot the way it behaved before this driver existed, and the console says so.
 */
void rtc_init(void)
{
    rtc_snapshot_t first;
    rtc_snapshot_t second;
    rtc_time_t when;
    char text[20];
    int64_t seconds = 0;
    uint32_t uptime;
    int disposition;
    int try;

    for (try = 0; try < RTC_SAMPLE_TRIES; try++) {
        if (rtc_sample(&first) != 0 || rtc_sample(&second) != 0) {
            break;
        }
        /* The uptime is taken with the sample and not before it. Interrupts are
         * on here, and waiting out an update can cost a tick or two, so a
         * reading taken before the sampling would date the clock earlier than
         * it was and put the machine that far ahead for the rest of the boot.
         */
        uptime = kernel_uptime_seconds;
        disposition = rtc_pair_disposition(&first, &second);
        if (disposition == RTC_PAIR_STOP) {
            break;
        }
        if (disposition == RTC_PAIR_RETRY) {
            continue;
        }
        if (rtc_decode(&first, &seconds, &when) != 0) {
            break;
        }
        rtc_anchor(seconds, uptime);

        /* Composed by hand: this printf has no field width, so the two-digit
         * fields would print as one digit and read as a different time. */
        rtc_two(text + 0, when.year / 100);
        rtc_two(text + 2, when.year);
        text[4] = '-';
        rtc_two(text + 5, when.month);
        text[7] = '-';
        rtc_two(text + 8, when.day);
        text[10] = ' ';
        rtc_two(text + 11, when.hour);
        text[13] = ':';
        rtc_two(text + 14, when.minute);
        text[16] = ':';
        rtc_two(text + 17, when.second);
        text[19] = '\0';
        klog(KLOG_INFO, "rtc", "%s UTC", text);

        return;
    }

    klog(KLOG_WARN, "rtc", "no clock: times run from the epoch");
}

int rtc_read_clock(rtc_time_t *out)
{
    rtc_snapshot_t first;
    rtc_snapshot_t second;
    int64_t seconds;
    uint32_t flags;
    int sampled, try;

    for (try = 0; try < RTC_SAMPLE_TRIES; try++) {
        flags = kirq_save();
        sampled = rtc_sample(&first) == 0 && rtc_sample(&second) == 0;
        kirq_restore(flags);
        if (!sampled) {
            return -1;
        }
        switch (rtc_pair_disposition(&first, &second)) {
        case RTC_PAIR_ACCEPT:
            return rtc_decode(&first, &seconds, out);
        case RTC_PAIR_RETRY:
            break;
        default:
            return -1;
        }
    }
    return -1;
}

/** Write `value` into the CMOS register `reg`. */
static void rtc_write_register(uint8_t reg, uint8_t value)
{
    outb(RTC_ADDRESS_PORT, reg);
    outb(RTC_DATA_PORT, value);
}

/** `value`, 0 to 99, as status B says the clock keeps it: binary or BCD. */
static uint8_t rtc_to_register(int32_t value, int binary)
{
    return (uint8_t)(binary ? value : (value / 10) << 4 | value % 10);
}

static uint8_t rtc_hour_register(int32_t hour, uint8_t b);

int rtc_write_clock(const rtc_time_t *t)
{
    int32_t weekday, yearday;
    uint8_t b, hour;
    uint32_t flags;
    int binary;

    if (t->year < RTC_CENTURY_BASE || t->year > RTC_CENTURY_BASE + 99 || t->month < 1 ||
        t->month > 12 || t->day < 1 || t->day > rtc_days_in_month(t->year, t->month) ||
        t->hour < 0 || t->hour > 23 || t->minute < 0 || t->minute > 59 || t->second < 0 ||
        t->second > 59) {
        return -1;
    }
    rtc_week_and_year_day(t, &weekday, &yearday);
    flags = kirq_save();
    b = rtc_register(RTC_REG_STATUS_B);
    binary = (b & RTC_STATUS_B_BINARY) != 0;
    hour = rtc_hour_register(t->hour, b);
    rtc_write_register(RTC_REG_STATUS_B, (uint8_t)(b | RTC_STATUS_B_SET));
    rtc_write_register(RTC_REG_SECOND, rtc_to_register(t->second, binary));
    rtc_write_register(RTC_REG_MINUTE, rtc_to_register(t->minute, binary));
    rtc_write_register(RTC_REG_HOUR, hour);
    rtc_write_register(RTC_REG_WEEKDAY, rtc_to_register(weekday + 1, binary));
    rtc_write_register(RTC_REG_DAY, rtc_to_register(t->day, binary));
    rtc_write_register(RTC_REG_MONTH, rtc_to_register(t->month, binary));
    rtc_write_register(RTC_REG_YEAR, rtc_to_register(t->year - RTC_CENTURY_BASE, binary));
    rtc_write_register(RTC_REG_STATUS_B, (uint8_t)(b & ~RTC_STATUS_B_SET));
    kirq_restore(flags);
    return 0;
}

/** `reg`, an alarm or time register, decoded from the clock's encoding, with
 * the hour's PM bit turned into a 24-hour hour; -1 for a value that is not
 * a number in the encoding. */
static int32_t rtc_from_register(uint8_t value, uint8_t b, int hour)
{
    int32_t v;
    int pm = 0;

    if (hour && !(b & RTC_STATUS_B_24_HOUR)) {
        pm = (value & RTC_HOUR_PM) != 0;
        value = (uint8_t)(value & ~RTC_HOUR_PM);
    }
    v = (b & RTC_STATUS_B_BINARY) ? (int32_t)value : rtc_from_bcd(value);
    if (v >= 0 && hour && !(b & RTC_STATUS_B_24_HOUR)) {
        v = v % 12 + (pm ? 12 : 0);
    }
    return v;
}

/** The 24-hour hour `hour` as the hour register of a clock whose status B is
 * `b` keeps it. */
static uint8_t rtc_hour_register(int32_t hour, uint8_t b)
{
    int binary = (b & RTC_STATUS_B_BINARY) != 0;
    int32_t hour12;

    if (b & RTC_STATUS_B_24_HOUR) {
        return rtc_to_register(hour, binary);
    }
    hour12 = hour % 12 == 0 ? 12 : hour % 12;
    return (uint8_t)(rtc_to_register(hour12, binary) | (hour >= 12 ? RTC_HOUR_PM : 0));
}

void rtc_alarm_write(const rtc_time_t *t)
{
    uint32_t flags = kirq_save();
    uint8_t b = rtc_register(RTC_REG_STATUS_B);
    int binary = (b & RTC_STATUS_B_BINARY) != 0;

    rtc_write_register(RTC_REG_SECOND_ALARM, rtc_to_register(t->second, binary));
    rtc_write_register(RTC_REG_MINUTE_ALARM, rtc_to_register(t->minute, binary));
    rtc_write_register(RTC_REG_HOUR_ALARM, rtc_hour_register(t->hour, b));
    (void)rtc_register(RTC_REG_STATUS_C);
    kirq_restore(flags);
}

int rtc_alarm_read(rtc_time_t *t)
{
    uint32_t flags = kirq_save();
    uint8_t b = rtc_register(RTC_REG_STATUS_B);

    t->second = rtc_from_register(rtc_register(RTC_REG_SECOND_ALARM), b, 0);
    t->minute = rtc_from_register(rtc_register(RTC_REG_MINUTE_ALARM), b, 0);
    t->hour = rtc_from_register(rtc_register(RTC_REG_HOUR_ALARM), b, 1);
    kirq_restore(flags);
    return (b & RTC_STATUS_B_AIE) != 0;
}

void rtc_alarm_interrupts(int on)
{
    uint32_t flags = kirq_save();
    uint8_t b = rtc_register(RTC_REG_STATUS_B);

    b = (uint8_t)(on ? b | RTC_STATUS_B_AIE : b & ~RTC_STATUS_B_AIE);
    rtc_write_register(RTC_REG_STATUS_B, b);
    (void)rtc_register(RTC_REG_STATUS_C);
    kirq_restore(flags);
}

int rtc_alarm_ack(void)
{
    return (rtc_register(RTC_REG_STATUS_C) & RTC_STATUS_C_AF) != 0;
}

/* The CMOS clock's interrupt, IRQ 8, on the slave 8259, which reaches the
 * processor through the master's cascade line, 2. */
#define RTC_IRQ 8

void rtc_alarm_attach(isr_handler_t handler)
{
    register_isr_handler((uint8_t)IRQ_VECTOR(RTC_IRQ), handler, "rtc");
    pic_unmask_irq(2);
}

#endif /* !RACH2_HOSTTEST || RTC_HOST_TEST */
