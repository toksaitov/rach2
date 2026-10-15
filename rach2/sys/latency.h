/*
 * latency.h: the longest delays the kernel has measured since boot, which
 * `/proc/stat` reports.
 */
#ifndef _LATENCY_H_
#define _LATENCY_H_

#include "lib/kint.h"

/*
 * Four are kept, each a maximum in microseconds from `timer_now_us`: the gap
 * between two timer ticks, which a tick delayed by interrupts held off makes
 * long; a block request from its taking to its completion; a caller's wait
 * for the request limit before it could take one; and a console line from
 * the moment it became readable to the moment a reader had it. `/proc/stat`
 * prints them as `latency_timer_us`, `latency_disk_us`,
 * `latency_diskwait_us` and `latency_console_us`.
 */

#define LAT_TIMER     0
#define LAT_DISK      1
#define LAT_DISK_WAIT 2
#define LAT_CONSOLE   3
#define LAT_COUNT     4

/** Record a delay of `us` for `kind` if it is the longest so far; a `kind`
 * outside `LAT_TIMER` to `LAT_CONSOLE` is ignored. */
void latency_note(int kind, uint32_t us);

/**
 * Record the delay from the reading `from` of `timer_now_us` to the later
 * reading `to`. A pair whose `to` is less than its `from`, as across the
 * clock's wrap after about 71 minutes, is dropped, since the subtraction
 * would make it the largest delay ever seen.
 */
void latency_span(int kind, uint32_t from, uint32_t to);

/** The longest delay recorded for `kind`, in microseconds; 0 for a `kind`
 * outside the four. */
uint32_t latency_max(int kind);

/** Measure the gap since the last tick, from the boot processor's timer
 * handler at each tick; the first call only starts the measure. */
void latency_tick(void);

#endif /* _LATENCY_H_ */
