#include "pit.h"

#include "lib/kirq.h"

/* The tick, which is this chip's own count of the interrupts it has raised. */
extern volatile uint32_t kernel_ticks;

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
    outb(0x40, (uint8_t) (divisor & 0xFF));
    outb(0x40, (uint8_t) ((divisor >> 8) & 0xFF));
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
 * millisecond or two into the period it names. So a reading that finds the
 * counter behind where the last reading left it counts the wrap itself, and
 * the tick counts one only when no reading beat it to it.
 */
static uint32_t clock_periods;
static uint32_t clock_last_phase;
static int clock_wrapped;

/** Where the counter is inside its period, in microseconds. */
static uint32_t clock_phase(void)
{
    uint32_t count, phase;
    uint8_t status, lo, hi;

    outb(0x43, 0xE2); /* read back channel 0's status, leaving the count */
    status = inb(0x40);
    outb(0x43, 0x00); /* latch the count */
    lo = inb(0x40);
    hi = inb(0x40);

    count = ((uint32_t)hi << 8) | (uint32_t)lo;
    if (count > pit_reload) {
        count = pit_reload; /* a reload seen mid-latch reads as the start */
    }
    phase = (pit_reload - count) * US_PER_COUNT_THOUSANDTHS / 1000u;
    if ((status & 0x80u) == 0) {
        phase += sweep_us(); /* OUT is low through the second half */
    }
    return phase;
}

/**
 * Count one period, from the timer handler and nowhere else.
 *
 * The tick counts the period unless a reading has already counted it, which
 * is what `clock_wrapped` says. Both counting it would put the clock ten
 * milliseconds ahead once a period.
 */
void pit_tick_clock(void)
{
    uint32_t flags = kirq_save();
    uint32_t phase = clock_phase();

    if (!clock_wrapped) {
        clock_periods++;
    }
    clock_wrapped = 0;
    clock_last_phase = phase;
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
 * period unless something reads the clock inside it, exactly as the tick
 * counter does.
 *
 * It wraps after about 71 minutes, which is a diagnostic clock's bound and
 * not a wall clock's; `rtc.c` is where the time of day lives.
 */
uint32_t pit_now_us(void)
{
    uint32_t flags = kirq_save();
    uint32_t phase = clock_phase();
    uint32_t periods;

    if (phase < clock_last_phase) {
        clock_periods++;
        clock_wrapped = 1;
    }
    clock_last_phase = phase;
    periods = clock_periods;
    kirq_restore(flags);
    return periods * 2u * sweep_us() + phase;
}
