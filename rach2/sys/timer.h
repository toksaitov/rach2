/*
 * timer.h: the kernel's tick, the sequence count by which another processor
 * reads it whole, and the clock under the tick, which measures within one.
 */
#ifndef _TIMER_H_
#define _TIMER_H_

#include "lib/kint.h"

/* The kernel's tick rate, in ticks a second, and the tick's length. */
#define TICK_HZ 1000
#define TICK_NS (1000000000u / TICK_HZ)
#define TICK_US (1000000u / TICK_HZ)

/* The rate of the `clock_t` a program sees, Linux's USER_HZ: `times`, the
 * /proc stat files and AT_CLKTCK count in it, whatever TICK_HZ is. TICK_HZ
 * is a multiple of it. */
#define USER_HZ 100

/* Milliseconds as whole ticks, rounded up, for a constant under 2^22 ms. */
#define MS_TO_TICKS(ms) (((uint32_t)(ms) * TICK_HZ + 999u) / 1000u)

/* The ticks of the monotonic clock since the timer started, counted by the
 * boot processor's timer handler (kernel.c): the raw ticks each interrupt
 * counts, as many as `timer_tick_clock` says, each of the NTP state's tick
 * length, which is TICK_NS while nothing slews the clock (clock.c's
 * `rtc_tick_advance`). It is 64 bits wide, so it does not wrap; the low
 * 32 bits, which any processor reads in one load, wrap after 2^32 ticks.
 * On the i686 a reader of the whole count reads it with `tick_count`, since
 * there the two halves are two loads; one that wants only an interval
 * shorter than the low half's cycle takes `(uint32_t)kernel_ticks`. */
extern volatile uint64_t kernel_ticks;

/*
 * A timer pass (kernel.c) changes `kernel_ticks`, the uptime's seconds and
 * ticks, and the clock's record of where the tick began between two
 * increments of `kernel_tick_seq`, which is odd meanwhile. The boot
 * processor's tick makes the pass, and so does another processor's when the
 * boot processor's is late; the writer holds the big lock with interrupts
 * off. A reader reads them as one reading by reading them again when the
 * count was odd or has moved (`tick_read_begin`, `tick_read_retry`); every
 * variable they cover is volatile, so the compiler keeps the order of the
 * accesses, and the x86 keeps the order of stores and of loads. The writer's
 * own processor needs no second reading: the count is odd for it only under
 * an NMI, which must not wait for it.
 */
extern volatile uint32_t kernel_tick_seq;

/** Begin and end a timer pass's change of the tick (kernel.c), with the big
 * lock held and interrupts off: each adds one to `kernel_tick_seq`, and the
 * first notes this processor as the writer. */
void tick_write_begin(void);
void tick_write_end(void);

/**
 * Begin a reading of what `kernel_tick_seq` covers, and return the count to
 * give `tick_read_retry` after it. On the processor writing the tick it
 * returns 0 at once; on another it waits for an even count, serving the
 * other processors' requests of this one (`smp_service`) meanwhile.
 */
uint32_t tick_read_begin(void);

/** Whether what was read since `tick_read_begin` gave `seq` must be read
 * again: on a processor other than the one writing the tick, whether the
 * count has moved; never on the writer's. */
int tick_read_retry(uint32_t seq);

/** The uptime's seconds, the ticks of this second and `kernel_ticks`, as
 * one reading, with interrupts off while it reads. */
void tick_now(uint32_t *seconds, uint32_t *second_ticks, uint64_t *ticks);

/**
 * `n` divided by `d`, with the remainder in `*rem` when it is not NULL, for
 * a divisor from 1 to 65535: a count of ticks as seconds, or as `clock_t`.
 * The freestanding kernel has no `__udivdi3` for a 64-bit division, so it
 * divides the high word and then the low word sixteen bits at a time.
 */
uint64_t tick_div(uint64_t n, uint32_t d, uint32_t *rem);

/** `n` divided by `d`, which is not 0, with the remainder in `*rem` when it
 * is not NULL, for any divisor: one bit of the quotient at a time where
 * either is past 32 bits, so slower than `tick_div`. */
uint64_t tick_div64(uint64_t n, uint64_t d, uint64_t *rem);

/**
 * A span of `sec` seconds and `nsec` nanoseconds, neither negative and
 * `nsec` under a second, as whole ticks, a partial tick counted as one. The
 * seconds are cut at 2^40, about 34800 years, which a deadline of ticks
 * waits as long as any later one, so the sum with the tick count cannot
 * overflow.
 */
uint64_t tick_span(int64_t sec, uint32_t nsec);

/** Ticks as the `clock_t` of USER_HZ that a program sees. */
#define TICKS_TO_CLOCK(t) tick_div((t), TICK_HZ / USER_HZ, (uint32_t *)0)

/** `kernel_ticks` whole, on any processor and in any context, with
 * interrupts off while it reads. A deadline of ticks is compared with it. */
uint64_t tick_count(void);

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
 * Microseconds since the boot, for intervals shorter than a tick:
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
 * The resolution of the place within the tick, in nanoseconds, which the
 * real-time and monotonic clocks are read to: a period of the reference,
 * rounded up, or a microsecond while the counter under the tick keeps the
 * clock, since its place is read in microseconds.
 */
uint32_t timer_resolution_ns(void);

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
 * Count the tick, from a timer pass within the tick's sequence count and
 * nowhere else, and return the ticks this interrupt counts. Only the boot
 * processor's pass calls it until `timer_tick_shared` says any may. Before a
 * reference starts that is 1, and the clock counts a period of the counter
 * under the tick unless a reading of `timer_now_us` has counted it already. The
 * first tick with a reference ready starts it; from then on it is the
 * reference's whole ticks since the last one counted, at most a minute's: 0 for
 * an interrupt a hypervisor delivers again, and more than 1 for one that came
 * late.
 */
uint32_t timer_tick_clock(void);

/** 1 once a reference keeps the clock, so that a processor other than the
 * boot one may count the tick (`timer_tick_clock`), which then reads the
 * reference and not the boot processor's own counter; 0 before. */
int timer_tick_shared(void);

/** The timer passes since boot that a processor other than the boot one
 * made because the boot processor's was late (kernel.c), modulo 2^32, which
 * `/proc/stat` gives as `timer_passes_late`. */
uint32_t timer_late_passes(void);

/**
 * Have this processor's timer interrupt at `ns`, nanoseconds since boot on
 * the monotonic clock, besides its ticks, so that a timed sleep or a timer
 * whose time falls between two ticks ends at that time, as Linux's hrtimers
 * end one, and not at the tick after it. A sleep or a timer asks when it is
 * set, however far its time, as Linux starts an hrtimer then. The interrupt
 * makes the pass `timer_hr_pass` describes. The processor keeps the
 * earliest time asked for until an interrupt serves it; every pass, that
 * interrupt's and each tick's, then asks again for each time still to come,
 * so a time an earlier one put aside is armed once that is served
 * (apictimer.c). Nothing is armed where the local APIC's timer is not the
 * tick or before a reference keeps the clock (`timer_tick_shared`), whose
 * clock the timer's own count then is. Any context; interrupts are left as
 * they were.
 */
void timer_wake_at(int64_t ns);

/**
 * Begin a pass over many sleepers or timers on this processor, during which
 * `timer_wake_at` only keeps the earliest time asked of it, until the
 * matching `timer_wake_release` programs the timer once for that time, as
 * Linux's hrtimer_interrupt sets in_hrtirq so that a timer started by a
 * callback does not program the device, and programs the next event once
 * after the queues have run (kernel/time/hrtimer.c:907, 1863-1893). Passes
 * nest. Interrupts are off from the hold through its release, so that both
 * run on the same processor.
 */
void timer_wake_hold(void);

/** End the pass `timer_wake_hold` began; the outermost release asks
 * `timer_wake_at` for the earliest time asked during it. Interrupts are
 * off. */
void timer_wake_release(void);

/** Whether `timer_wake_at` arms anything, so that a sleep ends at its time
 * and not at its tick: the local APIC's timer is the tick, has a vector for
 * its interrupts between ticks, and a reference keeps the clock. Any
 * context. */
int timer_wakes_between(void);

/**
 * The pass of an interrupt `timer_wake_at` armed: make ready the timed
 * sleepers whose time of the monotonic clock has come before their tick
 * (`proc_wake_due`), and expire the timers whose clock has reached their
 * time (`timerfd_due`), each of which arms again every time still to come,
 * however far, and post the POSIX timers' signals when the big lock is
 * free, leaving them to a tick when it is not. kernel.c defines it; the
 * caller holds no lock, the big lock included, with interrupts off.
 */
void timer_hr_pass(void);

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
 * Measure the processor's time-stamp counter, once, with interrupts off, where
 * its counter runs at one rate whatever the processor's clock does (on a PC,
 * what `cpuid` leaf 0x80000007 says, or `rach2.tscreliable`) and `rach2.notsc`
 * is not on the command line: against the reference that keeps the clock, the
 * HPET or the PM timer, over the 20 ms window of three whose readings of it
 * were closest to the counter's, or on a machine with neither, as the lowest
 * rate of three waits of 20 ms on the 8254, so that it can be the reference
 * from the next tick on. The vDSO's clock reads the counter at this rate
 * (arch/x86_64/vclock.c). Returns its counts in one tick, and 0 when it is not
 * measured.
 */
uint32_t timer_calibrate(void);

/** Convert processor cycles to microseconds at the calibrated TSC rate.
 * Returns 0 before calibration or when no invariant counter was measured.
 * This conversion reads no hardware and is safe with interrupts off. */
uint64_t timer_cycles_us(uint64_t cycles);

/** The time on the monotonic clock, in nanoseconds since boot, at which the
 * tick this processor is taking was due, the last of them when it came
 * ticks late, or 0 when its timer keeps none, as a periodic timer does: the
 * local APIC's timer once it is one-shot (`apic_hr_entry`). For the tick's
 * handler, with interrupts off. */
int64_t timer_tick_due(void);

/** The time on the monotonic clock, to the microsecond, at which this
 * processor's counter read `cycles` (`arch_cycles`), given that it reads
 * `at` while the clock reads `now`, at the calibrated rate; 0, the clock's
 * start, when the rate was not measured or the reading is more than a
 * second old. */
int64_t timer_cycles_when(uint64_t cycles, uint64_t at, int64_t now);

/** Whether a span of processor cycles can be timed by `timer_cycles_us`:
 * the counter's rate was measured and every processor started so far
 * passed the check that its counter agrees with the boot processor's
 * (arch/x86/tsc.h), so that a span begun on one processor and ended on
 * another reads true. Safe with interrupts off. */
int timer_cycles_agree(void);

/** The reference that keeps the clock, "hpet", "pm timer" or "tsc", or NULL
 * when the counter under the tick keeps it. */
const char *timer_reference(void);

/**
 * The `n`th clock source, from 0, by the name Linux gives it, best first as
 * Linux rates them: "tsc" when the counter under the tick is measured, then
 * the reference `refclock_init` found, "hpet" or "acpi_pm", or "jiffies"
 * when there is neither; NULL past the last. With `n` -1, the one the
 * kernel's clock reads now, as Linux's timekeeping reads its current clock
 * source: "tsc" while the clock runs from the TSC (`vclock_running`), which
 * it does where the TSC runs at one rate and every processor's agrees,
 * Linux's rule for a stable TSC; else the reference that keeps the clock,
 * as `timer_reference` names it, or "jiffies".
 */
const char *timer_clocksource(int n);

#endif /* _TIMER_H_ */
