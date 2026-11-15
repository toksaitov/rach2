/*
 * rtc.h: the clocks since boot and the time of day, kept from the tick and
 * one reading of the machine's clock at boot, slewed as the NTP state asks,
 * and the machine's clock read, set and its alarm.
 */
#ifndef _RTC_H_
#define _RTC_H_

#include "irq.h"

#include "lib/kint.h"

struct ktimex;

/*
 * The time the kernel keeps and the calendar arithmetic are clock.c's. The
 * platform's clock driver defines the reading, the setting and the alarm
 * (arch/x86/cmos.c, the PC's MC146818 CMOS clock) and sets the anchor
 * through `rtc_anchor` or `rtc_anchor_now`.
 */

/* A time in calendar fields, in UTC: the full year, the month from 1 to 12,
 * the day from 1, the hour from 0 to 23. */
typedef struct {
    int32_t year;
    int32_t month;
    int32_t day;
    int32_t hour;
    int32_t minute;
    int32_t second;
} rtc_time_t;

/** The days in `month` (1 to 12) of `year`, a full year. */
int32_t rtc_days_in_month(int32_t year, int32_t month);

/** Anchor real time: the clock read `seconds` since the epoch at uptime
 * second `uptime`. The platform's `rtc_init` calls it once. */
void rtc_anchor(int64_t seconds, uint32_t uptime);

/** Anchor real time whole: it is `seconds` since the epoch and
 * `nanoseconds`, under a second, now, for a clock that states the fraction
 * of its second, as KVM's wall clock does. The platform's `rtc_init` calls
 * it once in `rtc_anchor`'s place, with interrupts off, right after it
 * read the clock. */
void rtc_anchor_now(int64_t seconds, uint32_t nanoseconds);

/** Take the clock's alarm interrupt with `handler`, and route it to the
 * processor. */
void rtc_alarm_attach(isr_handler_t handler);

/**
 * Real time from an anchor, apart from the hardware and from the anchor
 * kept here, so that a host test can call it: in `*seconds`,
 * `anchor_seconds`, which the clock read at uptime second `anchor_uptime`,
 * plus the uptime seconds since, to `uptime`; in `*nanoseconds`, the
 * `subsecond_ticks` into the current second. With the anchor at zero it is
 * the time since boot.
 */
void rtc_time_from_anchor(int64_t anchor_seconds, uint32_t anchor_uptime, uint32_t uptime,
                          uint32_t subsecond_ticks, int64_t *seconds, int64_t *nanoseconds);

/**
 * Read the machine's clock at boot, in pairs of samples until two agree, up
 * to four pairs, and anchor real time to it, with the time in the log as
 * `rtc: 2026-10-15 12:00:00 UTC`. A clock that cannot be read, holds no
 * time or never agrees leaves no anchor, real time runs from the epoch, and
 * the log says `rtc: no clock: times run from the epoch`.
 */
void rtc_init(void);

/** Whether the boot read the clock and anchored real time to it. */
int rtc_is_anchored(void);

/*
 * The clocks since boot. The raw clock, CLOCK_MONOTONIC_RAW, is the ticks
 * the timer counts and its place within the tick. The monotonic clock,
 * which CLOCK_MONOTONIC and CLOCK_BOOTTIME read, the machine never being
 * suspended, gains the NTP state's tick length in each raw tick, as Linux's
 * tkr_mono follows the tick length and tkr_raw does not
 * (kernel/time/timekeeping.c:2054-2107, 2164-2193); its whole ticks are the
 * kernel's tick count, `kernel_ticks`, and the uptime, so that a deadline of
 * ticks is a time of the monotonic clock. Real time is the monotonic clock
 * plus an offset (`rtc_offset`) that only a step changes. The rate of a tick
 * length, what the monotonic clock gains or loses in a nanosecond of the raw
 * clock, is in 2^-RTC_RATE_SHIFT nanoseconds, under a quarter of a
 * nanosecond either way.
 */
#define RTC_RATE_SHIFT 45

/** `ns` nanoseconds of the raw clock, below 2^62, as the monotonic clock's
 * at `rate`; a pure function, for the vDSO's clock and a host test. */
uint64_t rtc_slewed(uint64_t ns, int64_t rate);

/** The NTP state's tick length now (`ntp_tick_length`), which the timer
 * pass reads before it changes the tick, for `rtc_tick_advance`. */
int64_t rtc_tick_length(void);

/**
 * Count `raw` ticks of the timer, from the timer pass (kernel.c) between
 * `tick_write_begin` and `tick_write_end`: each moves the monotonic clock on
 * by the tick length the last pass took, and its whole ticks move
 * `kernel_ticks` and the uptime; the ticks after these take `length`, from
 * `rtc_tick_length`. Returns the seconds of real time begun, which the pass
 * gives `rtc_seconds` once the tick is written.
 */
uint32_t rtc_tick_advance(uint32_t raw, int64_t length);

/** The rate the monotonic clock runs at now against the raw clock, from
 * the tick length the last pass took, for the vDSO's clock. */
int64_t rtc_mono_rate(void);

/** The monotonic and the raw clock now in nanoseconds, from the tick and
 * the timer's place within it, read together with interrupts off: the
 * reference the vDSO's clock follows. */
void rtc_tick_both(uint64_t *mono_ns, uint64_t *raw_ns);

/** The monotonic clock at the last tick in nanoseconds, the time of the
 * tick's edge and not of its handler, as the coarse clocks read it from the
 * tick (`rtc_coarse_monotonic`), without the floor a stop of the vDSO's
 * clock leaves: the time the vDSO's coarse clocks give while it runs. */
uint64_t rtc_tick_coarse_ns(void);

/** The real time of uptime zero, which /proc/stat gives as `btime` and a
 * node with no times of its own reports (a device, a /proc file, a pipe):
 * the seconds of `rtc_offset`, which a slew does not move. */
int64_t rtc_boot_epoch(void);

/** Real time less the monotonic clock, in `*seconds` and `*nanoseconds`,
 * which is 0 to 10^9 - 1: the anchor less the uptime it was read at, with
 * what each setting and leap second since have added. */
void rtc_offset(int64_t *seconds, int32_t *nanoseconds);

/**
 * The real time to the last tick, read with no lock and no wait, for a
 * panic: a processor stopped while it held the real-time lock or wrote the
 * tick would stop a reader that took either. A change made on another
 * processor during the read can tear it by that change.
 */
void rtc_realtime_unlocked(int64_t *seconds, int32_t *microseconds);

/**
 * The real time now, in seconds since 1970 and nanoseconds within the
 * second: the monotonic clock to the timer's place within the tick, or the
 * TSC's clock while it runs (vdso.h's `vclock_read`), plus the offset, so a
 * reading here and the vDSO's agree. No reading is earlier than the one
 * before it until the clock is set or a leap second is inserted.
 * Interrupts are off while it reads.
 */
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds);

/** The real time to the last tick, without the timer's place within it and
 * without `rtc_realtime`'s floor: the coarse clock, which reads no timer;
 * the TSC's clock as of its last tick while that one runs, and otherwise
 * never before the last time that clock gave where it stopped. */
void rtc_coarse_realtime(int64_t *seconds, int64_t *nanoseconds);

/** The monotonic clock, to the timer's place within the tick, or by the
 * TSC's clock while it runs, never earlier than the reading before it.
 * Setting the clock does not move it. */
void rtc_monotonic(int64_t *seconds, int64_t *nanoseconds);

/** `rtc_monotonic` as nanoseconds since boot. */
int64_t rtc_monotonic_ns(void);

/** The raw clock, CLOCK_MONOTONIC_RAW, as `rtc_monotonic` reads the
 * monotonic one, which the NTP state does not slew. */
void rtc_raw_monotonic(int64_t *seconds, int64_t *nanoseconds);

/**
 * Run `change`, which stops or starts the vDSO's clock (vclock.c), with no
 * reading of the kernel's clocks under way across it, so that the clock it
 * stops hands its last time to the tick's and the clock it starts begins at
 * or past every reading given. `change` is given in `*mono_ns` the latest
 * monotonic time a reading has given, the monotonic floor or real time's
 * less the offset, and in `*raw_ns` the raw clock's floor, in nanoseconds
 * since boot; it may move either later, to the last time of the clock it
 * stops or the time of the clock it starts, and each floor is then raised
 * to them, real time's to the monotonic time plus the offset, and the coarse
 * clocks' to the monotonic time. Interrupts may be on or off; `change` runs
 * with them off.
 */
void rtc_vclock_change(void (*change)(uint64_t *mono_ns, uint64_t *raw_ns));

/** The monotonic clock at the last tick, without the timer's place within
 * it: the coarse monotonic clock, which reads no timer; the TSC's clock as
 * of its last tick while that one runs, and otherwise never before the last
 * time that clock gave where it stopped (`rtc_vclock_change`). */
void rtc_coarse_monotonic(int64_t *seconds, int64_t *nanoseconds);

/** The tick count of the monotonic clock now, with the ticks that have
 * passed and the handler has not counted (`timer_phase`), and how far into
 * the next tick the clock is, in nanoseconds, read together with interrupts
 * off. */
void rtc_tick_phase(uint64_t *ticks, uint32_t *phase_ns);

/** The tick count as the clocks count it, with the ticks the handler has
 * not counted yet (`rtc_tick_phase`). A deadline of whole ticks is counted
 * from it, and not from `kernel_ticks`, which takes the tick only when the
 * boot processor's handler runs: a wait begun while the tick is pending
 * would end a tick early by the monotonic clock. */
uint64_t rtc_ticks_now(void);

/* The latest time `rtc_set_realtime` takes, Linux's TIME_SETTOD_SEC_MAX
 * (linux/time64.h:43-44): the seconds of 2^63 nanoseconds, less 30 years
 * for the monotonic clock, so that a time in nanoseconds never overflows. */
#define RTC_SETTOD_SEC_MAX (9223372036LL - 30LL * 365 * 24 * 3600)

/**
 * Make the real time `seconds` and `nanoseconds` now, as clock_settime,
 * settimeofday and stime do for a privileged caller; the caller checks the
 * privilege. Real time goes on from it at the rate it had, its floor starts
 * over, and `rtc_boot_epoch` moves by the same step. Returns -1, with the
 * time unchanged, for a time Linux refuses (kernel/time/timekeeping.c:
 * 1350-1364): before 1970, at or past its TIME_SETTOD_SEC_MAX, or before the
 * monotonic clock; else 0. On a refusal for the monotonic clock, as on a
 * step, the NTP state forgets its offsets and adjtime's slew (`ntp_clear`)
 * and the timers of real time are told (`timerfd_clock_was_set`), as
 * Linux's clock_was_set tells them on both paths (kernel/time/
 * timekeeping.c:1369-1376); a TFD_TIMER_CANCEL_ON_SET timerfd is canceled,
 * and the step count moves on (`rtc_step_count`), only when the offset of
 * real time changed, which a refusal leaves, as Linux's timerfd_clock_was_set
 * compares each timerfd's moffs with it (fs/timerfd.c:97-116). The
 * machine's clock is not written (`rtc_write_clock`).
 */
int rtc_set_realtime(int64_t seconds, int32_t nanoseconds);

/**
 * Move real time on by `seconds` and `nanoseconds`, the latter from 0 to
 * 10^9 - 1, earlier for negative seconds, as adjtimex's ADJ_SETOFFSET does,
 * Linux's timekeeping_inject_offset (kernel/time/timekeeping.c:1393-1429):
 * a step as `rtc_set_realtime` makes, refused, with -1, for a time before
 * 1970, past TIME_SETTOD_SEC_MAX or before the monotonic clock, though the
 * NTP state forgets its offsets and the timers are told either way.
 */
int rtc_shift_realtime(int64_t seconds, int32_t nanoseconds);

/**
 * The NTP part of adjtimex on `tx`, whose modes the caller has checked
 * (`ntp_adjtimex`), with the clock's lock held, whose tick length the next
 * tick takes: no step, and no TFD_TIMER_CANCEL_ON_SET timerfd canceled, as
 * Linux slews ADJ_OFFSET and adjtime's offsets and calls clock_was_set for
 * neither (kernel/time/ntp.c:754-755, kernel/time/timekeeping.c:2490-2532).
 * `tx->sec` and `tx->usec` get the real time read before, in the unit
 * STA_NANO chooses. Returns the clock's state.
 */
int32_t rtc_adjtimex(struct ktimex *tx);

/**
 * The `n` seconds of real time begun at the last timer pass
 * (`rtc_tick_advance`), each the NTP state's second (`ntp_second_overflow`),
 * whose tick length the next tick takes, and which inserts or deletes a
 * leap second: a step of real time a second back or on, counted as a
 * setting is (`rtc_step_count`), with the timers of real time told
 * (`timerfd_clock_was_set`). The boot processor's timer pass calls it, or a
 * late pass on another (kernel.c), holding `clock_pass_lock` with
 * interrupts off, once the tick is written, since it writes the tick's
 * length.
 */
void rtc_seconds(uint32_t n);

/** The monotonic time at which real time reaches `sec` and `nsec`, real
 * time less the offset, in `*mono_sec` and `*mono_nsec`, from 0 to
 * 10^9 - 1: a time past is one before the monotonic clock now. */
void rtc_mono_of_real(int64_t sec, int32_t nsec, int64_t *mono_sec, int32_t *mono_nsec);

/**
 * The count of the steps of real time, its settings and its leap seconds,
 * never 0, which moves on at a leap second and at a `rtc_set_realtime` or
 * `rtc_shift_realtime` that changes real time's offset. A sleep to an
 * absolute time of real time reads it before the clock, counts its tick
 * from the clock, and keeps it in `rt_steps` (proc/process.h); a later step
 * makes the two differ, the timer tick wakes the sleep, and the sleep counts
 * its tick again, as Linux's
 * clock_was_set moves the hrtimers of the real-time base
 * (kernel/time/hrtimer.c:951-987). A slew changes no offset and counts no
 * step, since real time and the ticks run at its rate alike.
 */
uint32_t rtc_step_count(void);

#if defined(RACH2_HOSTTEST)
/** Clear every floor of the clocks, for a host test that starts the tick
 * again (clock.c). */
void rtc_forget_floors(void);
#endif

/**
 * Read the machine's clock now, as the boot reads it: two samples that agree,
 * decoded, with interrupts off and the clock's ports locked against other
 * processors while each pair is read. 0 with `*out`
 * filled, or -1 when the clock cannot be read or holds no time.
 */
int rtc_read_clock(rtc_time_t *out);

/** The day of the week of `*t`, 0 for Sunday, and of the year, from 0, for
 * a valid date from 1970. */
void rtc_week_and_year_day(const rtc_time_t *t, int32_t *weekday, int32_t *yearday);

/** `*t` for `seconds` since 1970 in UTC, the calendar the clock keeps: 0,
 * or -1 for a time before 1970 or with a year past 2^31 - 1, which leaves
 * `*t` unchanged. */
int rtc_time_of_seconds(int64_t seconds, rtc_time_t *t);

/** The seconds since 1970 of `*t`, a valid time from 1970 in UTC, whose
 * year may be any to 2^31 - 1. */
int64_t rtc_seconds_of(const rtc_time_t *t);

/**
 * Set the machine's clock to `*t`, a time in UTC in the range the clock
 * keeps (on a PC, 2000 to 2099, in the encoding and hour format status
 * register B gives, with updates held while the registers are written and
 * the day of the week written too). 0, or -1 for a time out of range. The
 * time the kernel keeps is not changed.
 */
int rtc_write_clock(const rtc_time_t *t);

/**
 * The clock's alarm, which the clock keeps as a time of day, a second, a
 * minute and an hour, and raises its interrupt at when the interrupt is
 * enabled (on a PC, IRQ 8):
 * `rtc_alarm_write` sets it to `t`'s time of day in the clock's encoding;
 * `rtc_alarm_read` gives it back in `t`'s hour, minute and second, with
 * whether its interrupt is enabled; `rtc_alarm_interrupts` enables or
 * disables the interrupt; and `rtc_alarm_ack`, which the interrupt's handler
 * calls, lets the clock raise the next (on a PC, by reading status register
 * C) and returns whether the alarm was what raised it. Each holds the lock
 * on the clock's ports through its accesses, so any processor may call them,
 * in any context.
 */
void rtc_alarm_write(const rtc_time_t *t);
int rtc_alarm_read(rtc_time_t *t);
void rtc_alarm_interrupts(int on);
int rtc_alarm_ack(void);

#endif /* _RTC_H_ */
