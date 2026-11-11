/*
 * clock.c: the kernel's clocks: time since boot, from the tick, and real
 * time, that clock plus an offset from one reading of the machine's clock
 * at boot (the anchor, which the platform's clock driver sets through
 * `rtc_anchor`) and from each setting, which runs at the rate the NTP
 * state's tick length gives (ntp.c); with the calendar arithmetic the clock
 * drivers and the clock calls share, and the sequence count by which another
 * processor reads the tick whole.
 */

#include "ntp.h"
#include "rtc.h"
#include "timer.h"
#include "vdso.h"

#include "arch/arch.h"
#include "io/timerfd.h"
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

/* Whether the boot read the clock and anchored real time to it. */
static int rtc_anchored;

/*
 * Real time's course against the monotonic clock (`struct rtc_course`),
 * which the anchor, each setting and each change of the NTP state's tick
 * length make; the tick length its rate was made from; and the rate's
 * inverse, the monotonic nanoseconds a nanosecond of real time takes at
 * it, times 2^31. Each reading of real time is the monotonic clock plus
 * the course's offset there. The last reading is kept too, before which no
 * later one may fall until the clock is set.
 */
static struct rtc_course rtc_course_now;
static int64_t rtc_applied_length = (int64_t)TICK_NS << 32;
static uint64_t rtc_inverse = (uint64_t)1 << 31;
static int64_t rtc_last_seconds;
static int64_t rtc_last_nanoseconds;
/* Protect the course, the NTP state and the real-time floor across
 * processors. */
static struct spinlock rtc_realtime_lock = SPINLOCK_INIT_NAMED("rtc_realtime_lock");
/* The settings of the clock and the changes of its rate since boot, plus
 * one, modulo 2^32 with 0 left out (`rtc_step_count`). */
static volatile uint32_t rtc_steps = 1;

/* The latest time `rtc_set_realtime` takes, Linux's TIME_SETTOD_SEC_MAX
 * (linux/time64.h:43-44): the seconds of 2^63 nanoseconds, less 30 years
 * for the monotonic clock, so that a time in nanoseconds never overflows. */
#define RTC_SETTOD_SEC_MAX (9223372036LL - 30LL * 365 * 24 * 3600)

/** `ns` nanoseconds added to `sec` seconds, as seconds in `*out_sec` and
 * nanoseconds from 0 to 10^9 - 1 in `*out_nsec`, for `ns` of either
 * sign. */
static void rtc_normalize(int64_t sec, int64_t ns, int64_t *out_sec, int32_t *out_nsec)
{
    uint64_t q, rem;

    if (ns >= 0) {
        q = tick_div64((uint64_t)ns, 1000000000u, &rem);
        sec += (int64_t)q;
    } else {
        q = tick_div64((uint64_t)-ns, 1000000000u, &rem);
        sec -= (int64_t)q;
        if (rem != 0) {
            rem = 1000000000u - rem;
            sec--;
        }
    }
    *out_sec = sec;
    *out_nsec = (int32_t)rem;
}

/** `a` times `b`, all 128 bits, in `*hi` and `*lo`, from the four products
 * of their halves, since C89 has no wider type. */
static void rtc_mul128(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);

    *lo = (mid << 32) | (p00 & 0xFFFFFFFFu);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

/**
 * The slew is the time since the course's start times its rate, both of
 * either sign, in 2^-RTC_RATE_SHIFT nanoseconds, plus the course's own
 * fraction; its floor in nanoseconds goes to the offset and the rest is
 * the fraction, so that a course started from this one's offset and
 * fraction at any time gives the same offsets after it.
 */
void rtc_course_at(const struct rtc_course *c, int64_t sec, int64_t nsec, int64_t *off_sec,
                   int32_t *off_nsec, uint64_t *frac)
{
    const uint64_t one = (uint64_t)1 << RTC_RATE_SHIFT;
    int64_t e = (sec - c->mono_sec) * 1000000000 + (nsec - c->mono_nsec);
    uint64_t hi, lo, whole, part;
    int64_t ns;

    rtc_mul128(e < 0 ? (uint64_t)-e : (uint64_t)e,
               c->rate < 0 ? (uint64_t)-c->rate : (uint64_t)c->rate, &hi, &lo);
    whole = (hi << (64 - RTC_RATE_SHIFT)) | (lo >> RTC_RATE_SHIFT);
    part = lo & (one - 1u);
    if ((e < 0) == (c->rate < 0)) {
        part += c->frac;
        ns = (int64_t)(whole + (part >> RTC_RATE_SHIFT));
        *frac = part & (one - 1u);
    } else if (part <= c->frac) {
        ns = -(int64_t)whole;
        *frac = c->frac - part;
    } else {
        ns = -(int64_t)whole - 1;
        *frac = c->frac + one - part;
    }
    rtc_normalize(c->off_sec, c->off_nsec + ns, off_sec, off_nsec);
}

/** Start the course again at the monotonic time `sec` and `nsec`, from its
 * offset and fraction there, with the lock held: the offsets it gives are
 * the same, and the time since its start, which every reading multiplies,
 * stays short. */
static void rtc_course_rebase(int64_t sec, int64_t nsec)
{
    struct rtc_course *c = &rtc_course_now;
    int64_t off_sec;
    int32_t off_nsec;
    uint64_t frac;

    rtc_course_at(c, sec, nsec, &off_sec, &off_nsec, &frac);
    rtc_normalize(sec, nsec, &c->mono_sec, &c->mono_nsec);
    c->off_sec = off_sec;
    c->off_nsec = off_nsec;
    c->frac = frac;
}

/** Move the step count on, past 0. */
static void rtc_count_step(void)
{
    rtc_steps = rtc_steps % 0xffffffffu + 1u; /* 1 to 2^32-1, never 0 */
}

/**
 * Take the NTP state's tick length, with the lock held and the monotonic
 * clock at `sec` and `nsec`, when it changed: the course starts again
 * there at the rate the length gives, what a tick of TICK_NS nanoseconds
 * gains or loses, so that real time runs on from where it is at the new
 * rate, as Linux's timekeeping_adjust steers the clock to the tick length
 * (kernel/time/timekeeping.c:2054-2107); the vDSO's clock takes the course,
 * and the step count moves on, so that every wait for a time of real time
 * counts its tick again at the new rate. Returns 1 when the length
 * changed, for the caller to have the timers counted again
 * (`timerfd_clock_was_set`) once the lock is let go.
 */
static int rtc_take_rate(int64_t sec, int64_t nsec)
{
    int64_t length = ntp_tick_length();
    int64_t d = length - ((int64_t)TICK_NS << 32);
    uint64_t rate;

    if (length == rtc_applied_length) {
        return 0;
    }
    rtc_applied_length = length;
    rtc_course_rebase(sec, nsec);
    /* under 2^63: a tick is at most 23% longer or shorter, under 2^50 */
    rate = tick_div64((d < 0 ? (uint64_t)-d : (uint64_t)d) << (RTC_RATE_SHIFT - 32), TICK_NS,
                      (uint64_t *)0);
    rtc_course_now.rate = d < 0 ? -(int64_t)rate : (int64_t)rate;
    rtc_inverse = tick_div64((uint64_t)1 << 62,
                             (uint64_t)(((int64_t)1 << RTC_RATE_SHIFT) + rtc_course_now.rate) >> 14,
                             (uint64_t *)0);
    vclock_set_course(&rtc_course_now, 0);
    rtc_count_step();
    return 1;
}

void rtc_anchor(int64_t seconds, uint32_t uptime)
{
    uint32_t flags = spin_lock_irqsave(&rtc_realtime_lock);

    rtc_anchored = 1;
    rtc_course_now.off_sec += seconds - (int64_t)uptime;
    vclock_set_course(&rtc_course_now, 1);
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
}

void rtc_get_course(struct rtc_course *c)
{
    uint32_t flags = spin_lock_irqsave(&rtc_realtime_lock);

    *c = rtc_course_now;
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
    int64_t off_sec, ns;
    int32_t off_nsec;
    uint64_t frac;

    ns = (int64_t)kernel_second_ticks * TICK_NS;
    rtc_course_at(&rtc_course_now, kernel_uptime_seconds, ns, &off_sec, &off_nsec, &frac);
    rtc_normalize((int64_t)kernel_uptime_seconds + off_sec, ns + off_nsec, seconds, &off_nsec);
    *microseconds = off_nsec / 1000;
}

int rtc_is_anchored(void)
{
    return rtc_anchored;
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

/** The monotonic clock now, to the timer's place within the tick, or by
 * the clock from the TSC while it runs, without `rtc_monotonic`'s floor,
 * with nanoseconds that may pass a second; the caller has interrupts off. */
static void rtc_mono_now(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t uptime, ticks, phase_ns;

    if (vclock_read(0, 0, seconds, nanoseconds)) {
        return;
    }
    rtc_since_boot(&uptime, &ticks, &phase_ns);
    *seconds = uptime;
    *nanoseconds = (int64_t)ticks * RTC_NANOSECONDS_PER_TICK + phase_ns;
}

/** Real time at the monotonic time `msec` and `mnsec`, by the course, with
 * the lock held, in `*seconds` and `*nanoseconds`, under a second. */
static void rtc_real_of_mono(int64_t msec, int64_t mnsec, int64_t *seconds, int64_t *nanoseconds)
{
    int64_t off_sec;
    int32_t off_nsec;
    uint64_t frac;

    rtc_course_at(&rtc_course_now, msec, mnsec, &off_sec, &off_nsec, &frac);
    rtc_normalize(msec + off_sec, mnsec + off_nsec, seconds, &off_nsec);
    *nanoseconds = off_nsec;
}

/** The monotonic clock and real time now, with the lock held: real time
 * from the TSC's clock while it runs, the reading the vDSO gives, and by
 * the course otherwise. */
static void rtc_now_locked(int64_t *msec, int64_t *mnsec, int64_t *seconds, int64_t *nanoseconds)
{
    rtc_mono_now(msec, mnsec);
    if (!vclock_read(1, 0, seconds, nanoseconds)) {
        rtc_real_of_mono(*msec, *mnsec, seconds, nanoseconds);
    }
}

void rtc_offset(int64_t *seconds, int32_t *nanoseconds)
{
    uint32_t flags = spin_lock_irqsave(&rtc_realtime_lock);
    int64_t msec, mnsec;
    uint64_t frac;

    rtc_mono_now(&msec, &mnsec);
    rtc_course_at(&rtc_course_now, msec, mnsec, seconds, nanoseconds, &frac);
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
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

/**
 * The last reading is kept as a floor, since a reading can fall back:
 * without a reference counter a stretch with interrupts off longer than a
 * period loses periods the tick does not count, and a faulty latch of the
 * counter reads zero.
 */
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t flags = spin_lock_irqsave(&rtc_realtime_lock);
    int64_t msec, mnsec;

    rtc_now_locked(&msec, &mnsec, seconds, nanoseconds);
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

    rtc_mono_now(seconds, nanoseconds);
    while (*nanoseconds >= 1000000000) {
        *nanoseconds -= 1000000000;
        (*seconds)++;
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

int64_t rtc_monotonic_ns(void)
{
    int64_t sec, nsec;

    rtc_monotonic(&sec, &nsec);
    return sec * 1000000000ll + nsec;
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
    rtc_real_of_mono(uptime, (int64_t)ticks * RTC_NANOSECONDS_PER_TICK, seconds, nanoseconds);
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
 * The step, with the lock held and the monotonic clock at `msec` and
 * `mnsec`: real time becomes `seconds` and `nanoseconds` there, and its
 * floor that time, unless `valid` is clear or the time is before the
 * monotonic clock, which Linux refuses (kernel/time/timekeeping.c:1361-
 * 1364, 1410-1414); and either way the NTP state forgets its offsets and
 * the course takes its rate, the vDSO's clock takes the course, and the
 * step count moves on, as Linux updates the clock with TK_CLEAR_NTP and
 * calls clock_was_set on both paths (kernel/time/timekeeping.c:1369-1376,
 * 1419-1426). 0, or -1 for a time refused.
 */
static int rtc_step_locked(int64_t msec, int64_t mnsec, int64_t seconds, int32_t nanoseconds,
                           int valid)
{
    struct rtc_course *c = &rtc_course_now;
    int32_t ns;
    int rc = -1;

    rtc_normalize(msec, mnsec, &msec, &ns);
    if (valid && (seconds > msec || (seconds == msec && nanoseconds >= ns))) {
        c->mono_sec = msec;
        c->mono_nsec = ns;
        rtc_normalize(seconds - msec, (int64_t)nanoseconds - ns, &c->off_sec, &c->off_nsec);
        c->frac = 0;
        rtc_last_seconds = seconds;
        rtc_last_nanoseconds = nanoseconds;
        rc = 0;
    }
    ntp_clear();
    (void)rtc_take_rate(msec, ns);
    vclock_set_course(c, 1);
    rtc_count_step();
    return rc;
}

/**
 * The difference between the course's offset now and the one asked for
 * becomes its offset, so the clock goes on from the new time at the rate it
 * had. A reading after a step back may be earlier than one before it, and
 * the boot epoch moves with the step, as Linux's does, so that a process's
 * start time read from it stays where it was against the clock.
 */
int rtc_set_realtime(int64_t seconds, int32_t nanoseconds)
{
    uint32_t flags;
    int64_t msec, mnsec;
    int rc;

    if (seconds < 0 || seconds >= RTC_SETTOD_SEC_MAX || nanoseconds < 0 ||
        nanoseconds > 999999999) {
        return -1;
    }
    flags = spin_lock_irqsave(&rtc_realtime_lock);
    rtc_mono_now(&msec, &mnsec);
    rc = rtc_step_locked(msec, mnsec, seconds, nanoseconds, 1);
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
    timerfd_clock_was_set(1);
    return rc;
}

int rtc_shift_realtime(int64_t seconds, int32_t nanoseconds)
{
    uint32_t flags = spin_lock_irqsave(&rtc_realtime_lock);
    int64_t msec, mnsec, now, now_ns;
    int32_t nsec;
    int valid;
    int rc;

    rtc_now_locked(&msec, &mnsec, &now, &now_ns);
    valid = seconds > -RTC_SETTOD_SEC_MAX && seconds < RTC_SETTOD_SEC_MAX;
    rtc_normalize(valid ? now + seconds : 0, now_ns + nanoseconds, &now, &nsec);
    valid = valid && now >= 0 && now < RTC_SETTOD_SEC_MAX;
    rc = rtc_step_locked(msec, mnsec, now, nsec, valid);
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
    timerfd_clock_was_set(1);
    return rc;
}

/**
 * Linux reads the time before it takes its lock (kernel/time/timekeeping.c:
 * 2504-2509), so the time reported is the one before the modes changed
 * anything.
 */
int32_t rtc_adjtimex(struct ktimex *tx)
{
    uint32_t flags;
    int64_t sec, nsec, msec, mnsec;
    int32_t rc;
    int changed;

    rtc_realtime(&sec, &nsec);
    flags = spin_lock_irqsave(&rtc_realtime_lock);
    rc = ntp_adjtimex(tx, sec);
    rtc_mono_now(&msec, &mnsec);
    changed = rtc_take_rate(msec, mnsec);
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
    if (changed) {
        timerfd_clock_was_set(0);
    }
    tx->sec = sec;
    tx->usec = (tx->status & STA_NANO) ? nsec : (int64_t)((int32_t)nsec / 1000);
    return rc;
}

/**
 * A course whose rate is not 0 starts again at each second, though its
 * rate stays, so that the time since its start, which every reading
 * multiplies, stays near a second.
 */
void rtc_seconds(uint32_t n)
{
    uint32_t flags = spin_lock_irqsave(&rtc_realtime_lock);
    int64_t msec, mnsec;
    int changed;

    while (n-- > 0) {
        ntp_second_overflow();
    }
    rtc_mono_now(&msec, &mnsec);
    changed = rtc_take_rate(msec, mnsec);
    if (!changed && rtc_course_now.rate != 0) {
        rtc_course_rebase(msec, mnsec);
    }
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
    if (changed) {
        timerfd_clock_was_set(0);
    }
}

/** `ns` nanoseconds of real time as nanoseconds of the monotonic clock at
 * the course's rate (`rtc_inverse`), with the lock held; 0 or less is
 * itself, and so is 2^62 or more, over 146 years, which the product could
 * carry past 2^63. */
static int64_t rtc_to_mono_locked(int64_t ns)
{
    uint64_t hi, lo;

    if (ns <= 0 || ns >= ((int64_t)1 << 62)) {
        return ns;
    }
    rtc_mul128((uint64_t)ns, rtc_inverse, &hi, &lo);
    return (int64_t)((hi << 33) | (lo >> 31));
}

int64_t rtc_real_to_mono_ns(int64_t ns)
{
    uint32_t flags = spin_lock_irqsave(&rtc_realtime_lock);

    ns = rtc_to_mono_locked(ns);
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
    return ns;
}

uint64_t rtc_real_span(int64_t sec, uint32_t nsec)
{
    int64_t ns;

    if (sec < 0) {
        return 0;
    }
    if (sec >= ((int64_t)1 << 32)) {
        return tick_span(sec, nsec);
    }
    ns = rtc_real_to_mono_ns(sec * 1000000000 + nsec);
    return tick_div64((uint64_t)ns + TICK_NS - 1u, TICK_NS, (uint64_t *)0);
}

void rtc_mono_of_real(int64_t sec, int32_t nsec, int64_t *mono_sec, int32_t *mono_nsec)
{
    uint32_t flags = spin_lock_irqsave(&rtc_realtime_lock);
    int64_t msec, mnsec, now, now_ns, d;

    rtc_now_locked(&msec, &mnsec, &now, &now_ns);
    d = sec - now;
    if (d > -((int64_t)1 << 32) && d < ((int64_t)1 << 32)) {
        d = rtc_to_mono_locked(d * 1000000000 + (nsec - now_ns));
        rtc_normalize(msec, mnsec + d, mono_sec, mono_nsec);
    } else {
        rtc_normalize(msec + d, mnsec + (nsec - now_ns), mono_sec, mono_nsec);
    }
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
}

uint32_t rtc_step_count(void)
{
    return rtc_steps;
}
