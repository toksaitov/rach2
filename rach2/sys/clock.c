/*
 * clock.c: the kernel's clocks: the raw clock, the timer's ticks since boot;
 * the monotonic clock, each of those ticks the length the NTP state gives it
 * (ntp.c), whose ticks are the kernel's tick count; and real time, the
 * monotonic clock plus an offset from one reading of the machine's clock at
 * boot (the anchor, which the platform's clock driver sets through
 * `rtc_anchor`), which only a setting or a leap second moves; with the
 * calendar arithmetic the clock drivers and the clock calls share, and the
 * sequence count by which another processor reads the tick whole.
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

/* A tick of the monotonic clock in 2^-32 nanoseconds, the unit of the NTP
 * state's tick length. */
#define RTC_TICK_ONE ((uint64_t)TICK_NS << 32)

/*
 * The clocks since boot, which the timer pass changes within the tick's
 * sequence count (`rtc_tick_advance`): the raw ticks the timer has counted;
 * how far the monotonic clock is past the edge of its tick `kernel_ticks`,
 * in 2^-32 nanoseconds, below RTC_TICK_ONE; the length each raw tick adds to
 * the monotonic clock, the NTP state's tick length as the last pass took it;
 * and the rate that length gives, the nanoseconds the monotonic clock gains
 * in a nanosecond of the raw clock, times 2^RTC_RATE_SHIFT, negative while
 * it loses them.
 */
static volatile uint64_t rtc_raw_ticks;
static volatile uint64_t rtc_part;
static volatile int64_t rtc_applied = (int64_t)RTC_TICK_ONE;
static volatile int64_t rtc_rate;

/*
 * Real time less the monotonic clock, Linux's offs_real, the negated
 * wall_to_monotonic (kernel/time/timekeeping.c:151-164), in seconds and in
 * nanoseconds from 0 to 10^9 - 1, which the anchor, each setting and each
 * leap second move and nothing else; and the last reading of real time,
 * before which no later one may fall until the clock is set or a leap
 * second is inserted.
 */
static int64_t rtc_wall_sec;
static volatile int32_t rtc_wall_nsec;
static int64_t rtc_last_seconds;
static int64_t rtc_last_nanoseconds;
/* Protect the offset, the NTP state and the real-time floor across
 * processors. */
static struct spinlock rtc_realtime_lock = SPINLOCK_INIT_NAMED("rtc_realtime_lock");
/* The steps of real time since boot, its settings and leap seconds, plus
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

uint64_t rtc_slewed(uint64_t ns, int64_t rate)
{
    uint64_t hi, lo, gain;

    if (rate == 0) {
        return ns;
    }
    rtc_mul128(ns, rate < 0 ? (uint64_t)-rate : (uint64_t)rate, &hi, &lo);
    gain = (hi << (64 - RTC_RATE_SHIFT)) | (lo >> RTC_RATE_SHIFT);
    return rate < 0 ? ns - gain : ns + gain;
}

/** The rate (`rtc_rate`) of the tick length `length`, what a tick of
 * TICK_NS nanoseconds gains or loses in each of its nanoseconds. */
static int64_t rtc_rate_of(int64_t length)
{
    int64_t d = length - (int64_t)RTC_TICK_ONE;
    uint64_t rate;

    /* under 2^63: a tick is at most 23% longer or shorter, under 2^50 */
    rate = tick_div64((d < 0 ? (uint64_t)-d : (uint64_t)d) << (RTC_RATE_SHIFT - 32), TICK_NS,
                      (uint64_t *)0);
    return d < 0 ? -(int64_t)rate : (int64_t)rate;
}

/** Move the step count on, past 0. */
static void rtc_count_step(void)
{
    rtc_steps = rtc_steps % 0xffffffffu + 1u; /* 1 to 2^32-1, never 0 */
}

int64_t rtc_tick_length(void)
{
    uint32_t flags = spin_lock_irqsave(&rtc_realtime_lock);
    int64_t length = ntp_tick_length();

    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
    return length;
}

/** The second of real time the monotonic clock is in at the last pass,
 * modulo 2^32, with real time `wall_nsec` nanoseconds past the monotonic
 * clock's second; the caller writes the tick. */
static uint32_t rtc_real_second(int32_t wall_nsec)
{
    uint32_t ns = kernel_second_ticks * RTC_NANOSECONDS_PER_TICK + (uint32_t)(rtc_part >> 32) +
                  (uint32_t)wall_nsec;

    return kernel_uptime_seconds + (ns >= 1000000000u ? 1u : 0u);
}

/**
 * Each raw tick adds the tick length to the monotonic clock, as Linux's
 * logarithmic_accumulation adds xtime_interval for each cycle_interval
 * (kernel/time/timekeeping.c:2164-2193), and the whole ticks of the sum
 * move `kernel_ticks` and the uptime; a length from the NTP state is taken
 * only here, for the ticks after this one, as Linux's timekeeping_adjust
 * takes it at the end of timekeeping_advance (timekeeping.c:2054-2107,
 * 2245-2246), so that the coarse clocks, the monotonic clock at the last
 * tick, never go back. The seconds of real time are counted at real time's
 * own second, which is what Linux's accumulate_nsecs_to_secs counts
 * (timekeeping.c:2115-2153); the offset's nanoseconds are read once, so a
 * setting between two passes moves the count by at most one.
 */
uint32_t rtc_tick_advance(uint32_t raw, int64_t length)
{
    int32_t wall_nsec = rtc_wall_nsec;
    uint32_t before = rtc_real_second(wall_nsec);

    rtc_raw_ticks += raw;
    while (raw-- > 0) {
        rtc_part += (uint64_t)rtc_applied;
        while (rtc_part >= RTC_TICK_ONE) {
            rtc_part -= RTC_TICK_ONE;
            ++kernel_ticks;
            if (++kernel_second_ticks >= RTC_TICKS_PER_SECOND) {
                kernel_second_ticks = 0;
                ++kernel_uptime_seconds;
            }
        }
    }
    if (length != rtc_applied) {
        rtc_applied = length;
        rtc_rate = rtc_rate_of(length);
    }
    return rtc_real_second(wall_nsec) - before;
}

int64_t rtc_mono_rate(void)
{
    uint32_t flags = kirq_save();
    uint32_t seq;
    int64_t rate;

    do {
        seq = tick_read_begin();
        rate = rtc_rate;
    } while (tick_read_retry(seq));
    kirq_restore(flags);
    return rate;
}

/* One reading of the clocks since boot from the tick (`rtc_tick_read`). */
struct rtc_reading {
    uint64_t ticks;        /* kernel_ticks */
    uint64_t raw_ticks;    /* rtc_raw_ticks */
    uint32_t uptime;       /* kernel_uptime_seconds */
    uint32_t second_ticks; /* kernel_second_ticks */
    uint64_t mono_past;    /* the monotonic clock's ns past `ticks`' edge */
    uint64_t raw_past;     /* the raw clock's ns past `raw_ticks`' edge */
};

/**
 * Read the clocks since boot from the tick into `*r`, to the timer's place
 * within it, with interrupts off. The ticks the handler has not counted yet
 * belong to the time now (`timer_phase`): they and the place within the
 * tick are raw time, which the rate the last pass took (`rtc_rate`) makes
 * the monotonic clock's.
 */
static void rtc_tick_read(struct rtc_reading *r)
{
    uint32_t seq, phase_ns;
    uint64_t part;
    int64_t rate;
    int pending;

    do {
        seq = tick_read_begin();
        r->ticks = kernel_ticks;
        r->raw_ticks = rtc_raw_ticks;
        r->uptime = kernel_uptime_seconds;
        r->second_ticks = kernel_second_ticks;
        part = rtc_part;
        rate = rtc_rate;
        timer_phase(&pending, &phase_ns);
    } while (tick_read_retry(seq));
    r->raw_past = (uint64_t)(uint32_t)pending * RTC_NANOSECONDS_PER_TICK + phase_ns;
    r->mono_past = (part >> 32) + rtc_slewed(r->raw_past, rate);
}

/** The monotonic clock at the last pass, with interrupts off: the uptime's
 * seconds in `*seconds` and the nanoseconds past them in `*nanoseconds`. */
static void rtc_coarse_since_boot(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t seq, uptime, ticks;
    uint64_t part;

    do {
        seq = tick_read_begin();
        uptime = kernel_uptime_seconds;
        ticks = kernel_second_ticks;
        part = rtc_part;
    } while (tick_read_retry(seq));
    *seconds = uptime;
    *nanoseconds = (int64_t)ticks * RTC_NANOSECONDS_PER_TICK + (int64_t)(part >> 32);
}

void rtc_anchor(int64_t seconds, uint32_t uptime)
{
    uint32_t flags = spin_lock_irqsave(&rtc_realtime_lock);

    rtc_anchored = 1;
    rtc_wall_sec += seconds - (int64_t)uptime;
    vclock_set_wall(rtc_wall_sec, rtc_wall_nsec);
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
    int64_t ns = (int64_t)kernel_second_ticks * RTC_NANOSECONDS_PER_TICK +
                 (int64_t)(rtc_part >> 32) + rtc_wall_nsec;
    int32_t nsec;

    rtc_normalize((int64_t)kernel_uptime_seconds + rtc_wall_sec, ns, seconds, &nsec);
    *microseconds = nsec / 1000;
}

int rtc_is_anchored(void)
{
    return rtc_anchored;
}

void rtc_tick_phase(uint64_t *ticks, uint32_t *phase_ns)
{
    uint32_t flags = kirq_save();
    struct rtc_reading r;
    uint64_t rem;

    rtc_tick_read(&r);
    *ticks = r.ticks + tick_div64(r.mono_past, RTC_NANOSECONDS_PER_TICK, &rem);
    *phase_ns = (uint32_t)rem;
    kirq_restore(flags);
}

uint64_t rtc_ticks_now(void)
{
    uint64_t ticks;
    uint32_t phase_ns;

    rtc_tick_phase(&ticks, &phase_ns);
    return ticks;
}

void rtc_tick_both(uint64_t *mono_ns, uint64_t *raw_ns)
{
    uint32_t flags = kirq_save();
    struct rtc_reading r;

    rtc_tick_read(&r);
    *mono_ns = r.ticks * RTC_NANOSECONDS_PER_TICK + r.mono_past;
    *raw_ns = r.raw_ticks * RTC_NANOSECONDS_PER_TICK + r.raw_past;
    kirq_restore(flags);
}

/** The monotonic clock now, to the timer's place within the tick, or by
 * the clock from the TSC while it runs, without `rtc_monotonic`'s floor,
 * with nanoseconds that may pass a second; the caller has interrupts off. */
static void rtc_mono_now(int64_t *seconds, int64_t *nanoseconds)
{
    struct rtc_reading r;

    if (vclock_read(VCLOCK_READ_MONO, 0, seconds, nanoseconds)) {
        return;
    }
    rtc_tick_read(&r);
    *seconds = r.uptime;
    *nanoseconds = (int64_t)r.second_ticks * RTC_NANOSECONDS_PER_TICK + (int64_t)r.mono_past;
}

/** Real time at the monotonic time `msec` and `mnsec`, whose nanoseconds
 * may pass a second, with the lock held, in `*seconds` and `*nanoseconds`,
 * under a second. */
static void rtc_real_of_mono(int64_t msec, int64_t mnsec, int64_t *seconds, int64_t *nanoseconds)
{
    int32_t nsec;

    rtc_normalize(msec + rtc_wall_sec, mnsec + rtc_wall_nsec, seconds, &nsec);
    *nanoseconds = nsec;
}

void rtc_offset(int64_t *seconds, int32_t *nanoseconds)
{
    uint32_t flags = spin_lock_irqsave(&rtc_realtime_lock);

    *seconds = rtc_wall_sec;
    *nanoseconds = rtc_wall_nsec;
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
}

/**
 * The anchor is the moment of the read, some way into the boot; this is that
 * reading less the uptime it was taken at, uptime zero, which is what `btime`
 * means, Linux's getboottime64 (kernel/time/timekeeping.c:2296-2301), which
 * only a setting or a leap second moves.
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

    rtc_mono_now(&msec, &mnsec);
    rtc_real_of_mono(msec, mnsec, seconds, nanoseconds);
    if (*seconds < rtc_last_seconds ||
        (*seconds == rtc_last_seconds && *nanoseconds < rtc_last_nanoseconds)) {
        *seconds = rtc_last_seconds;
        *nanoseconds = rtc_last_nanoseconds;
    }
    rtc_last_seconds = *seconds;
    rtc_last_nanoseconds = *nanoseconds;
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
}

/* The last readings of the monotonic and raw clocks, which no later one is
 * earlier than. */
static int64_t rtc_mono_seconds;
static int64_t rtc_mono_nanoseconds;
static int64_t rtc_raw_seconds;
static int64_t rtc_raw_nanoseconds;
static struct spinlock rtc_mono_lock = SPINLOCK_INIT_NAMED("rtc_mono_lock");

/** Hold `*seconds` and `*nanoseconds`, whose nanoseconds may pass a second,
 * to the floor `*floor_sec` and `*floor_nsec` and make them the floor, with
 * `rtc_mono_lock` held. */
static void rtc_floor(int64_t *seconds, int64_t *nanoseconds, int64_t *floor_sec,
                      int64_t *floor_nsec)
{
    while (*nanoseconds >= 1000000000) {
        *nanoseconds -= 1000000000;
        (*seconds)++;
    }
    if (*seconds < *floor_sec || (*seconds == *floor_sec && *nanoseconds < *floor_nsec)) {
        *seconds = *floor_sec;
        *nanoseconds = *floor_nsec;
    }
    *floor_sec = *seconds;
    *floor_nsec = *nanoseconds;
}

/** The floor is `rtc_realtime`'s, for the same reasons, kept apart from it
 * since setting the clock moves that one and not this. */
void rtc_monotonic(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t flags = kirq_save();

    rtc_mono_now(seconds, nanoseconds);
    spin_lock(&rtc_mono_lock);
    rtc_floor(seconds, nanoseconds, &rtc_mono_seconds, &rtc_mono_nanoseconds);
    spin_unlock(&rtc_mono_lock);
    kirq_restore(flags);
}

/** The raw clock is the timer's ticks and its place within the tick, or the
 * TSC's raw clock while that runs, with a floor of its own. */
void rtc_raw_monotonic(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t flags = kirq_save();
    struct rtc_reading r;
    uint32_t rem;

    if (!vclock_read(VCLOCK_READ_RAW, 0, seconds, nanoseconds)) {
        rtc_tick_read(&r);
        *seconds = (int64_t)tick_div(r.raw_ticks, RTC_TICKS_PER_SECOND, &rem);
        *nanoseconds = (int64_t)rem * RTC_NANOSECONDS_PER_TICK + (int64_t)r.raw_past;
    }
    spin_lock(&rtc_mono_lock);
    rtc_floor(seconds, nanoseconds, &rtc_raw_seconds, &rtc_raw_nanoseconds);
    spin_unlock(&rtc_mono_lock);
    kirq_restore(flags);
}

/** The time `sec` seconds and `nsec` nanoseconds, a floor's, in
 * nanoseconds, 0 for one before 0. */
static uint64_t rtc_floor_ns(int64_t sec, int64_t nsec)
{
    int64_t ns = sec * 1000000000ll + nsec;

    return ns > 0 ? (uint64_t)ns : 0u;
}

/**
 * Real time's lock and the floors' are held across `change`, so that no
 * reading of real time is under way, and none that could hold a floor below
 * what `change` gives; neither lock's holder waits for the vDSO's page, as
 * real time's does in `rtc_realtime` when it is not the holder here.
 */
void rtc_vclock_change(void (*change)(uint64_t *mono_ns, uint64_t *raw_ns))
{
    uint32_t flags = spin_lock_irqsave(&rtc_realtime_lock);
    uint64_t mono, raw;
    int64_t sec, nsec, real_sec, real_nsec, real_mono;
    int32_t ns;

    spin_lock(&rtc_mono_lock);
    mono = rtc_floor_ns(rtc_mono_seconds, rtc_mono_nanoseconds);
    real_mono =
        (rtc_last_seconds - rtc_wall_sec) * 1000000000ll + rtc_last_nanoseconds - rtc_wall_nsec;
    if (real_mono > 0 && (uint64_t)real_mono > mono) {
        mono = (uint64_t)real_mono;
    }
    raw = rtc_floor_ns(rtc_raw_seconds, rtc_raw_nanoseconds);
    change(&mono, &raw);
    rtc_normalize(0, (int64_t)mono, &sec, &ns);
    nsec = ns;
    rtc_floor(&sec, &nsec, &rtc_mono_seconds, &rtc_mono_nanoseconds);
    rtc_normalize(0, (int64_t)raw, &sec, &ns);
    nsec = ns;
    rtc_floor(&sec, &nsec, &rtc_raw_seconds, &rtc_raw_nanoseconds);
    spin_unlock(&rtc_mono_lock);
    rtc_real_of_mono(0, (int64_t)mono, &real_sec, &real_nsec);
    if (real_sec > rtc_last_seconds ||
        (real_sec == rtc_last_seconds && real_nsec > rtc_last_nanoseconds)) {
        rtc_last_seconds = real_sec;
        rtc_last_nanoseconds = real_nsec;
    }
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
}

int64_t rtc_monotonic_ns(void)
{
    int64_t sec, nsec;

    rtc_monotonic(&sec, &nsec);
    return sec * 1000000000ll + nsec;
}

void rtc_coarse_monotonic(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t flags;
    int32_t nsec;

    if (vclock_read(VCLOCK_READ_MONO, 1, seconds, nanoseconds)) {
        return;
    }
    flags = kirq_save();
    rtc_coarse_since_boot(seconds, nanoseconds);
    kirq_restore(flags);
    rtc_normalize(*seconds, *nanoseconds, seconds, &nsec);
    *nanoseconds = nsec;
}

void rtc_coarse_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    uint32_t flags;
    int64_t msec, mnsec;

    if (vclock_read(VCLOCK_READ_REAL, 1, seconds, nanoseconds)) {
        return;
    }
    flags = spin_lock_irqsave(&rtc_realtime_lock);
    rtc_coarse_since_boot(&msec, &mnsec);
    rtc_real_of_mono(msec, mnsec, seconds, nanoseconds);
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
}

/**
 * The step, with the lock held and the monotonic clock at `msec` and
 * `mnsec`: real time becomes `seconds` and `nanoseconds` there, and its
 * floor that time, unless `valid` is clear or the time is before the
 * monotonic clock, which Linux refuses (kernel/time/timekeeping.c:1361-
 * 1364, 1410-1414); and either way the NTP state forgets its offsets, which
 * the next tick takes, and the vDSO's clock takes the offset, as Linux
 * updates the clock with TK_CLEAR_NTP on both paths (kernel/time/
 * timekeeping.c:1369-1376, 1419-1426). The step count moves on only when
 * the offset changed, so that a refused time, or a step of nothing, counts
 * the sleeps of real time again for no step. 0, or -1 for a time refused.
 */
static int rtc_step_locked(int64_t msec, int64_t mnsec, int64_t seconds, int32_t nanoseconds,
                           int valid)
{
    int64_t wall_sec;
    int32_t ns, wall_nsec;
    int rc = -1;

    rtc_normalize(msec, mnsec, &msec, &ns);
    if (valid && (seconds > msec || (seconds == msec && nanoseconds >= ns))) {
        rtc_normalize(seconds - msec, (int64_t)nanoseconds - ns, &wall_sec, &wall_nsec);
        if (wall_sec != rtc_wall_sec || wall_nsec != rtc_wall_nsec) {
            rtc_wall_sec = wall_sec;
            rtc_wall_nsec = wall_nsec;
            rtc_count_step();
        }
        rtc_last_seconds = seconds;
        rtc_last_nanoseconds = nanoseconds;
        rc = 0;
    }
    ntp_clear();
    vclock_set_wall(rtc_wall_sec, rtc_wall_nsec);
    return rc;
}

/**
 * The offset becomes the time asked for less the monotonic clock now, so
 * the clock goes on from the new time at the rate it had. A reading after
 * a step back may be earlier than one before it, and the boot epoch moves
 * with the step, as Linux's does, so that a process's start time read from
 * it stays where it was against the clock.
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
    timerfd_clock_was_set();
    return rc;
}

int rtc_shift_realtime(int64_t seconds, int32_t nanoseconds)
{
    uint32_t flags = spin_lock_irqsave(&rtc_realtime_lock);
    int64_t msec, mnsec, now, now_ns;
    int32_t nsec;
    int valid;
    int rc;

    rtc_mono_now(&msec, &mnsec);
    rtc_real_of_mono(msec, mnsec, &now, &now_ns);
    valid = seconds > -RTC_SETTOD_SEC_MAX && seconds < RTC_SETTOD_SEC_MAX;
    rtc_normalize(valid ? now + seconds : 0, now_ns + nanoseconds, &now, &nsec);
    valid = valid && now >= 0 && now < RTC_SETTOD_SEC_MAX;
    rc = rtc_step_locked(msec, mnsec, now, nsec, valid);
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
    timerfd_clock_was_set();
    return rc;
}

/**
 * Linux reads the time before it takes its lock (kernel/time/timekeeping.c:
 * 2504-2509), so the time reported is the one before the modes changed
 * anything. The modes count their interval and reference time from the
 * clock's seconds at the last pass, read under its lock, as Linux's
 * __ktime_get_real_seconds (ntp.c:321-325, 708-709, timekeeping.c:944-953);
 * the next tick takes the tick length they give (`rtc_tick_advance`).
 */
int32_t rtc_adjtimex(struct ktimex *tx)
{
    uint32_t flags;
    int64_t sec, nsec, clock_sec, clock_nsec;
    int32_t rc;

    rtc_realtime(&sec, &nsec);
    flags = spin_lock_irqsave(&rtc_realtime_lock);
    rtc_coarse_since_boot(&clock_sec, &clock_nsec);
    rtc_real_of_mono(clock_sec, clock_nsec, &clock_sec, &clock_nsec);
    tx->sec = sec;
    rc = ntp_adjtimex(tx, clock_sec);
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
    tx->usec = (tx->status & STA_NANO) ? nsec : (int64_t)((int32_t)nsec / 1000);
    return rc;
}

/**
 * The seconds are those of real time at the last pass, the latest last, as
 * Linux's accumulate_nsecs_to_secs gives second_overflow the seconds just
 * begun (kernel/time/timekeeping.c:2121-2150); the tick length they leave
 * is taken for the ticks after this one, within the tick's sequence count,
 * as Linux's timekeeping_adjust takes it in the same timekeeping_advance
 * (timekeeping.c:2245-2251), which the caller, the timer pass, may write
 * since it holds `clock_pass_lock` with interrupts off; a leap second moves
 * the offset, and the seconds after it, by its step, and the floor with it, so
 * that real time can go back the second inserted, and the timers of real
 * time are told, as Linux's clock_was_set_delayed tells them
 * (timekeeping.c:2279-2283).
 */
void rtc_seconds(uint32_t n)
{
    uint32_t flags = spin_lock_irqsave(&rtc_realtime_lock);
    int64_t msec, mnsec, secs, shift = 0;
    int32_t nsec;

    rtc_coarse_since_boot(&msec, &mnsec);
    rtc_normalize(msec + rtc_wall_sec, mnsec + rtc_wall_nsec, &secs, &nsec);
    secs -= n;
    while (n-- > 0) {
        secs++;
        shift += ntp_second_overflow(secs + shift);
    }
    tick_write_begin();
    rtc_applied = ntp_tick_length();
    rtc_rate = rtc_rate_of(rtc_applied);
    tick_write_end();
    if (shift != 0) {
        rtc_wall_sec += shift;
        rtc_last_seconds += shift;
        vclock_set_wall(rtc_wall_sec, rtc_wall_nsec);
        rtc_count_step();
    }
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
    if (shift != 0) {
        timerfd_clock_was_set();
    }
}

void rtc_mono_of_real(int64_t sec, int32_t nsec, int64_t *mono_sec, int32_t *mono_nsec)
{
    uint32_t flags = spin_lock_irqsave(&rtc_realtime_lock);

    rtc_normalize(sec - rtc_wall_sec, (int64_t)nsec - rtc_wall_nsec, mono_sec, mono_nsec);
    spin_unlock_irqrestore(&rtc_realtime_lock, flags);
}

uint32_t rtc_step_count(void)
{
    return rtc_steps;
}
