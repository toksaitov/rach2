/*
 * rtc.h: the time of day, kept from one reading of the machine's clock at
 * boot and the tick since, and the machine's clock read, set and its alarm.
 */
#ifndef _RTC_H_
#define _RTC_H_

#include "irq.h"

#include "lib/kint.h"

/*
 * The time the kernel keeps and the calendar arithmetic are clock.c's. The
 * platform's clock driver defines the reading, the setting and the alarm
 * (arch/x86/cmos.c, the PC's MC146818 CMOS clock) and sets the anchor
 * through `rtc_anchor`.
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

/** The real time at which the boot read the clock, which a node with no
 * times of its own reports (a device, a /proc file, a pipe); 0 with no
 * anchor. Setting the clock does not move it. */
int64_t rtc_boot_time(void);

/** The real time of uptime zero, which /proc/stat gives as `btime`: the
 * anchor less the uptime it was read at, with the step `rtc_set_realtime`
 * made added; the step alone with no anchor. */
int64_t rtc_boot_epoch(void);

/**
 * The real time now, in seconds since 1970 and nanoseconds within the
 * second: the anchor's time, the time since it to the timer's place within
 * the tick, and the step `rtc_set_realtime` made. No reading is earlier than
 * the one before it until the clock is set again. Interrupts are off while it
 * reads.
 */
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds);

/** The real time to the last tick, without the timer's place within it and
 * without `rtc_realtime`'s floor: the coarse clock, which reads no timer. */
void rtc_coarse_realtime(int64_t *seconds, int64_t *nanoseconds);

/** The time since boot, to the timer's place within the tick, never earlier
 * than the reading before it. Setting the clock does not move it. */
void rtc_monotonic(int64_t *seconds, int64_t *nanoseconds);

/** The tick count, with a tick whose interrupt is pending counted, and how
 * far into that tick the timer is, in nanoseconds, read together with
 * interrupts off. */
void rtc_tick_phase(uint32_t *ticks, uint32_t *phase_ns);

/** The tick count as the clocks count it, with a tick whose interrupt is
 * pending counted (`rtc_tick_phase`). A deadline of whole ticks is counted
 * from it, and not from `kernel_ticks`, which takes the tick only when the
 * boot processor's handler runs: a wait begun while the tick is pending
 * would end a tick early by the monotonic clock. */
uint32_t rtc_ticks_now(void);

/**
 * Make the real time `seconds` and `nanoseconds` now, as clock_settime,
 * settimeofday and stime do for a privileged caller; the caller checks the
 * privilege. Real time goes on from it at the tick's rate, its floor starts
 * over, and `rtc_boot_epoch` moves by the same step. The machine's clock is
 * not written (`rtc_write_clock`).
 */
void rtc_set_realtime(int64_t seconds, int32_t nanoseconds);

/**
 * Read the machine's clock now, as the boot reads it: two samples that agree,
 * decoded, with interrupts off while each pair is read. 0 with `*out`
 * filled, or -1 when the clock cannot be read or holds no time.
 */
int rtc_read_clock(rtc_time_t *out);

/** The day of the week of `*t`, 0 for Sunday, and of the year, from 0. */
void rtc_week_and_year_day(const rtc_time_t *t, int32_t *weekday, int32_t *yearday);

/** `*t` for `seconds` since 1970 in UTC, the calendar the clock keeps. */
void rtc_time_of_seconds(uint32_t seconds, rtc_time_t *t);

/** The seconds since 1970 of `*t`, a time from 1970 in UTC. */
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
 * C) and returns whether the alarm was what raised it.
 */
void rtc_alarm_write(const rtc_time_t *t);
int rtc_alarm_read(rtc_time_t *t);
void rtc_alarm_interrupts(int on);
int rtc_alarm_ack(void);

#endif /* _RTC_H_ */
