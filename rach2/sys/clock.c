/*
 * clock.c: the kernel's real time, kept from one reading of the machine's
 * clock at boot (the anchor, which the platform's clock driver sets through
 * `rtc_anchor`) and the tick since, with the calendar arithmetic the clock
 * drivers and the clock calls share, and the sequence count by which another
 * processor reads the tick whole.
 */

#include "rtc.h"
#include "timer.h"
#include "vdso.h"

#include "arch/arch.h"
#include "lib/kirq.h"
#include "lib/spinlock.h"
#include "proc/smp.h"

/* The uptime the timer handler keeps (kernel.c). */
extern volatile uint32_t kernel_uptime_seconds;
extern volatile uint32_t kernel_second_ticks;

volatile uint32_t kernel_tick_seq;

/* The processor in a change of the tick, plus 1, or 0 between changes. */
static volatile uint32_t tick_writer;

void tick_write_begin(void)
{
    tick_writer = arch_cpu_id() + 1u;
    kernel_tick_seq++;
}

void tick_write_end(void)
{
    kernel_tick_seq++;
    tick_writer = 0;
}

uint32_t tick_read_begin(void)
{
    uint32_t seq;

    if (tick_writer == arch_cpu_id() + 1u) {
        return 0;
    }
    while ((seq = kernel_tick_seq) & 1u) {
        smp_service();
        arch_relax();
    }
    return seq;
}

int tick_read_retry(uint32_t seq)
{
    return tick_writer != arch_cpu_id() + 1u && kernel_tick_seq != seq;
}

void tick_now(uint32_t *seconds, uint32_t *second_ticks, uint64_t *ticks)
{
    uint32_t flags = kirq_save();
    uint32_t seq;

    do {
        seq = tick_read_begin();
        *seconds = kernel_uptime_seconds;
        *second_ticks = kernel_second_ticks;
        *ticks = kernel_ticks;
    } while (tick_read_retry(seq));
    kirq_restore(flags);
}

uint64_t tick_count(void)
{
    uint32_t flags = kirq_save();
    uint32_t seq;
    uint64_t ticks;

    do {
        seq = tick_read_begin();
        ticks = kernel_ticks;
    } while (tick_read_retry(seq));
    kirq_restore(flags);
    return ticks;
}

#define RTC_SECONDS_PER_DAY    86400
#define RTC_SECONDS_PER_HOUR   3600
#define RTC_SECONDS_PER_MINUTE 60
#define RTC_EPOCH_YEAR         1970
/* The Gregorian calendar repeats every 400 years, of 146097 days. */
#define RTC_CYCLE_YEARS        400
#define RTC_CYCLE_DAYS         146097

/* The tick's rate (TICK_HZ in timer.h), and its length. */
#define RTC_TICKS_PER_SECOND     TICK_HZ
#define RTC_NANOSECONDS_PER_TICK TICK_NS

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

int32_t rtc_days_in_month(int32_t year, int32_t month)
{
    static const int32_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (month == 2 && rtc_is_leap(year)) {
        return 29;
    }

    return days[month - 1];
}

/**
 * The uptime since the read is added, and not the whole uptime, which would
 * count the stretch of the boot before the read twice. On a machine that
 * reads its clock early that error is under a second, which no tolerance
 * would catch, so `tests/host/rtc_hosttest.c` states the case exactly: a
 * clock read at uptime 5 saying 1000 says 1002 at uptime 7.
 *
 * The difference is of uptime seconds and not of ticks, which the kernel
 * would need a 64-bit division to turn into seconds. The ticks within a
 * second cannot wrap.
 */
void rtc_time_from_anchor(int64_t anchor_seconds, uint32_t anchor_uptime, uint32_t uptime,
                          uint32_t subsecond_ticks, int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = anchor_seconds + (int64_t)(uptime - anchor_uptime);
    *nanoseconds = (int64_t)(subsecond_ticks % RTC_TICKS_PER_SECOND) * RTC_NANOSECONDS_PER_TICK;
}

/* The anchor: the second the clock reported, the uptime second at which it
 * was read, and whether the boot read it. Real time is the reported second
 * plus the uptime seconds since the read (`rtc_time_from_anchor`). */
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
/* Protect the clock's step and real-time floor across processors. */
static struct spinlock rtc_realtime_lock = SPINLOCK_INIT_NAMED("rtc_realtime_lock");

void rtc_anchor(int64_t seconds, uint32_t uptime)
{
    int64_t sec;
    int32_t nsec;

    rtc_anchor_seconds = seconds;
    rtc_anchor_uptime = uptime;
    rtc_anchored = 1;
    rtc_offset(&sec, &nsec);
    vclock_set_offset(sec, nsec);
}

/** Real time less the monotonic clock, with the real-time lock held: the
 * anchor less its uptime, and the step. */
static void rtc_offset_locked(int64_t *seconds, int32_t *nanoseconds)
{
    *seconds = rtc_step_seconds;
    if (rtc_anchored) {
        *seconds += rtc_anchor_seconds - (int64_t)rtc_anchor_uptime;
    }
    *nanoseconds = rtc_step_nanoseconds;
}

void rtc_offset(int64_t *seconds, int32_t *nanoseconds)
{
    uint32_t flags = spin_lock_irqsave(&rtc_realtime_lock);

    rtc_offset_locked(seconds, nanoseconds);
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
}

/** The days from 1970-01-01 to January 1 of `year`, a full year from 1970,
 * as whole 400-year cycles in `*cycles` and the days within the cycle,
 * which is returned. Each cycle has 146097 days, a whole number of weeks,
 * so the days within it give the day of the week; and splitting off the
 * cycles bounds the loop at 400 years whatever the year. A year before
 * 1970 counts as 1970. */
static int32_t rtc_days_to_year(int32_t year, int32_t *cycles)
{
    int32_t days = 0, each;

    *cycles = year > RTC_EPOCH_YEAR ? (year - RTC_EPOCH_YEAR) / RTC_CYCLE_YEARS : 0;
    for (each = RTC_EPOCH_YEAR + *cycles * RTC_CYCLE_YEARS; each < year; each++) {
        days += rtc_is_leap(each) ? 366 : 365;
    }
    return days;
}

void rtc_week_and_year_day(const rtc_time_t *t, int32_t *weekday, int32_t *yearday)
{
    int32_t cycles, days = rtc_days_to_year(t->year, &cycles), each;

    *yearday = t->day - 1;
    for (each = 1; each < t->month; each++) {
        *yearday += rtc_days_in_month(t->year, each);
    }
    *weekday = (days + *yearday + 4) % 7; /* 1970-01-01 was a Thursday */
}

int64_t rtc_seconds_of(const rtc_time_t *t)
{
    int32_t weekday, yearday, cycles, days = rtc_days_to_year(t->year, &cycles);

    rtc_week_and_year_day(t, &weekday, &yearday);
    return ((int64_t)cycles * RTC_CYCLE_DAYS + days + yearday) * RTC_SECONDS_PER_DAY +
           t->hour * RTC_SECONDS_PER_HOUR + t->minute * RTC_SECONDS_PER_MINUTE + t->second;
}

/**
 * The whole 400-year cycles are taken off by long division, one bit of the
 * quotient at a time, since i686 has no 64-bit divide without libgcc; the
 * quotient is below 2^30 for any `seconds` up to 2^63. What is left is
 * under a cycle, and the years of it are counted from 1970, whose leap
 * years fall as those of the same place in any cycle.
 */
int rtc_time_of_seconds(int64_t seconds, rtc_time_t *t)
{
    const uint64_t cycle = (uint64_t)RTC_CYCLE_DAYS * RTC_SECONDS_PER_DAY;
    uint64_t rest = (uint64_t)seconds, len;
    uint32_t cycles = 0, days, part;
    int32_t year = RTC_EPOCH_YEAR;
    int bit;

    if (seconds < 0) {
        return -1;
    }
    for (bit = 30; bit >= 0; bit--) {
        if (rest >= cycle << bit) {
            rest -= cycle << bit;
            cycles |= (uint32_t)1 << bit;
        }
    }
    while (rest >= (len = (uint64_t)(rtc_is_leap(year) ? 366 : 365) * RTC_SECONDS_PER_DAY)) {
        rest -= len;
        year++;
    }
    if (cycles > (uint32_t)(0x7fffffffL - year) / RTC_CYCLE_YEARS) {
        return -1;
    }
    part = (uint32_t)rest;
    days = part / RTC_SECONDS_PER_DAY;
    part %= RTC_SECONDS_PER_DAY;
    t->month = 1;
    while (days >= (uint32_t)rtc_days_in_month(year, t->month)) {
        days -= (uint32_t)rtc_days_in_month(year, t->month);
        t->month++;
    }
    t->year = year + (int32_t)cycles * RTC_CYCLE_YEARS;
    t->day = (int32_t)days + 1;
    t->hour = (int32_t)(part / RTC_SECONDS_PER_HOUR);
    t->minute = (int32_t)(part / RTC_SECONDS_PER_MINUTE % 60u);
    t->second = (int32_t)(part % 60u);
    return 0;
}

void rtc_realtime_unlocked(int64_t *seconds, int32_t *microseconds)
{
    int64_t sec;
    int32_t nsec;
    uint32_t us = kernel_second_ticks * TICK_US;

    rtc_offset_locked(&sec, &nsec);
    sec += kernel_uptime_seconds;
    us += (uint32_t)nsec / 1000u;
    while (us >= 1000000u) {
        us -= 1000000u;
        sec++;
    }
    *seconds = sec;
    *microseconds = (int32_t)us;
}

int rtc_is_anchored(void)
{
    return rtc_anchored;
}

/**
 * The anchor is the moment of the read, some way into the boot; this is that
 * reading less the uptime it was taken at, uptime zero, which is what `btime`
 * means.
 */
int64_t rtc_boot_epoch(void)
{
    int64_t seconds;
    int32_t nanoseconds;

    rtc_offset(&seconds, &nanoseconds);
    return seconds;
}

/** Add the clock's step to a time the anchor keeps, with the real-time lock
 * held. */
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
 * timer's place within the tick; the caller has interrupts off.
 *
 * Ticks can have passed that the handler has not counted, the one of a
 * pending interrupt or, with a reference, those of a late one, and they
 * belong to the time now before the handler counts them (`timer_phase`).
 */
static void rtc_since_boot(uint32_t *uptime, uint32_t *ticks, uint32_t *phase_ns)
{
    uint32_t seq;
    int pending;

    do {
        seq = tick_read_begin();
        *uptime = kernel_uptime_seconds;
        *ticks = kernel_second_ticks;
        timer_phase(&pending, phase_ns);
    } while (tick_read_retry(seq));
    *ticks += (uint32_t)pending;
    *uptime += *ticks / RTC_TICKS_PER_SECOND;
    *ticks %= RTC_TICKS_PER_SECOND;
}

void rtc_tick_phase(uint64_t *ticks, uint32_t *phase_ns)
{
    uint32_t flags = kirq_save();
    uint32_t seq;
    int pending;

    do {
        seq = tick_read_begin();
        *ticks = kernel_ticks;
        timer_phase(&pending, phase_ns);
    } while (tick_read_retry(seq));
    *ticks += (uint32_t)pending;
    kirq_restore(flags);
}

uint64_t rtc_ticks_now(void)
{
    uint64_t ticks;
    uint32_t phase_ns;

    rtc_tick_phase(&ticks, &phase_ns);
    return ticks;
}

/** The time the anchor keeps now, to the timer's place within the tick, or
 * by the clock from the TSC while it runs, with nanoseconds that may pass a
 * second; the caller has interrupts off. */
static void rtc_kept_time(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t uptime, ticks, phase_ns;

    if (vclock_read(0, 0, seconds, nanoseconds)) {
        if (rtc_anchored) {
            *seconds += rtc_anchor_seconds - (int64_t)rtc_anchor_uptime;
        }
        return;
    }
    rtc_since_boot(&uptime, &ticks, &phase_ns);
    rtc_time_from_anchor(rtc_anchor_seconds, rtc_anchor_uptime, uptime, ticks, seconds,
                         nanoseconds);
    *nanoseconds += phase_ns;
}

/**
 * The last reading is kept as a floor, since a reading can fall back:
 * without a reference counter a stretch with interrupts off longer than a
 * period loses periods the tick does not count, and a faulty latch of the
 * counter reads zero.
 */
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t flags = spin_lock_irqsave(&rtc_realtime_lock);

    rtc_kept_time(seconds, nanoseconds);
    rtc_add_step(seconds, nanoseconds);
    if (*seconds < rtc_last_seconds ||
        (*seconds == rtc_last_seconds && *nanoseconds < rtc_last_nanoseconds)) {
        *seconds = rtc_last_seconds;
        *nanoseconds = rtc_last_nanoseconds;
    }
    rtc_last_seconds = *seconds;
    rtc_last_nanoseconds = *nanoseconds;
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
}

/* The last reading of time since boot, which no later one is earlier than. */
static int64_t rtc_mono_seconds;
static int64_t rtc_mono_nanoseconds;
static struct spinlock rtc_mono_lock = SPINLOCK_INIT_NAMED("rtc_mono_lock");

/** The floor is `rtc_realtime`'s, for the same reasons, kept apart from it
 * since setting the clock moves that one and not this. */
void rtc_monotonic(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t flags = kirq_save();
    uint32_t uptime, ticks, phase_ns;

    if (!vclock_read(0, 0, seconds, nanoseconds)) {
        rtc_since_boot(&uptime, &ticks, &phase_ns);
        *seconds = uptime;
        *nanoseconds = (int64_t)ticks * RTC_NANOSECONDS_PER_TICK + phase_ns;
    }
    spin_lock(&rtc_mono_lock);
    if (*seconds < rtc_mono_seconds ||
        (*seconds == rtc_mono_seconds && *nanoseconds < rtc_mono_nanoseconds)) {
        *seconds = rtc_mono_seconds;
        *nanoseconds = rtc_mono_nanoseconds;
    }
    rtc_mono_seconds = *seconds;
    rtc_mono_nanoseconds = *nanoseconds;
    spin_unlock(&rtc_mono_lock);
    kirq_restore(flags);
}

void rtc_coarse_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t flags;
    uint32_t uptime, ticks;
    uint64_t total;

    if (vclock_read(1, 1, seconds, nanoseconds)) {
        return;
    }
    flags = spin_lock_irqsave(&rtc_realtime_lock);
    tick_now(&uptime, &ticks, &total);
    rtc_time_from_anchor(rtc_anchor_seconds, rtc_anchor_uptime, uptime, ticks, seconds,
                         nanoseconds);
    rtc_add_step(seconds, nanoseconds);
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
}

void rtc_coarse_monotonic(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t uptime, ticks;
    uint64_t total;

    if (vclock_read(0, 1, seconds, nanoseconds)) {
        return;
    }
    tick_now(&uptime, &ticks, &total);
    *seconds = uptime;
    *nanoseconds = (int64_t)ticks * RTC_NANOSECONDS_PER_TICK;
}

/**
 * The anchor stays as the boot read it; the difference between the time it
 * keeps now and the time asked for becomes the step every reading adds, so the
 * clock goes on from the new time at the same rate. A reading after a step
 * back may be earlier than one before it, and the boot epoch moves with the
 * step, as Linux's does, so that a process's start time read from it stays
 * where it was against the clock.
 */
void rtc_set_realtime(int64_t seconds, int32_t nanoseconds)
{
    int64_t kept_seconds;
    int64_t kept_nanoseconds;
    uint32_t flags = spin_lock_irqsave(&rtc_realtime_lock);

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
    rtc_offset_locked(&kept_seconds, &nanoseconds);
    vclock_set_offset(kept_seconds, nanoseconds);
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
}
