/*
 * timer.h: the kernel's tick, the sequence count by which another processor
 * reads it whole, and the clock under the tick, which measures within one.
 */
#ifndef _TIMER_H_
#define _TIMER_H_

#include "lib/kint.h"

/* The ticks since the timer started, counted by the boot processor's timer
 * handler (kernel.c) at the platform's rate, as many for each interrupt as
 * `timer_tick_clock` says. */
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
 * The clock under the tick: the ticks each interrupt counts, microseconds
 * since the boot, and the place within the tick. The platform's timer
 * defines it (arch/x86/pit.c). From the first tick on it is kept by a
 * reference counter where the machine has one, the HPET's main counter, the
 * ACPI PM timer or, without either, a time-stamp counter at one rate, so
 * that a tick delivered late, coalesced with others or delivered again
 * counts the time that passed; without one it is the periods of the timer
 * that ticks and that timer's count. The tick itself is the kernel's
 * (kernel_ticks).
 */

/**
 * Microseconds since the boot, for intervals shorter than the 10 ms tick:
 * the latency record, block requests, TCP's round trips and the USB stamps.
 * With a reference, its ticks since it started and its place within the
 * next, on every processor; without one, the periods the boot processor's
 * tick and readings counted and the place of the counter under the tick,
 * which loses a period no reading fell inside while interrupts were off.
 *
 * No reading is earlier than one given before on any processor. It wraps
 * after about 71 minutes. It turns interrupts off and takes a spinlock of
 * its own while it reads, so an interrupt handler may call it.
 */
uint32_t timer_now_us(void);

/**
 * Where the clock is within the tick, with interrupts off: `*pending` is the
 * ticks that have passed and that the boot processor's handler has not yet
 * counted, and `*phase_ns` the nanoseconds into the tick in progress. With a
 * reference these are its own, on any processor, and `*pending` is any
 * number up to a minute's ticks; without one they come from the counter
 * under the boot processor's tick, and `*pending` is 1 when its interrupt
 * has been raised and not yet taken. The caller reads them within
 * `tick_read_begin` and `tick_read_retry`.
 */
void timer_phase(int *pending, uint32_t *phase_ns);

/**
 * Count the boot processor's tick, from its timer handler within the tick's
 * sequence count and nowhere else, and return the ticks this interrupt
 * counts. Before a reference starts that is 1, and the clock counts a
 * period of the counter under the tick unless a reading of `timer_now_us`
 * has counted it already. The first tick with a reference ready starts it;
 * from then on it is the reference's whole ticks since the last one counted,
 * at most a minute's: 0 for an interrupt a hypervisor delivers again, and
 * more than 1 for one that came late.
 */
uint32_t timer_tick_clock(void);

/**
 * Whether the clock's arithmetic is right, on fixed readings: the period
 * accounting of `timer_now_us` and `timer_tick_clock` without a reference,
 * and the reference's count of whole ticks with a fraction of a count in
 * each, fed readings whose answer is known, so a wrong accounting fails on
 * every machine and a machine a hypervisor stops cannot fail it, since
 * nothing here is read from a timer. The boot's timing record takes its
 * level from this. Nonzero when every answer is right.
 */
int timer_clock_check(void);

/**
 * Measure the processor's time-stamp counter against the 8254, with
 * interrupts off for 20 ms, on a machine with neither the HPET nor the PM
 * timer whose counter runs at one rate whatever the processor's clock does
 * (on a PC, what `cpuid` leaf 0x80000007 says), so that it can be the
 * reference from the next tick on. Returns its counts in one tick, and 0
 * when it is not measured.
 */
uint32_t timer_calibrate(void);

/** The reference that keeps the clock, "hpet", "pm timer" or "tsc", or NULL
 * when the counter under the tick keeps it. */
const char *timer_reference(void);

#endif /* _TIMER_H_ */
