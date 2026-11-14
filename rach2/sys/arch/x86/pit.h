#ifndef _PIT_H_
#define _PIT_H_

#include "isr.h"

#include "../../timer.h"

#include "../../lib/kint.h"
#include "../../kio.h"

/*
 * The PC's timer, the 8254 programmable interval timer, and the clock under
 * the tick that ../../timer.h declares. Channel 0 raises the tick on line 0
 * when the 8259 pair has the lines. With the I/O APIC the local APIC's
 * timer ticks, or channel 0 through the I/O APIC when that timer could not
 * be measured, and `timer_set_counter` puts the ticking timer's count under
 * the clock.
 */

/** Program channel 0 to raise line 0 `frequency` times a second, in mode 3,
 * the square wave, and record its reload in `pit_reload`. The caller
 * installs the tick's handler. */
void init_pit(uint32_t frequency);

/*
 * The reload value `init_pit` last programmed into channel 0.
 *
 * `udelay.c` measures against the same counter and needs to know where it
 * wraps, so it reads the reload here rather than computing the divisor a
 * second time, which a change to the tick rate could leave behind. It has
 * the value for TICK_HZ before `init_pit` runs, so that a wait during early
 * boot is measured.
 */
extern uint32_t pit_reload;

/**
 * Whether channel 0 counts: 1 when its count moves within a few thousand
 * readings after `init_pit`, 0 on a machine with no 8254, where the ports
 * read all ones.
 */
int pit_probe(void);

/*
 * The counter under the tick, which the clock of ../../timer.h reads: where
 * it is inside the tick's period, in microseconds and in nanoseconds, the
 * period, and whether the tick has been raised and not yet taken. It is the
 * 8254's channel 0 until the platform chooses another, the local APIC's timer
 * when that is the tick (apictimer.c).
 */
struct tick_counter {
    uint32_t (*phase_us)(void);
    uint32_t (*phase_ns)(void);
    uint32_t (*period_us)(void);
    int (*pending)(void);
};

/** Read the clock from `c`, or from channel 0 again when `c` is NULL. The
 * platform calls it with interrupts off, before the tick starts or when it
 * moves the tick to another timer. */
void timer_set_counter(const struct tick_counter *c);

/* Channel 0's own parts of a counter, for a platform that takes its tick
 * through another controller than the 8259 (apictimer.c). */

/** Where channel 0's counter is inside its period now, in microseconds,
 * from its count and status latched together. */
uint32_t pit_phase_us(void);

/** `pit_phase_us` in nanoseconds, at a microsecond's resolution. */
uint32_t pit_phase_ns(void);

/** Channel 0's period, two sweeps of its counter, in microseconds. */
uint32_t pit_period_us(void);

/* Channel 0's counts a second as it is read in mode 3, where the count
 * falls by two at each tick of the 1.193182 MHz input clock. */
#define PIT_COUNTS_PER_SEC (2u * 1193182u)

/**
 * Sample channel 0 until its count has fallen by `counts` at least, and
 * return the counts it fell from the first sample to the last, which a
 * measurement divides by PIT_COUNTS_PER_SEC. A sweep of the counter that
 * passes between two samples, as when the processor is stopped, is not
 * seen, so the counts returned are never more than the time that passed and
 * may be less. 0 when the counter stops. Defined in udelay.c.
 */
uint32_t pit_wait_counts(uint32_t counts);

/** The TSC's counts in a tick, as `timer_calibrate` measured them or KVM
 * stated them, or 0 when there is no rate: no invariant TSC, no rate from
 * KVM and no `rach2.tscreliable`, or `rach2.notsc`. */
uint32_t tsc_counts_per_tick(void);

/**
 * Measure a TSC that nothing vouches for, for the vDSO's clock alone,
 * where `tsc_counts_per_tick` has no rate (pit.c). Its counts in a tick,
 * or 0. Called once, before the first tick, with interrupts off.
 */
uint32_t timer_tsc_watch(void);

/** Take `per_tick` as the watched TSC's counts in a tick, the rate the
 * vDSO's clock measured over its first span (vclock.c); nothing where no
 * TSC is watched. */
void timer_tsc_watch_refine(uint32_t per_tick);

/** The counts in a tick of the TSC the clock may run from: the trusted
 * rate, `tsc_counts_per_tick`, or else the watched one, or 0. */
uint32_t tsc_clock_per_tick(void);

/** Whether the TSC keeps the clock even where the HPET or the PM timer
 * could, and is measured before the first tick (`platform_timer_start`):
 * under `rach2.tscreliable`, or where KVM's clock pages are kept
 * (`kvmclock_kept`, kvmclock.h), whose clock, the TSC converted through a
 * page, Linux reads with no exit. */
int timer_tsc_trusted(void);

/* What Linux makes of the TSC where KVM's clock pages are kept
 * (`timer_tsc_kvm`). */
#define TSC_KVM_BELOW    0 /* kvm-clock is the clock: no TSC at one rate */
#define TSC_KVM_WATCHED  1 /* the TSC is, with kvm-clock its watchdog */
#define TSC_KVM_RELIABLE 2 /* the TSC is, unwatched */

/** How the TSC rates against kvm-clock, a TSC_KVM_ value, as Linux rates
 * the two clock sources and decides whether its clocksource watchdog
 * keeps the TSC: by the processor's invariant TSC bit, its IA32_TSC_ADJUST
 * and `rach2.tscreliable`. The same on every processor and at every
 * call. */
int timer_tsc_kvm(void);

/*
 * The wait `k_udelay` makes instead of counting channel 0, or NULL to count
 * it: set by the platform on a machine with no 8254, where it is the HPET's
 * or the PM timer's (refclock.c), and by pit.c to the TSC's once the TSC
 * keeps the clock. Defined in udelay.c.
 */
extern void (*udelay_wait)(uint32_t usec);

#endif
