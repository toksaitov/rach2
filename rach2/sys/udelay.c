/*
 * udelay.c: short waits measured against the 8254 counter.
 *
 * The device bring-up sequences in this kernel ask for waits in
 * microseconds and milliseconds, and every one of them is a hardware
 * requirement, not a convenience: a wait that comes up short leaves a
 * chip half-configured in a way that shows on one machine and not another. A
 * loop counting instructions cannot promise a duration on a processor whose
 * speed is unknown, so this measures against the timer chip, which runs at
 * 1.193182 MHz on every PC regardless.
 *
 * Channel 0 is the one the tick already programmed. Reading it with a
 * latch command samples the count without changing it, so this neither
 * disturbs the tick nor needs interrupts, which matters because most callers
 * run during boot with interrupts off.
 *
 * The mode matters to the arithmetic. `init_pit` writes control byte 0x36,
 * which selects mode 3, the square wave generator, and in mode 3 the counter
 * is decremented by TWO on each input clock: it sweeps the reload value down
 * to zero twice per interrupt period, not once. So one count is half an
 * input clock, and the conversion below uses twice the input frequency. An odd
 * reload (11931 here) makes the two half sweeps differ by one count, which is
 * under one part in ten thousand and is ignored.
 */

#include "udelay.h"
#include "kio.h"
#include "pit.h"

/* The 8254's input frequency. Where the counter wraps is `pit_reload`, which
 * init_pit publishes, not this file computing the divisor a second
 * time: two copies would agree today and disagree the day the tick rate
 * changes. */
#define PIT_HZ 1193182u

/* Counts per second as READ BACK, which is twice the input clock in mode 3. */
#define PIT_COUNTS_PER_SEC (2u * PIT_HZ)

/*
 * The longest single wait this measures in one pass. The bound is arithmetic,
 * not physical: `usec * PIT_COUNTS_PER_SEC` must stay inside 32 bits, and 1000
 * leaves it at about 2.4 billion of the 4.29 billion available. Longer waits
 * are made of repeated passes, and nothing is lost between passes because each
 * pass measures its own counts. What must stay short is the interval between
 * two SAMPLES, not the total: the counter sweeps every 5 ms, and a gap longer
 * than that would hide a whole sweep. The loop below samples every few port
 * reads, so that has orders of magnitude of margin.
 */
#define PIT_WINDOW_USEC 1000u

/** Sample channel 0's current count without disturbing it. */
static uint16_t pit_count(void)
{
    uint8_t lo, hi;

    outb(0x43, 0x00); /* latch channel 0 */
    lo = inb(0x40);
    hi = inb(0x40);
    return (uint16_t) ((uint16_t) lo | ((uint16_t) hi << 8));
}

/**
 * Wait `usec` microseconds, which must be at most `PIT_WINDOW_USEC`.
 *
 * The counter descends, so the elapsed count between two samples is the drop;
 * when the counter has reloaded since the last sample the drop is taken around
 * the reload value instead. Sampling faster than the counter moves is
 * harmless, since a zero drop simply adds nothing.
 */
static void udelay_window(uint32_t usec)
{
    uint32_t want = (usec * PIT_COUNTS_PER_SEC + 999999u) / 1000000u;
    uint32_t seen = 0;
    uint16_t prev = pit_count();

    if (want == 0) {
        want = 1;
    }
    while (seen < want) {
        uint16_t now = pit_count();
        uint32_t step;

        if (now <= prev) {
            step = (uint32_t) (prev - now);
        } else {
            /* The counter reloaded between the two samples. */
            step = (uint32_t) prev + (pit_reload - (uint32_t) now);
        }
        seen += step;
        prev = now;
    }
}

void k_udelay(uint32_t usec)
{
    while (usec > PIT_WINDOW_USEC) {
        udelay_window(PIT_WINDOW_USEC);
        usec -= PIT_WINDOW_USEC;
    }
    if (usec > 0) {
        udelay_window(usec);
    }
}
