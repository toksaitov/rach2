/*
 * clock.c: the kernel's real time, kept from one reading of the machine's
 * clock at boot (the anchor, which the platform's clock driver sets through
 * `rtc_anchor`) and the tick since, with the calendar arithmetic the clock
 * drivers and the clock calls share.
 */

#include "rtc.h"
#include "timer.h"

#include "lib/kirq.h"

/* The uptime the timer handler keeps (kernel.c). */
extern volatile uint32_t kernel_uptime_seconds;
extern volatile uint32_t kernel_second_ticks;

#define RTC_SECONDS_PER_DAY    86400
#define RTC_SECONDS_PER_HOUR   3600
#define RTC_SECONDS_PER_MINUTE 60
#define RTC_EPOCH_YEAR         1970

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
int32_t rtc_days_in_month(int32_t year, int32_t month)
{
    static const int32_t days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (month == 2 && rtc_is_leap(year)) {
        return 29;
    }

    return days[month - 1];
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

void rtc_anchor(int64_t seconds, uint32_t uptime)
{
    rtc_anchor_seconds = seconds;
    rtc_anchor_uptime = uptime;
    rtc_anchored = 1;
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

int64_t rtc_seconds_of(const rtc_time_t *t)
{
    int32_t weekday, yearday, year;
    int64_t days = 0;

    for (year = RTC_EPOCH_YEAR; year < t->year; year++) {
        days += rtc_is_leap(year) ? 366 : 365;
    }
    rtc_week_and_year_day(t, &weekday, &yearday);
    return (days + yearday) * 86400 + t->hour * 3600 + t->minute * 60 + t->second;
}

void rtc_time_of_seconds(uint32_t seconds, rtc_time_t *t)
{
    uint32_t days = seconds / 86400u, rest = seconds % 86400u, len;

    t->year = RTC_EPOCH_YEAR;
    while (days >= (len = rtc_is_leap(t->year) ? 366u : 365u)) {
        days -= len;
        t->year++;
    }
    t->month = 1;
    while (days >= (len = (uint32_t)rtc_days_in_month(t->year, t->month))) {
        days -= len;
        t->month++;
    }
    t->day = (int32_t)days + 1;
    t->hour = (int32_t)(rest / 3600u);
    t->minute = (int32_t)(rest / 60u % 60u);
    t->second = (int32_t)(rest % 60u);
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
 * timer's place within the tick; the caller has interrupts off.
 *
 * The tick's interrupt can be pending after the counter wraps. Read its pending bit on both
 * sides of the latch; if it changed, latch again. A pending tick belongs to
 * the new period even before the handler counts it.
 */
static void rtc_since_boot(uint32_t *uptime, uint32_t *ticks, uint32_t *phase_ns)
{
    int pending;

    *uptime = kernel_uptime_seconds;
    *ticks = kernel_second_ticks;
    pending = timer_tick_pending();
    *phase_ns = timer_subtick_ns();
    if (pending != timer_tick_pending()) {
        pending = 1;
        *phase_ns = timer_subtick_ns();
    }
    *ticks += (uint32_t)pending;
    if (*ticks >= RTC_TICKS_PER_SECOND) {
        *ticks -= RTC_TICKS_PER_SECOND;
        (*uptime)++;
    }
}

void rtc_tick_phase(uint32_t *ticks, uint32_t *phase_ns)
{
    uint32_t flags = kirq_save();
    int pending = timer_tick_pending();

    *phase_ns = timer_subtick_ns();
    if (pending != timer_tick_pending()) {
        pending = 1;
        *phase_ns = timer_subtick_ns();
    }
    *ticks = kernel_ticks + (uint32_t)pending;
    kirq_restore(flags);
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
