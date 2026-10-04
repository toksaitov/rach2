#ifndef _RTC_H_
#define _RTC_H_

#include "irq.h"

#include "lib/kint.h"

/*
 * The real-time clock: the time of day the kernel keeps from one reading of
 * the machine's clock at boot and the tick since (clock.c), the clock read
 * and set, and its alarm. The platform's clock driver defines the reading,
 * the setting and the alarm (arch/x86/cmos.c, the PC's MC146818 CMOS clock)
 * and sets the anchor through `rtc_anchor`.
 */

/* A sample decoded into fields, in UTC: what the clock meant, not what it
 * held. */
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

void rtc_time_from_anchor(int64_t anchor_seconds, uint32_t anchor_uptime, uint32_t uptime,
                          uint32_t subsecond_ticks, int64_t *seconds, int64_t *nanoseconds);

void rtc_init(void);
int rtc_is_anchored(void);
int64_t rtc_boot_time(void);
int64_t rtc_boot_epoch(void);
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds);
void rtc_coarse_realtime(int64_t *seconds, int64_t *nanoseconds);
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
