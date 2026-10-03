#ifndef _LATENCY_H_
#define _LATENCY_H_

#include "lib/kint.h"

/*
 * latency: the longest delays the kernel has seen since boot, for the load
 * row and the record a machine keeps of itself. Four are kept, each a
 * maximum in microseconds from `timer_now_us`: the gap between two timer
 * ticks, which a tick delayed by interrupts held off makes long; a block
 * request from its taking to its completion; a caller's wait for the
 * request limit before it could take one; and a console line from the
 * moment it became readable to the moment a reader had it. `/proc/stat`
 * prints them as `latency_timer_us`, `latency_disk_us`,
 * `latency_diskwait_us` and `latency_console_us`.
 */

#define LAT_TIMER     0
#define LAT_DISK      1
#define LAT_DISK_WAIT 2
#define LAT_CONSOLE   3
#define LAT_COUNT     4

/** Record a delay of `us` for `kind` if it is the longest so far. */
void latency_note(int kind, uint32_t us);

/**
 * Record the delay from the reading `from` of `timer_now_us` to the later
 * reading `to`. A reading taken later that answers less is a step of the
 * clock backwards within a tick, which the subtraction would turn into the
 * largest delay ever seen; such a pair is dropped.
 */
void latency_span(int kind, uint32_t from, uint32_t to);

/** The longest delay recorded for `kind`, in microseconds. */
uint32_t latency_max(int kind);

/** The timer's tick: the gap since the last one is measured here. */
void latency_tick(void);

#endif /* _LATENCY_H_ */
