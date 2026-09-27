#ifndef _TIMERFD_H_
#define _TIMERFD_H_

#include "../lib/kint.h"

/*
 * timerfd: a timer read through a descriptor. timerfd_settime arms it with a
 * first expiration, relative or absolute on its clock, and an interval; a
 * read of eight bytes returns the expirations since the last read and waits
 * while there are none; poll and epoll report it readable while there are
 * some. The kernel keeps time in 10 ms ticks, so an expiration is rounded up
 * to a tick. The tick handler calls `timerfd_tick`.
 */

struct isr_params;

int32_t sys_timerfd_create(struct isr_params *params);
int32_t sys_timerfd_settime(struct isr_params *params);
int32_t sys_timerfd_gettime(struct isr_params *params);
int32_t sys_timerfd_settime64(struct isr_params *params);
int32_t sys_timerfd_gettime64(struct isr_params *params);

/** Count the expirations due by the tick `now` and wake the readers; called
 * from the timer interrupt. */
void timerfd_tick(uint32_t now);

/*
 * The clocks' part, in syscall.c: the time on clock `clk` as clock_gettime
 * reads it, in seconds and nanoseconds. 0, or -EINVAL for a clock with no
 * time here.
 */
int32_t clock_now(int32_t clk, int64_t *sec, int32_t *nsec);

#endif /* _TIMERFD_H_ */
