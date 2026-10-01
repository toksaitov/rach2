#include "klog.h"
#include "rtc.h"

#define RTC_SECONDS_PER_DAY    86400
#define RTC_SECONDS_PER_HOUR   3600
#define RTC_SECONDS_PER_MINUTE 60
#define RTC_EPOCH_YEAR         1970

/*
 * The century is inferred, not read. Register 0x32 has one on machines
 * whose firmware maintains it and has whatever was in that CMOS byte on
 * machines whose firmware does not, and an unrelated byte decodes to a
 * plausible-looking century as easily as to an absurd one. A two-digit year
 * plus this base is wrong in 2100; reading a byte nobody wrote is wrong now.
 */
#define RTC_CENTURY_BASE 2000

/* The timer runs at 100 Hz (kernel.c calls init_pit(100)), so a tick is 10
 * ms. */
#define RTC_TICKS_PER_SECOND     100
#define RTC_NANOSECONDS_PER_TICK 10000000

/** Whether `year` (a full year, not the two-digit register) has a February
 * 29. */
static int rtc_is_leap(int32_t year)
{
    if (year % 400 == 0) {
        return 1;
    }
    if (year % 100 == 0) {
        return 0;
    }

    return year % 4 == 0;
}

/** Days in `month` (1..12) of `year`. */
static int32_t rtc_days_in_month(int32_t year, int32_t month)
{
    static const int32_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (month == 2 && rtc_is_leap(year)) {
        return 29;
    }

    return days[month - 1];
}

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
    int32_t days;
    int32_t each;
    int32_t m;
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
    if (second > 59 || minute > 59 || hour > 23 || month < 1 || month > 12 ||
        year < RTC_EPOCH_YEAR) {
        return -1;
    }
    if (day < 1 || day > rtc_days_in_month(year, month)) {
        return -1;
    }

    days = 0;
    for (each = RTC_EPOCH_YEAR; each < year; each++) {
        days += rtc_is_leap(each) ? 366 : 365;
    }
    for (m = 1; m < month; m++) {
        days += rtc_days_in_month(year, m);
    }
    days += day - 1;

    *seconds = (int64_t)days * RTC_SECONDS_PER_DAY + (int64_t)hour * RTC_SECONDS_PER_HOUR +
               (int64_t)minute * RTC_SECONDS_PER_MINUTE + (int64_t)second;

    if (out) {
        out->year = year;
        out->month = month;
        out->day = day;
        out->hour = hour;
        out->minute = minute;
        out->second = second;
    }

    return 0;
}

/**
 * Real time from an anchor: the second a clock reported, the uptime second at
 * which it was read, the uptime now, and the ticks into the current second.
 *
 * Kept apart from the hardware, and from the statics that hold the anchor, so
 * that `tests/host/rtc_hosttest.c` can state the case that matters in one line:
 * a clock read at uptime 5 saying 1000, asked again at uptime 7, says 1002.
 * Adding the whole uptime instead of the time since the READ counts the stretch
 * before the sample twice, and on a machine that reads its clock early that
 * error is under a second, which no tolerance test would catch and a student
 * would never see until it mattered.
 *
 * It counts uptime SECONDS, not ticks because the tick counter is 32
 * bits and wraps after about 497 days, and a difference of ticks would jump
 * back by that whole cycle at the wrap. The sub-second part cannot wrap: it
 * only ever spans one second.
 *
 * With the anchor at zero this is time since boot, which is what a machine with
 * no readable clock reports.
 */
void rtc_time_from_anchor(int64_t anchor_seconds, uint32_t anchor_uptime, uint32_t uptime,
                          uint32_t subsecond_ticks, int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = anchor_seconds + (int64_t)(uptime - anchor_uptime);
    *nanoseconds = (int64_t)(subsecond_ticks % RTC_TICKS_PER_SECOND) * RTC_NANOSECONDS_PER_TICK;
}

#if defined(__i386__) || defined(RTC_HOST_TEST)

#include "lib/kirq.h"
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
#define RTC_REG_STATUS_D 0x0D

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

/*
 * The anchor: the second the clock reported, and the uptime second at which it
 * was read. Real time is the first plus the seconds elapsed since that read,
 * which is not the same as the first plus the whole uptime: the kernel has been
 * running for a while by the time the clock is sampled, and adding that stretch
 * twice would put the machine ahead with nothing to show for it.
 */
static int64_t rtc_anchor_seconds;
static uint32_t rtc_anchor_uptime;
static int rtc_anchored;

/* What setting the clock added to the time the anchor keeps, in seconds and
 * nanoseconds below a second, which every reading of real time adds; and the
 * last reading, before which no later one may fall until the clock is set. */
static int64_t rtc_step_seconds;
static int32_t rtc_step_nanoseconds;
static int64_t rtc_last_seconds;
static int64_t rtc_last_nanoseconds;

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
        rtc_anchor_seconds = seconds;
        rtc_anchor_uptime = uptime;
        rtc_anchored = 1;

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

void rtc_week_and_year_day(const rtc_time_t *t, int32_t *weekday, int32_t *yearday)
{
    int32_t days = 0, each;

    for (each = RTC_EPOCH_YEAR; each < t->year; each++) {
        days += rtc_is_leap(each) ? 366 : 365;
    }
    *yearday = t->day - 1;
    for (each = 1; each < t->month; each++) {
        *yearday += rtc_days_in_month(t->year, each);
    }
    *weekday = (days + *yearday + 4) % 7; /* 1970-01-01 was a Thursday */
}

int rtc_write_clock(const rtc_time_t *t)
{
    int32_t weekday, yearday, hour12;
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
    if (b & RTC_STATUS_B_24_HOUR) {
        hour = rtc_to_register(t->hour, binary);
    } else {
        hour12 = t->hour % 12 == 0 ? 12 : t->hour % 12;
        hour = (uint8_t)(rtc_to_register(hour12, binary) | (t->hour >= 12 ? RTC_HOUR_PM : 0));
    }
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

/** Whether the clock was read at boot. */
int rtc_is_anchored(void)
{
    return rtc_anchored;
}

/**
 * The real time at which this machine's clock was read, which is as close to
 * boot as it knows.
 *
 * Reported by the nodes that have no history of their own: a device, a /proc
 * file, a pipe. Zero would be a claim about 1970, and NOW would be worse: a
 * synthetic file whose modification time advanced on every stat would look
 * freshly written to anything comparing times, which is what a make does.
 */
int64_t rtc_boot_time(void)
{
    return rtc_anchor_seconds;
}

/**
 * The real time at which this machine booted, which is not the same moment.
 *
 * `rtc_boot_time` is when the clock was READ, some way into the boot; this is
 * that reading less the uptime it was taken at, so it names uptime zero. It is
 * what `/proc/stat`'s `btime` needs: a value that does not move between two
 * reads within one boot, which the read moment does not promise either, but
 * which anything comparing boots would be misled by. Zero when the clock could
 * not be read, the same answer the rest of this file gives.
 */
int64_t rtc_boot_epoch(void)
{
    if (!rtc_anchored) {
        return rtc_step_seconds;
    }
    return rtc_anchor_seconds - (int64_t)rtc_anchor_uptime + rtc_step_seconds;
}

/** Add the clock's step to a time the anchor keeps. */
static void rtc_add_step(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds += rtc_step_seconds;
    *nanoseconds += rtc_step_nanoseconds;
    while (*nanoseconds >= 1000000000) {
        *nanoseconds -= 1000000000;
        (*seconds)++;
    }
}

/**
 * Time since boot now: the uptime seconds, the ticks of this second and the
 * PIT's place within the tick; the caller has interrupts off.
 *
 * IRQ0 can be pending after the counter wraps. Read its pending bit on both
 * sides of the latch; if it changed, latch again. A pending tick belongs to
 * the new period even before the handler counts it.
 */
static void rtc_since_boot(uint32_t *uptime, uint32_t *ticks, uint32_t *phase_ns)
{
    int pending;

    *uptime = kernel_uptime_seconds;
    *ticks = kernel_second_ticks;
    pending = pic_irq_pending(0);
    *phase_ns = pit_subtick_ns();
    if (pending != pic_irq_pending(0)) {
        pending = 1;
        *phase_ns = pit_subtick_ns();
    }
    *ticks += (uint32_t)pending;
    if (*ticks >= RTC_TICKS_PER_SECOND) {
        *ticks -= RTC_TICKS_PER_SECOND;
        (*uptime)++;
    }
}

/* The time the anchor keeps now, with the PIT position added to the
 * delivered ticks; the caller has interrupts off. */
static void rtc_kept_time(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t uptime, ticks, phase_ns;

    rtc_since_boot(&uptime, &ticks, &phase_ns);
    rtc_time_from_anchor(rtc_anchor_seconds, rtc_anchor_uptime, uptime, ticks, seconds,
                         nanoseconds);
    *nanoseconds += phase_ns;
}

/**
 * The current real time: the time the anchor keeps, with the clock's step
 * added. The saved result prevents backward steps if interrupts stay masked
 * across several periods, which the PIC cannot count, or a faulty latch reads
 * zero, until the clock is set. This only supplies now; callers that set an
 * explicit file time keep it.
 */
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t flags = kirq_save();

    rtc_kept_time(seconds, nanoseconds);
    rtc_add_step(seconds, nanoseconds);
    if (*seconds < rtc_last_seconds ||
        (*seconds == rtc_last_seconds && *nanoseconds < rtc_last_nanoseconds)) {
        *seconds = rtc_last_seconds;
        *nanoseconds = rtc_last_nanoseconds;
    }
    rtc_last_seconds = *seconds;
    rtc_last_nanoseconds = *nanoseconds;
    kirq_restore(flags);
}

/* The last reading of time since boot, which no later one is earlier than. */
static int64_t rtc_mono_seconds;
static int64_t rtc_mono_nanoseconds;

/**
 * Time since boot, with the PIT's place within the tick, as real time reads
 * it, and never earlier than the reading before it, for the reasons
 * `rtc_realtime` gives. Setting the clock does not move it.
 */
void rtc_monotonic(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t flags = kirq_save();
    uint32_t uptime, ticks, phase_ns;

    rtc_since_boot(&uptime, &ticks, &phase_ns);
    *seconds = uptime;
    *nanoseconds = (int64_t)ticks * RTC_NANOSECONDS_PER_TICK + phase_ns;
    if (*seconds < rtc_mono_seconds ||
        (*seconds == rtc_mono_seconds && *nanoseconds < rtc_mono_nanoseconds)) {
        *seconds = rtc_mono_seconds;
        *nanoseconds = rtc_mono_nanoseconds;
    }
    rtc_mono_seconds = *seconds;
    rtc_mono_nanoseconds = *nanoseconds;
    kirq_restore(flags);
}

/**
 * The current real time to the last tick, without the PIT's place within it:
 * the coarse clock, which costs no latch.
 */
void rtc_coarse_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t flags = kirq_save();

    rtc_time_from_anchor(rtc_anchor_seconds, rtc_anchor_uptime, kernel_uptime_seconds,
                         kernel_second_ticks, seconds, nanoseconds);
    rtc_add_step(seconds, nanoseconds);
    kirq_restore(flags);
}

/**
 * Make the real time `seconds` and `nanoseconds` now, which clock_settime,
 * settimeofday and stime do for a privileged caller.
 *
 * The anchor stays as the boot read it; the difference between the time it
 * keeps now and the time asked for becomes the step every reading adds, so the
 * clock goes on from the new time at the same rate. A reading after a step
 * back may be earlier than one before it, as the step asks, and the boot epoch
 * moves with the step, as Linux's does, so that a process's start time read
 * from it stays where it was against the clock.
 */
void rtc_set_realtime(int64_t seconds, int32_t nanoseconds)
{
    int64_t kept_seconds;
    int64_t kept_nanoseconds;
    uint32_t flags = kirq_save();

    rtc_kept_time(&kept_seconds, &kept_nanoseconds);
    while (kept_nanoseconds >= 1000000000) {
        kept_nanoseconds -= 1000000000;
        kept_seconds++;
    }
    rtc_step_seconds = seconds - kept_seconds;
    rtc_step_nanoseconds = nanoseconds - (int32_t)kept_nanoseconds;
    if (rtc_step_nanoseconds < 0) {
        rtc_step_nanoseconds += 1000000000;
        rtc_step_seconds--;
    }
    rtc_last_seconds = seconds;
    rtc_last_nanoseconds = nanoseconds;
    kirq_restore(flags);
}

#endif /* __i386__ || RTC_HOST_TEST */
