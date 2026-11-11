/*
 * pit.c: the 8254's channel 0, and the clock under the tick that
 * ../../timer.h declares: the ticks each interrupt counts, the microseconds
 * since the boot and the place within the tick, kept by a reference counter.
 */

#include "portio.h"
#include "pit.h"
#include "refclock.h"

#include "kcpu.h"
#include "../arch.h"
#include "../../cmdline.h"
#include "../../vdso.h"
#include "../../lib/kirq.h"
#include "../../lib/spinlock.h"

uint32_t pit_reload = 1193182 / TICK_HZ;

/*
 * Microseconds per count as the counter is read in mode 3, where it steps by
 * two per input clock: 1000000 / (2 * 1193182) = 0.419047. Kept as 419
 * thousandths so the arithmetic stays in 32 bits; the error is one part in
 * ten thousand, which is a fifth of a microsecond over a whole sweep.
 */
#define US_PER_COUNT_THOUSANDTHS 419u

/**
 * The divisor scales the 1.193182 MHz input clock down to the rate. The
 * control byte, 0x36, selects channel 0, both bytes of the count and mode
 * 3, the square wave generator, and `udelay.c` converts counts to
 * microseconds on the strength of it: in mode 3 the counter steps by two
 * per input clock. A change of mode here changes that arithmetic, and
 * `udelay.c` must change with it.
 */
void init_pit(uint32_t frequency)
{
    uint32_t divisor = 1193182 / frequency;

    pit_reload = divisor;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

/** How long one sweep of the counter takes, which is half the tick's period. */
static uint32_t sweep_us(void)
{
    return pit_reload * US_PER_COUNT_THOUSANDTHS / 1000u;
}

/*
 * Where the clock is: the periods that have passed, where the counter was at
 * the last reading, and whether that reading was the one that saw the wrap.
 *
 * Neither the tick nor the counter can keep this alone. The counter says
 * where in a period the machine is and says nothing about which period, and
 * two readings a whole period apart land in the same place; the tick counts
 * periods exactly, but its interrupt comes late, under QEMU a millisecond or
 * two into the period it counts. So a reading that finds the counter behind
 * where the last reading left it counts the wrap itself, and the tick counts
 * one only when no reading counted it first.
 */
struct pit_clock {
    uint32_t periods;    /* whole periods counted since the boot */
    uint32_t last_phase; /* where the last reading or tick found the counter */
    int wrapped;         /* a reading counted the period the next tick sees */
};

/* The boot processor's clock, read and ticked with interrupts off; the
 * other processors read the edge below. */
static struct pit_clock clock;

/*
 * The latest reading `timer_now_us` has given on any processor, below which
 * no later reading goes, and the lock it is read and raised under. The boot
 * processor reads channel 0 and the others the reference counter, and the
 * two disagree by the rounding of their scales and the time between the
 * reads; a TCP round trip timed from one processor's send to another's
 * acknowledgement must not come out negative.
 */
static struct spinlock now_lock;
static uint32_t now_floor;

static uint32_t pit_phase_from(uint32_t count, uint8_t status);
static int pit_pending(void);

/* Channel 0's counter, and the counter the clock reads, channel 0's until
 * `timer_set_counter` chooses another. */
static const struct tick_counter pit_counter = {pit_phase_us, pit_phase_ns, pit_period_us,
                                                pit_pending};
static const struct tick_counter *counter = &pit_counter;

void timer_set_counter(const struct tick_counter *c)
{
    counter = c ? c : &pit_counter;
}

/**
 * A reading that found the counter at `phase` microseconds into its period:
 * a phase behind the last one is the counter wrapped since, which the
 * reading counts so that the tick to come does not count it again. Returns
 * the microseconds since the boot for a period of `period_us`.
 */
static uint32_t pit_clock_read(struct pit_clock *c, uint32_t phase, uint32_t period_us)
{
    if (phase < c->last_phase) {
        c->periods++;
        c->wrapped = 1;
    }
    c->last_phase = phase;
    return c->periods * period_us + phase;
}

/** The tick at `phase`: one period, counted by whichever saw the wrap first. */
static void pit_clock_tick(struct pit_clock *c, uint32_t phase)
{
    if (!c->wrapped) {
        c->periods++;
    }
    c->wrapped = 0;
    c->last_phase = phase;
}

/*
 * The reference that keeps time from the first tick after one is ready: the
 * platform's counter (refclock.h), whose rate the firmware states, or, on a
 * machine with neither the HPET nor the PM timer, the time-stamp counter
 * where it runs at one rate, measured against the 8254 by `timer_calibrate`.
 * Under `rach2.tscreliable` the TSC, which `platform_timer_start` has measured
 * by then, comes first, as Linux's TSC does: under a hypervisor every read of
 * the HPET or the PM timer is an exit, and the tick reads the reference at
 * least once. `k_udelay` then waits on the TSC as well (`tsc_wait_us`).
 *
 * The tick's interrupt then only prompts the count. Each interrupt counts
 * the whole ticks of the reference since the last one counted: one for an
 * interrupt on time; none for one that comes again, as a hypervisor delivers
 * the ticks it held back; and all that went by for one that an emulator
 * delayed, or that a stretch with interrupts off made the PIC hold, which
 * keeps one pending however many were raised. The place within the tick is
 * read from the reference too, so the count and the place cannot disagree.
 *
 * A count is kept in `mult` units, so that a tick is a whole number of them,
 * `tick`: at 1000 Hz the PM timer's 3579545 Hz is 3579.545 counts a tick, and
 * 3579545 units of a thousandth of a count. Where the tick in progress began
 * is `edge` counts and `frac` units past them, below `mult`.
 */
struct ref_clock {
    uint32_t mult;  /* units in a count: TICKS_PER_SECOND, or 1 for the TSC */
    uint32_t tick;  /* units in a tick */
    uint64_t edge;  /* the count where the tick in progress began */
    uint32_t frac;  /* the units past `edge` where it began */
    uint32_t ticks; /* ticks counted since the reference started */
};

#define REF_NONE    0
#define REF_COUNTER 1
#define REF_TSC     2

/* The tick's rate, TICK_HZ in timer.h, and its length. */
#define TICKS_PER_SECOND ((uint32_t)TICK_HZ)
#define US_PER_TICK      ((uint32_t)TICK_US)
#define NS_PER_TICK      ((uint32_t)TICK_NS)

/* The most ticks one interrupt counts, a minute's; the next counts on. */
#define REF_DUE_MAX (60u * TICKS_PER_SECOND)

/*
 * The reference, REF_NONE until it starts; its clock; the platform
 * counter's reading at the last tick and its counts from the start to that
 * reading, which extend the counter past its wrap (a stretch of 4.7 s
 * without a tick loses the PM timer's wraps, 43 s the HPET's at 100 MHz);
 * and `timer_now_us` when it started. The boot processor's tick writes them
 * within the tick's sequence count (../../timer.h).
 */
static volatile int ref_kind;
static volatile struct ref_clock ref;
static volatile uint32_t ref_raw;
static volatile uint64_t ref_total;
static volatile uint32_t ref_us_base;

/* The TSC's counts in a tick, from `timer_calibrate`, 0 for none; and
 * whether it has measured. */
static uint32_t tsc_per_tick;
static int tsc_measured;

/* The waits on the 8254 that `timer_calibrate` measures the TSC over, and
 * the 8254's counts in each, 20 ms. */
#define TSC_WINDOWS     3u
#define TSC_WAIT_COUNTS (PIT_COUNTS_PER_SEC / 50u)

#ifdef RACH2_HOSTTEST
void host_tsc_read(uint32_t *lo, uint32_t *hi);
#endif

/** The time-stamp counter's halves into `*lo` and `*hi`, from the model's
 * `host_tsc_read` in a host test. */
static void tsc_read(uint32_t *lo, uint32_t *hi)
{
#ifndef RACH2_HOSTTEST
    __asm__ __volatile__("rdtsc" : "=a"(*lo), "=d"(*hi));
#else
    host_tsc_read(lo, hi);
#endif
}

/** `n` divided by `d`, at most REF_DUE_MAX, with the remainder in `*rem`, or
 * 0 there when the quotient was cut to the bound. */
static uint32_t ref_divide(uint64_t n, uint32_t d, uint32_t *rem)
{
    uint32_t q, r;

    if ((uint32_t)(n >> 32) >= d) {
        *rem = 0;
        return REF_DUE_MAX; /* a quotient past 32 bits is past the bound too */
    }
#if defined(__x86_64__) || defined(RACH2_HOSTTEST)
    q = (uint32_t)(n / d);
    r = (uint32_t)(n % d);
#else
    __asm__("divl %4"
            : "=a"(q), "=d"(r)
            : "0"((uint32_t)n), "1"((uint32_t)(n >> 32)), "rm"(d)
            : "cc");
#endif
    if (q > REF_DUE_MAX) {
        *rem = 0;
        return REF_DUE_MAX;
    }
    *rem = r;
    return q;
}

/**
 * The whole ticks of `r` from where the tick in progress began to the
 * reference's `count`, at most REF_DUE_MAX, and the units into the next in
 * `*rem`. A count short of the edge, as the count `edge` itself is when
 * `frac` is not 0, is no tick and none into one.
 */
static uint32_t ref_due(const volatile struct ref_clock *r, uint64_t count, uint32_t *rem)
{
    uint64_t gone = count - r->edge, units = gone * r->mult;

    if ((gone >> 63) != 0 || units < r->frac) {
        *rem = 0;
        return 0;
    }
    return ref_divide(units - r->frac, r->tick, rem);
}

/** Move where the tick in progress began on by `n` ticks. */
static void ref_advance(volatile struct ref_clock *r, uint32_t n)
{
    r->ticks += n;
    while (n-- > 0) {
        r->frac += r->tick;
        r->edge += r->frac / r->mult;
        r->frac %= r->mult;
    }
}

/** The reference's count now, from the TSC or from the platform's counter's
 * counts since the last tick added to its total then; `*raw` is what the
 * counter read. */
static uint64_t ref_count(uint32_t *raw)
{
    uint32_t hi;

    if (ref_kind == REF_TSC) {
        tsc_read(raw, &hi);
        return ((uint64_t)hi << 32) | *raw;
    }
    *raw = refclock_read();
    return ref_total + ((*raw - ref_raw) & refclock_mask());
}

/** The TSC's counts now as one 64-bit number. */
static uint64_t tsc_now(void)
{
    uint32_t lo, hi;

    tsc_read(&lo, &hi);
    return ((uint64_t)hi << 32) | lo;
}

/**
 * Wait `usec` microseconds on the TSC, 100 at a time so that the counts of a
 * step stay inside 32 bits at any rate below 42 GHz; each step waits the
 * counts that cover it at least. `k_udelay`'s wait once the TSC is the
 * reference, as Linux's `delay_tsc`.
 */
static void tsc_wait_us(uint32_t usec)
{
    while (usec > 0) {
        uint32_t step = usec > 100u ? 100u : usec;
        uint32_t want = (step * tsc_per_tick + US_PER_TICK - 1u) / US_PER_TICK;
        uint64_t start = tsc_now();

        while (tsc_now() - start < want) {
            arch_relax();
        }
        usec -= step;
    }
}

/**
 * Start the reference at this tick, which the old count has counted, or
 * return 0 when none is ready. The tick in progress is put to have begun half
 * a tick ago, so that an interrupt early or late by less than half a tick
 * counts the one tick it was raised for, and the microsecond clock goes on
 * from where it is.
 */
static int ref_start(void)
{
    uint32_t raw, now_us;

    if (tsc_per_tick && (!refclock_name() || kcmdline_has("rach2.tscreliable"))) {
        ref.mult = 1;
        ref.tick = tsc_per_tick;
        ref_kind = REF_TSC;
        if (!udelay_wait) {
            udelay_wait = tsc_wait_us;
        }
    } else if (refclock_name()) {
        ref.mult = TICKS_PER_SECOND;
        ref.tick = refclock_hz();
        ref_raw = refclock_read();
        ref_total = 0;
        ref_kind = REF_COUNTER;
    } else {
        return 0;
    }
    now_us = pit_clock_read(&clock, counter->phase_us(), counter->period_us());
    ref.edge = ref_count(&raw) - ref.tick / ref.mult / 2u;
    ref.frac = 0;
    ref.ticks = 0;
    ref_us_base = now_us - US_PER_TICK / 2u;
    return 1;
}

/** Whether `cpuid` says the TSC runs at one rate whatever the processor's
 * clock does (leaf 0x80000007, EDX bit 8), as on a PC it does. */
static int tsc_one_rate(void)
{
    uint32_t a, b, c, d;

    kcpu_id(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000007u) {
        return 0;
    }
    kcpu_id(0x80000007u, &a, &b, &c, &d);
    return (d & (1u << 8)) != 0;
}

/** `gone` TSC counts over `seen` counts of a reference of `ref_hz` as the
 * TSC's counts in a tick, from a quarter of the counts a second; 0 when the
 * quotient does not fit. */
static uint32_t tsc_per_tick_of(uint64_t gone, uint32_t seen, uint32_t ref_hz)
{
    uint32_t counts;

    if (seen == 0 || (gone >> 32) != 0) {
        return 0; /* 20 ms is under 2^32 counts below a 200 GHz clock */
    }
    counts = refclock_scale((uint32_t)gone, ref_hz / 4u, seen);
    return counts == 0xFFFFFFFFu ? 0 : counts / (TICKS_PER_SECOND / 4u);
}

/**
 * The TSC's fewest counts in a tick of TSC_WINDOWS waits of 20 ms on the
 * 8254, each measured in the 8254's counts (`pit_wait_counts`). A stop of
 * the processor during a wait can hide sweeps of the 8254's counter, which
 * makes the TSC's counts too many for the wait and never too few.
 */
static uint32_t tsc_against_pit(void)
{
    uint32_t seen, counts, least = 0, i;
    uint64_t t0;

    for (i = 0; i < TSC_WINDOWS; i++) {
        t0 = tsc_now();
        seen = pit_wait_counts(TSC_WAIT_COUNTS);
        counts = tsc_per_tick_of(tsc_now() - t0, seen, PIT_COUNTS_PER_SEC);
        if (counts != 0 && (least == 0 || counts < least)) {
            least = counts;
        }
    }
    return least;
}

/**
 * The TSC's counts in a tick over the window of TSC_WINDOWS whose readings
 * of the platform's counter were closest to the TSC's, each window 20 ms of
 * that counter. Each end reads the TSC on both sides of the counter's read
 * and takes the middle, so a read held up, as an emulated device's is, adds
 * to the spread and not to the count; the counter counts on while the
 * processor is stopped, so no window needs the least. A counter that does
 * not move in 2^26 reads gives 0.
 */
static uint32_t tsc_against_reference(void)
{
    uint32_t hz = refclock_hz(), mask = refclock_mask(), want = hz / 50u;
    uint32_t r0, r1, seen = 0, spins, best = 0, i;
    uint64_t a0, b0, a1, b1, spread, best_spread = 0;

    for (i = 0; i < TSC_WINDOWS; i++) {
        a0 = tsc_now();
        r0 = refclock_read();
        b0 = tsc_now();
        spins = 0;
        do {
            a1 = tsc_now();
            r1 = refclock_read();
            b1 = tsc_now();
            seen = (r1 - r0) & mask;
        } while (seen < want && ++spins < (1u << 26));
        if (seen < want) {
            return 0;
        }
        spread = (b0 - a0) + (b1 - a1);
        if (best == 0 || spread < best_spread) {
            best = tsc_per_tick_of(((a1 + b1) >> 1) - ((a0 + b0) >> 1), seen, hz);
            best_spread = spread;
        }
    }
    return best;
}

/**
 * Measure the TSC against the active reference, the HPET or the PM timer,
 * or against the 8254 on a machine with neither, once, with interrupts off.
 * It is measured only where `cpuid` says it runs at one rate or
 * `rach2.tscreliable` says to take it as one that does, as QEMU's emulator
 * needs, which reports no such counter, and never with `rach2.notsc` on the
 * command line. The TSC's difference is taken in
 * 64 bits, so that a long one is not taken for a short one.
 */
uint32_t timer_calibrate(void)
{
    uint32_t flags;

    if (tsc_measured) {
        return tsc_per_tick;
    }
    tsc_measured = 1;
    if (kcmdline_has("rach2.notsc") || !(tsc_one_rate() || kcmdline_has("rach2.tscreliable"))) {
        return 0;
    }
    flags = kirq_save();
    tsc_per_tick = refclock_name() ? tsc_against_reference() : tsc_against_pit();
    kirq_restore(flags);
    return tsc_per_tick;
}

uint32_t tsc_counts_per_tick(void)
{
    return tsc_per_tick;
}

/** Scale cycles by the measured rate, without i686's absent libgcc divide. */
uint64_t timer_cycles_us(uint64_t cycles)
{
    uint64_t ticks = 0, rem = 0;
    int bit;

    if (!tsc_per_tick) {
        return 0;
    }
    for (bit = 63; bit >= 0; --bit) {
        rem = (rem << 1) | ((cycles >> bit) & 1u);
        if (rem >= tsc_per_tick) {
            rem -= tsc_per_tick;
            ticks |= (uint64_t)1 << bit;
        }
    }
    return ticks * TICK_US + refclock_scale((uint32_t)rem, TICK_US, tsc_per_tick);
}

int timer_tick_shared(void)
{
    return ref_kind != REF_NONE;
}

const char *timer_reference(void)
{
    if (ref_kind == REF_COUNTER) {
        return refclock_name();
    }
    return ref_kind == REF_TSC || tsc_per_tick ? "tsc" : 0;
}

/** Linux's name for the reference `name`, as refclock.h names it. */
static const char *clocksource_name(const char *name)
{
    return name[0] == 'p' ? "acpi_pm" : name;
}

const char *timer_clocksource(int n)
{
    const char *now = timer_reference();

    if (n < 0) {
        if (vclock_running()) {
            return "tsc";
        }
        return now ? clocksource_name(now) : "jiffies";
    }
    if (tsc_per_tick && n-- == 0) {
        return "tsc";
    }
    if (refclock_name() && n-- == 0) {
        return clocksource_name(refclock_name());
    }
    return n == 0 && !tsc_per_tick && !refclock_name() ? "jiffies" : 0;
}

/**
 * The phase rows of `timer_clock_check`: the period of two sweeps agrees
 * with the reload and the crystal to a hundredth; a count at the reload is
 * the start of the period and, with OUT low, the start of the second sweep;
 * a count a quarter in reads a quarter of a sweep; and a count above the
 * reload reads as the start.
 */
static int timer_phase_rows(uint32_t p)
{
    uint32_t want = pit_reload * 8381u / 10000u; /* 1193182 counts a second */
    uint32_t quarter = pit_phase_from(pit_reload - pit_reload / 4u, 0x80);
    int ok = 1;

    ok = ok && p >= want - want / 100u && p <= want + want / 100u;
    ok = ok && pit_phase_from(pit_reload, 0x80) == 0u;
    ok = ok && pit_phase_from(pit_reload, 0x00) == sweep_us();
    ok = ok && quarter + 2u >= sweep_us() / 4u && quarter <= sweep_us() / 4u + 2u;
    ok = ok && pit_phase_from(pit_reload + 5u, 0x80) == 0u;
    return ok;
}

/**
 * The clock rows of `timer_clock_check`, for a period of `p` microseconds,
 * with phases in thousandths of it: two readings inside one period add no
 * period; a tick with no reading ahead of it adds one; a reading behind the
 * last one counts the wrap and the tick after it adds nothing; and the next
 * tick, with no wrap read before it, adds its period.
 */
static int timer_count_rows(uint32_t p)
{
    struct pit_clock c;
    int ok = 1;

    c.periods = 0;
    c.last_phase = 0;
    c.wrapped = 0;
    ok = ok && pit_clock_read(&c, p * 300u / 1000u, p) == p * 300u / 1000u;
    ok = ok && pit_clock_read(&c, p * 800u / 1000u, p) == p * 800u / 1000u;
    pit_clock_tick(&c, p * 10u / 1000u);
    ok = ok && pit_clock_read(&c, p * 50u / 1000u, p) == p + p * 50u / 1000u;
    ok = ok && pit_clock_read(&c, p * 5u / 1000u, p) == 2u * p + p * 5u / 1000u;
    pit_clock_tick(&c, p * 12u / 1000u);
    ok = ok && pit_clock_read(&c, p * 900u / 1000u, p) == 2u * p + p * 900u / 1000u;
    pit_clock_tick(&c, p * 3u / 1000u);
    ok = ok && pit_clock_read(&c, p * 4u / 1000u, p) == 3u * p + p * 4u / 1000u;
    return ok;
}

/**
 * The rows of `timer_clock_check` for the reference, with the PM timer's
 * 35795.45 counts a tick of 100 Hz kept in hundredths of a count: short of a
 * tick, none, and a count past it, one; the edge then 45 hundredths past its
 * count, so that count is short of it; after two ticks, the edge two ticks
 * on exactly; an interrupt nine and a half ticks late counts nine, the same
 * one again none, with the place unchanged; a gap of 2^40 counts, some 85
 * hours, counts the bound; and a count behind the edge counts none.
 */
static int timer_ref_rows(void)
{
    struct ref_clock r;
    uint32_t rem;
    int ok = 1;

    r.mult = 100u;
    r.tick = 3579545u;
    r.edge = 0;
    r.frac = 0;
    r.ticks = 0;
    ok = ok && ref_due(&r, 35795u, &rem) == 0u && rem == 3579500u;
    ok = ok && ref_due(&r, 35796u, &rem) == 1u && rem == 55u;
    ref_advance(&r, 1u);
    ok = ok && r.edge == 35795u && r.frac == 45u;
    ok = ok && ref_due(&r, 35795u, &rem) == 0u && rem == 0u;
    ok = ok && ref_due(&r, 71591u, &rem) == 1u && rem == 10u;
    ref_advance(&r, 1u);
    ok = ok && r.edge == 71590u && r.frac == 90u && r.ticks == 2u;
    ok = ok && ref_due(&r, 411647u, &rem) == 9u && rem == 1789705u;
    ref_advance(&r, 9u);
    ok = ok && ref_due(&r, 411647u, &rem) == 0u && rem == 1789705u && r.ticks == 11u;
    ok = ok && ref_due(&r, r.edge + ((uint64_t)1 << 40), &rem) == REF_DUE_MAX;
    ok = ok && ref_due(&r, r.edge - 1u, &rem) == 0u && rem == 0u;
    return ok;
}

/** The rows are in three functions: the phase rows, the clock rows and the
 * reference's rows. */
int timer_clock_check(void)
{
    uint32_t p = 2u * sweep_us();

    return timer_phase_rows(p) && timer_count_rows(p) && timer_ref_rows();
}

/**
 * Where the counter is inside its period, in microseconds, from a latched
 * count and the read-back status. Mode 3 sweeps the count to zero twice per
 * period, and OUT is low through the second sweep, which the status says. A
 * count above the reload is a reload seen mid-latch and reads as the start.
 */
static uint32_t pit_phase_from(uint32_t count, uint8_t status)
{
    uint32_t phase;

    if (count > pit_reload) {
        count = pit_reload;
    }
    phase = (pit_reload - count) * US_PER_COUNT_THOUSANDTHS / 1000u;
    if ((status & 0x80u) == 0) {
        phase += sweep_us();
    }
    return phase;
}

uint32_t pit_phase_us(void)
{
    uint8_t status, lo, hi;

    outb(0x43, 0xC2); /* latch channel 0 status and count together */
    status = inb(0x40);
    lo = inb(0x40);
    hi = inb(0x40);
    return pit_phase_from(((uint32_t)hi << 8) | (uint32_t)lo, status);
}

/**
 * Before the reference starts, the tick counts the counter's period unless a
 * reading has already counted it, which is what `clock.wrapped` says;
 * counting it twice would put the clock a period ahead each period. The
 * first tick with a reference ready starts it (`ref_start`); from then on
 * the reference's whole ticks are counted, and the platform's counter's
 * reading is added to its total, so that the next reading counts from here.
 */
uint32_t timer_tick_clock(void)
{
    uint32_t flags = kirq_save();
    uint32_t raw, rem, due;
    uint64_t count;

    if (ref_kind == REF_NONE) {
        pit_clock_tick(&clock, counter->phase_ns() / 1000u);
        (void)ref_start();
        kirq_restore(flags);
        return 1u;
    }
    count = ref_count(&raw);
    if (ref_kind == REF_COUNTER) {
        ref_total = count;
        ref_raw = raw;
    }
    due = ref_due(&ref, count, &rem);
    ref_advance(&ref, due);
    kirq_restore(flags);
    return due;
}

/**
 * The tick alone has its own length's resolution, coarser than the intervals
 * the USB interrupt path is measured over, so this reads the place within the
 * tick as well. Once the reference has started, it is the reference's ticks
 * since it started and its place within the next, on every processor, read
 * within the tick's sequence count.
 *
 * Before then it is the boot processor's periods of the counter under the
 * tick and that counter's place: the local APIC timer's count when that is
 * the tick, and channel 0's otherwise. Mode 3 sweeps channel 0's counter to
 * zero twice per interrupt, so the count says where in a half-period the
 * machine is and the read-back status says which half: `OUT` is high
 * through the first. A stretch with interrupts off longer than a period then
 * loses the periods no reading fell inside.
 *
 * No reading is earlier than one given before on any processor
 * (`now_floor`), so the clock never runs back from one processor to
 * another. It wraps after about 71 minutes, which is a diagnostic clock's
 * bound and not a wall clock's; the time of day is kept by ../../clock.c.
 */
uint32_t timer_now_us(void)
{
    uint32_t flags = kirq_save();
    uint32_t now, seq, raw, rem, due;

    if (ref_kind != REF_NONE) {
        do {
            seq = tick_read_begin();
            due = ref_due(&ref, ref_count(&raw), &rem);
            now = ref_us_base + (ref.ticks + due) * US_PER_TICK +
                  refclock_scale(rem, US_PER_TICK, ref.tick);
        } while (tick_read_retry(seq));
    } else {
        now = pit_clock_read(&clock, counter->phase_us(), counter->period_us());
    }
    spin_lock(&now_lock);
    if ((int32_t)(now - now_floor) < 0) {
        now = now_floor;
    } else {
        now_floor = now;
    }
    spin_unlock(&now_lock);
    kirq_restore(flags);
    return now;
}

uint32_t timer_resolution_ns(void)
{
    if (ref_kind == REF_TSC) {
        return (NS_PER_TICK + ref.tick - 1u) / ref.tick;
    }
    if (ref_kind == REF_COUNTER) {
        uint32_t hz = refclock_hz();

        return 1000000000u / hz + (1000000000u % hz != 0u);
    }
    return 1000u;
}

uint32_t pit_period_us(void)
{
    return 2u * sweep_us();
}

uint32_t pit_phase_ns(void)
{
    return pit_phase_us() * 1000u;
}

/** The tick's interrupt, IRQ 0, raised at the 8259 and not yet taken. */
static int pit_pending(void)
{
    return pic_irq_pending(0);
}

/**
 * Once the reference has started, the ticks it has gone past the tick in
 * progress and its place within the next. Before then, the counter's pending
 * bit read on both sides of the latch of its place, and the latch again when
 * the bit changed between, so that the place belongs to the tick the bit
 * says.
 */
void timer_phase(int *pending, uint32_t *phase_ns)
{
    uint32_t raw, rem;

    if (ref_kind != REF_NONE) {
        *pending = (int)ref_due(&ref, ref_count(&raw), &rem);
        *phase_ns = refclock_scale(rem, NS_PER_TICK, ref.tick);
        return;
    }
    *pending = counter->pending();
    *phase_ns = counter->phase_ns();
    if (*pending != counter->pending()) {
        *pending = 1;
        *phase_ns = counter->phase_ns();
    }
}

/**
 * Read channel 0 until its count changes, a few thousand times at most. A
 * counting 8254 moves between two readings; an absent one reads 0xFFFF
 * every time.
 */
int pit_probe(void)
{
    uint32_t flags = kirq_save();
    uint16_t first = 0, now = 0;
    int i;

    for (i = 0; i < 4096; i++) {
        uint8_t lo, hi;

        outb(0x43, 0x00); /* latch channel 0 */
        lo = inb(0x40);
        hi = inb(0x40);
        now = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
        if (i == 0) {
            first = now;
        } else if (now != first) {
            break;
        }
    }
    kirq_restore(flags);
    return now != first;
}
