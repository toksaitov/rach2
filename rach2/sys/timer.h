/*
 * timer.h: the kernel's tick, the sequence count by which another processor
 * reads it whole, and the clock under the tick, which measures within one.
 */
#ifndef _TIMER_H_
#define _TIMER_H_

#include "lib/kint.h"

/* The ticks since the timer started, counted by the boot processor's timer
 * handler (kernel.c) at the platform's rate, with the ticks
 * `timer_lost_ticks` finds missed. */
extern volatile uint32_t kernel_ticks;

/*
 * The boot processor's tick changes `kernel_ticks`, the uptime's seconds and
 * ticks, and the clock's record of where the tick began between two
 * increments of `kernel_tick_seq`, which is odd meanwhile. Another processor
 * reads them as one reading by reading them again when the count was odd or
 * has moved (`tick_read_begin`, `tick_read_retry`); every variable they
 * cover is volatile, so the compiler keeps the order of the accesses, and
 * the x86 keeps the order of stores and of loads. The boot processor reads
 * them with interrupts off and needs no second reading: the count is never
 * odd for it but under an NMI, which must not wait for it.
 */
extern volatile uint32_t kernel_tick_seq;

/** Begin and end the boot processor's change of the tick, from its timer
 * handler (kernel.c): each adds one to `kernel_tick_seq`. */
void tick_write_begin(void);
void tick_write_end(void);

/**
 * Begin a reading of what `kernel_tick_seq` covers, and return the count to
 * give `tick_read_retry` after it. On the boot processor it returns 0 at
 * once; on another it waits for an even count, serving the other
 * processors' requests of this one (`smp_service`) meanwhile.
 */
uint32_t tick_read_begin(void);

/** Whether what was read since `tick_read_begin` gave `seq` must be read
 * again: on a processor other than the boot processor, whether the count
 * has moved; never on the boot processor. */
int tick_read_retry(uint32_t seq);

/** The uptime's seconds, the ticks of this second and `kernel_ticks`, as
 * one reading, with interrupts off while it reads. */
void tick_now(uint32_t *seconds, uint32_t *second_ticks, uint32_t *ticks);

/*
 * The clock under the tick: microseconds since the boot, the place within
 * the tick, and the ticks the timer did not deliver. The platform's timer
 * defines it (arch/x86/pit.c, from the counter of the timer that ticks, the
 * local APIC's or the 8254's, with the time-stamp counter where it runs at
 * one rate); the tick itself is the kernel's (kernel_ticks).
 */

/**
 * Microseconds since the boot, from the tick's periods and the place of the
 * counter under the tick within the current one, for intervals shorter than
 * the 10 ms tick: the latency record, block requests, TCP's round trips and
 * the USB stamps.
 *
 * On a processor other than the boot processor the place is the reference
 * counter's time since the boot processor's tick began, short of a whole
 * period. No reading is earlier than one given before on any processor. A
 * stretch with interrupts off longer than a period loses the periods no
 * reading fell inside; the tick count makes them up (`timer_lost_ticks`)
 * and this clock does not. It wraps after about 71 minutes. It turns
 * interrupts off and takes a spinlock of its own while it reads, so an
 * interrupt handler may call it.
 */
uint32_t timer_now_us(void);

/**
 * Where the clock is within the tick, with interrupts off: `*pending` is
 * nonzero when the tick's interrupt has been raised and not yet counted,
 * the counter having wrapped into a tick the handler has not seen, and
 * `*phase_ns` is the nanoseconds into the tick in progress. On the boot
 * processor these come from the counter under its tick; on any other, whose
 * own timer does not run in step with it, from the platform's reference
 * counter since the boot processor's last tick began, which the caller
 * reads within `tick_read_begin` and `tick_read_retry`.
 */
void timer_phase(int *pending, uint32_t *phase_ns);

/**
 * Count one period of the counter, unless a reading of `timer_now_us` has
 * counted it already, from the boot processor's timer handler and nowhere
 * else, and record where the tick began by the reference counter, for the
 * other processors' readings.
 *
 * `timer_now_us` takes its periods from the tick, because two readings a whole
 * period apart are at the same place in the counter's sweep and the wrap
 * between them cannot be seen.
 */
void timer_tick_clock(void);

/**
 * Whether the clock's period accounting is right, on fixed readings: the
 * arithmetic `timer_now_us` and `timer_tick_clock` run, fed readings whose
 * answer is known, so a wrong accounting fails on every machine and a
 * machine a hypervisor stops cannot fail it, since nothing here is read from
 * the timer. The boot's timing record takes its level from this. Nonzero
 * when every answer is right.
 */
int timer_clock_check(void);

/**
 * Measure the processor's cycle counter against the timer, with interrupts
 * off for 20 ms, where the counter runs at one rate whatever the processor's
 * clock does and no hypervisor runs the machine (on a PC, what `cpuid`
 * leaves 0x80000007 and 1 say), so that a tick the timer misses can be made
 * up. Returns the counter's counts in one tick, and 0 otherwise, when no
 * missed tick is made up.
 */
uint32_t timer_calibrate(void);

/**
 * The ticks the timer did not deliver since its last interrupt, by the cycle
 * counter, from the timer handler and nowhere else: the whole ticks of the
 * interval, rounded down, less the one this interrupt delivers, at most a
 * minute's. Zero without a calibration, and at the first call.
 */
uint32_t timer_lost_ticks(void);

#endif /* _TIMER_H_ */
