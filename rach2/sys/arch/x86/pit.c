#include "portio.h"
#include "pit.h"

#include "kcpu.h"
#include "../../lib/kirq.h"
#include "../../udelay.h"

uint32_t pit_reload = 1193182 / 100;

/*
 * Microseconds per count as the counter is READ in mode 3, where it steps by
 * two per input clock: 1000000 / (2 * 1193182) = 0.419047. Kept as 419
 * thousandths so the arithmetic stays in 32 bits; the error is one part in
 * ten thousand, which is a fifth of a microsecond over a whole sweep.
 */
#define US_PER_COUNT_THOUSANDTHS 419u

/**
 * Program the 8254 PIT channel 0 to raise IRQ0 at `frequency` Hz.
 *
 * Channel 0 is the system timer line (IRQ0, vector 32); the divisor scales the
 * fixed 1.193182 MHz input clock down to the requested rate. The caller
 * installs the IRQ0 handler.
 *
 * The control byte selects mode 3, the square wave generator, and `udelay.c`
 * converts counts to microseconds on the strength of it: in mode 3 the counter
 * steps by two per input clock. Changing the mode here changes that arithmetic
 * and must change it there too.
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
 * Neither the tick nor the counter can carry this alone. The counter says
 * where in a period the machine is and says nothing about which period, and
 * two readings a whole period apart land in the same place; the tick counts
 * periods exactly but arrives when it arrives, which under QEMU is a
 * millisecond or two into the period it counts. So a reading that finds the
 * counter behind where the last reading left it counts the wrap itself, and
 * the tick counts one only when no reading beat it to it.
 */
struct pit_clock {
    uint32_t periods;    /* whole periods counted since the boot */
    uint32_t last_phase; /* where the last reading or tick found the counter */
    int wrapped;         /* a reading counted the period the next tick sees */
};

static struct pit_clock clock;

static uint32_t pit_phase_from(uint32_t count, uint8_t status);

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
 * The TSC's measure of the ticks. A tick arrives only while interrupts are
 * on, and the PIC keeps one pending, so a stretch with them off longer than
 * a period loses periods, as a page fault that reclaims memory can take;
 * the time-stamp counter goes on counting. With the counts of one tick, from
 * `timer_calibrate`, the timer handler asks how many whole ticks have gone
 * by since the last one it saw (`timer_lost_ticks`), rounding down, so the
 * count can fall short of the time by part of a tick in a gap and never run
 * ahead of it. Zero counts: no TSC that runs at one rate, or a hypervisor,
 * whose emulated timer delivers the ticks it held back again, and no ticks
 * are made up.
 */
static uint32_t tsc_per_tick;
static uint32_t tsc_last_lo, tsc_last_hi;

/* The most ticks one interrupt makes up: a minute. */
#define PIT_LOST_MAX 6000u

/** The time-stamp counter's halves. */
static void tsc_read(uint32_t *lo, uint32_t *hi)
{
#ifndef RACH2_HOSTTEST
    __asm__ __volatile__("rdtsc" : "=a"(*lo), "=d"(*hi));
#else
    *lo = *hi = 0;
#endif
}

/**
 * The whole ticks of `per_tick` counts in the TSC interval `hi`:`lo` less
 * the one that ends it, at most PIT_LOST_MAX: the ticks the timer missed.
 */
static uint32_t pit_ticks_missed(uint32_t lo, uint32_t hi, uint32_t per_tick)
{
    uint32_t q;

    if (per_tick == 0) {
        return 0;
    }
    if (hi >= per_tick) {
        return PIT_LOST_MAX; /* a quotient past 32 bits is past the bound too */
    }
#ifndef RACH2_HOSTTEST
    __asm__("divl %2" : "=a"(q) : "0"(lo), "rm"(per_tick), "d"(hi) : "cc");
#else
    q = (uint32_t)((((uint64_t)hi << 32) | lo) / per_tick);
#endif
    if (q <= 1u) {
        return 0;
    }
    return q - 1u > PIT_LOST_MAX ? PIT_LOST_MAX : q - 1u;
}

uint32_t timer_calibrate(void)
{
    uint32_t a, b, c, d, lo0, hi0, lo1, hi1, flags;

#ifdef RACH2_HOSTTEST
    return 0; /* a host has its own clocks */
#endif
    kcpu_id(1u, &a, &b, &c, &d);
    if (c & (1u << 31)) {
        return 0; /* a hypervisor gives the ticks it held back again itself */
    }
    kcpu_id(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000007u) {
        return 0;
    }
    kcpu_id(0x80000007u, &a, &b, &c, &d);
    if (!(d & (1u << 8))) {
        return 0; /* no invariant TSC: its rate follows the processor's */
    }
    flags = kirq_save();
    tsc_read(&lo0, &hi0);
    k_udelay(20000);
    tsc_read(&lo1, &hi1);
    kirq_restore(flags);
    /* Twenty milliseconds are two ticks, and fewer than 2^32 counts below a
     * clock of 200 GHz. */
    tsc_per_tick = (lo1 - lo0) / 2u;
    return tsc_per_tick;
}

uint32_t timer_lost_ticks(void)
{
    uint32_t lo, hi, gap_lo, gap_hi, first = tsc_last_lo == 0 && tsc_last_hi == 0;

    if (tsc_per_tick == 0) {
        return 0;
    }
    tsc_read(&lo, &hi);
    gap_lo = lo - tsc_last_lo;
    gap_hi = hi - tsc_last_hi - (lo < tsc_last_lo ? 1u : 0u);
    tsc_last_lo = lo;
    tsc_last_hi = hi;
    return first ? 0 : pit_ticks_missed(gap_lo, gap_hi, tsc_per_tick);
}

/**
 * The clock's arithmetic on fixed inputs, as `pit.h` describes.
 *
 * The rows: the period of two sweeps agrees with the reload and the
 * crystal to a hundredth; a count at the reload is the start of the period
 * and, with OUT low, the start of the second sweep; a count a quarter in
 * reads a quarter of a sweep; a count above the reload reads as the start;
 * two readings inside one period add no period; a tick with no reading
 * ahead of it adds one; a reading behind the last one counts the wrap and
 * the tick after it adds nothing; and the next tick, with no wrap read
 * before it, adds its period. The ticks the TSC finds missed: none in a gap
 * of one tick or a little more, eight in one of nine and a half, none
 * without a calibration, and the bound for a gap whose quotient is past 32
 * bits.
 */
int timer_clock_check(void)
{
    uint32_t p = 2u * sweep_us();
    uint32_t want = pit_reload * 8381u / 10000u; /* 1193182 counts a second */
    uint32_t quarter = pit_phase_from(pit_reload - pit_reload / 4u, 0x80);
    struct pit_clock c;
    int ok = 1;

    ok = ok && p >= want - want / 100u && p <= want + want / 100u;
    ok = ok && pit_phase_from(pit_reload, 0x80) == 0u;
    ok = ok && pit_phase_from(pit_reload, 0x00) == sweep_us();
    ok = ok && quarter + 2u >= sweep_us() / 4u && quarter <= sweep_us() / 4u + 2u;
    ok = ok && pit_phase_from(pit_reload + 5u, 0x80) == 0u;
    c.periods = 0;
    c.last_phase = 0;
    c.wrapped = 0;
    ok = ok && pit_clock_read(&c, 3000u, p) == 3000u;
    ok = ok && pit_clock_read(&c, 8000u, p) == 8000u;
    pit_clock_tick(&c, 100u);
    ok = ok && pit_clock_read(&c, 500u, p) == p + 500u;
    ok = ok && pit_clock_read(&c, 50u, p) == 2u * p + 50u;
    pit_clock_tick(&c, 120u);
    ok = ok && pit_clock_read(&c, 900u, p) == 2u * p + 900u;
    pit_clock_tick(&c, 30u);
    ok = ok && pit_clock_read(&c, 40u, p) == 3u * p + 40u;
    ok = ok && pit_ticks_missed(1000u, 0u, 1000u) == 0u;
    ok = ok && pit_ticks_missed(1900u, 0u, 1000u) == 0u;
    ok = ok && pit_ticks_missed(9500u, 0u, 1000u) == 8u;
    ok = ok && pit_ticks_missed(9500u, 0u, 0u) == 0u;
    ok = ok && pit_ticks_missed(0u, 5000u, 1000u) == PIT_LOST_MAX;
    ok = ok && pit_ticks_missed(0u, 1u, 1000u) == PIT_LOST_MAX;
    return ok;
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

/** Where the counter is inside its period now, in microseconds. */
static uint32_t clock_phase(void)
{
    uint8_t status, lo, hi;

    outb(0x43, 0xC2); /* latch channel 0 status and count together */
    status = inb(0x40);
    lo = inb(0x40);
    hi = inb(0x40);
    return pit_phase_from(((uint32_t)hi << 8) | (uint32_t)lo, status);
}

/**
 * Count one period, from the timer handler and nowhere else.
 *
 * The tick counts the period unless a reading has already counted it, which
 * is what `clock_wrapped` says. Both counting it would put the clock ten
 * milliseconds ahead once a period.
 */
void timer_tick_clock(void)
{
    uint32_t flags = kirq_save();

    pit_clock_tick(&clock, clock_phase());
    kirq_restore(flags);
}

/**
 * Microseconds since the boot, from the tick and the counter under it.
 *
 * The tick alone has 10 ms of resolution, which is coarser than the intervals
 * the USB interrupt path is measured over, so this reads where channel 0 is
 * inside its period as well. Mode 3 sweeps the counter to zero twice per
 * interrupt, so the count says where in a half-period the machine is and the
 * read-back status says which half: `OUT` is high through the first.
 *
 * A machine whose interrupts are off for longer than a period loses that
 * period unless something reads the clock inside it. The tick counter makes
 * such periods up from the TSC where `timer_calibrate` found one at one
 * rate; this clock does not.
 *
 * It wraps after about 71 minutes, which is a diagnostic clock's bound and
 * not a wall clock's; `rtc.c` is where the time of day lives.
 */
uint32_t timer_now_us(void)
{
    uint32_t flags = kirq_save();
    uint32_t now = pit_clock_read(&clock, clock_phase(), 2u * sweep_us());

    kirq_restore(flags);
    return now;
}

/**
 * Nanoseconds within the current tick, with interrupts already disabled.
 *
 * The status and count must describe the same instant or a half-period wrap
 * can combine the old OUT bit with the new count. Use the same latched phase
 * as the diagnostic clock; its microsecond resolution fits in 32 bits.
 */
uint32_t timer_subtick_ns(void)
{
    return clock_phase() * 1000u;
}

/** The tick's interrupt, IRQ 0, raised at the 8259 and not yet taken. */
int timer_tick_pending(void)
{
    return pic_irq_pending(0);
}
