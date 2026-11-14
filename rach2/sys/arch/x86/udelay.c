/*
 * udelay.c: short waits measured against the 8254 counter, or, on a machine
 * with none, by the HPET or the ACPI PM timer through `udelay_wait`, which
 * the platform sets (platform.c, refclock.c), or by the TSC once it keeps the
 * clock (pit.c).
 *
 * The device bring-up sequences in this kernel ask for waits in
 * microseconds and milliseconds, and each is a hardware requirement: a wait
 * that comes up short leaves a chip half-configured in a way that shows on
 * one machine and not another. A loop counting instructions cannot promise
 * a duration on a processor whose speed is unknown, so this measures against
 * the timer chip, which runs at 1.193182 MHz on every PC.
 *
 * Channel 0 is the one the tick already programmed. Reading it with a
 * latch command samples the count without changing it, so this neither
 * disturbs the tick nor needs interrupts, which matters because most callers
 * run during boot with interrupts off. The latch and its reads are one
 * operation on a chip all processors share, so every reader of channel 0
 * goes through `pit_latch`, which serializes them.
 *
 * The mode matters to the arithmetic. `init_pit` writes control byte 0x36,
 * which selects mode 3, the square wave generator, and in mode 3 the counter
 * is decremented by two on each input clock: it sweeps the reload value down
 * to zero twice in each interrupt period. So one count is half an input
 * clock, and the conversion below uses twice the input frequency. An odd
 * reload (1193 at 1000 Hz) makes the two half sweeps differ by one count,
 * which is under one part in a thousand and is ignored.
 */

#include "portio.h"
#include "../arch.h"
#include "../../udelay.h"
#include "../../kio.h"
#include "../../lib/kirq.h"
#include "pit.h"

/*
 * The longest single wait this measures in one pass. The bound comes from
 * the arithmetic: `usec * PIT_COUNTS_PER_SEC` must stay inside 32 bits, and
 * 1000 leaves it at about 2.4 billion of the 4.29 billion available. Longer
 * waits are made of repeated passes, and nothing is lost between passes
 * because each pass measures its own counts. What must stay short is the
 * interval between two samples: the counter sweeps every half tick, 0.5 ms at
 * 1000 Hz, and a gap longer than that would hide a whole sweep. The loop
 * below samples every few port reads, so that has two orders of magnitude of
 * margin.
 */
#define PIT_WINDOW_USEC 1000u

/*
 * The samples in a row that may find the counter where it was before the
 * wait gives up on it. A counting 8254 moves between any two samples, each
 * three port accesses; a machine without one reads the same 0xFFFF every
 * time, and a wait that went on would never end.
 */
#define PIT_STALL_SAMPLES 100000u

/* The wait that replaces the 8254's, or NULL: `platform_timer_start` sets
 * `refclock_wait_us` here when channel 0 does not count and the HPET or the
 * PM timer does, and the reference's start the TSC's when the TSC keeps the
 * clock. */
uint32_t (*udelay_wait)(uint32_t usec);

/*
 * 1 while a processor is inside a latch of channel 0 (`pit_latch`), else 0.
 * It is taken by exchange with interrupts off and without the lock checker,
 * as Linux's i8253_lock is a raw spinlock, since a processor that stops the
 * others, or one in an NMI handler, may delay on the 8254.
 */
static volatile unsigned int latch_held;

/*
 * The tries `pit_latch` makes at a held latch before it gives up. A holder
 * keeps it for one command and three reads at most; only one that was
 * stopped inside, by the NMI that stops the processors or by one this
 * processor is handling on top of it, keeps it for this long.
 */
#define PIT_LATCH_TRIES 100000u

int pit_latch(uint8_t command, uint8_t *bytes, int n)
{
    uint32_t flags = kirq_save(), tries = 0;
    int i;

    while (arch_atomic_xchg(&latch_held, 1u) != 0) {
        if (++tries == PIT_LATCH_TRIES) {
            kirq_restore(flags);
            return 0;
        }
        arch_relax();
    }
    outb(0x43, command);
    for (i = 0; i < n; i++) {
        bytes[i] = inb(0x40);
    }
    (void)arch_atomic_xchg(&latch_held, 0u);
    kirq_restore(flags);
    return 1;
}

/**
 * Sample channel 0's current count into `*count` without disturbing it:
 * 1, or 0 when the latch could not be had (`pit_latch`).
 *
 * The latch and the two reads are one operation, under `pit_latch`. The
 * tick's clock latches this counter too, and a handler on this processor,
 * or another processor, that latched it between the latch here and the
 * reads would consume the latch, leaving the reads to return the live
 * count a byte at a time: a low byte from before a borrow and a high byte
 * from after it reads as a count below the last sample by a whole 256, the
 * drop is overcounted by that much, and the wait ends early.
 */
static int pit_count(uint16_t *count)
{
    uint8_t b[2];

    if (!pit_latch(0x00, b, 2)) {
        return 0;
    }
    *count = (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    return 1;
}

/**
 * The counter descends, so the elapsed count between two samples is the drop;
 * when the counter has reloaded since the last sample the drop is taken around
 * the reload value instead. Sampling faster than the counter moves is
 * harmless, since a zero drop adds nothing. A counter that does not move for
 * PIT_STALL_SAMPLES samples, or a sample that cannot be taken, ends the
 * wait, which then returns 0.
 */
uint32_t pit_wait_counts(uint32_t counts)
{
    uint32_t seen = 0, still = 0;
    uint16_t prev, now;

    if (!pit_count(&prev)) {
        return 0;
    }
    while (seen < counts) {
        uint32_t step;

        if (!pit_count(&now)) {
            return 0;
        }

        if (now <= prev) {
            step = (uint32_t)(prev - now);
        } else {
            /* The counter reloaded between the two samples. */
            step = (uint32_t)prev + (pit_reload - (uint32_t)now);
        }
        seen += step;
        prev = now;
        still = step == 0 ? still + 1u : 0u;
        if (still >= PIT_STALL_SAMPLES) {
            return 0;
        }
    }
    return seen;
}

/** Wait `usec` microseconds, which must be at most `PIT_WINDOW_USEC`:
 * `usec`, or 0 when the counter stopped first. */
static uint32_t udelay_window(uint32_t usec)
{
    uint32_t want = (usec * PIT_COUNTS_PER_SEC + 999999u) / 1000000u;

    return pit_wait_counts(want == 0 ? 1u : want) != 0 ? usec : 0u;
}

/**
 * Wait `usec` microseconds on channel 0, a run of windows and a last
 * shorter one. A window whose counter stops ends the wait, and the windows
 * waited before it are what the wait returns; the part of that window is
 * not counted, so the return never exceeds the time waited.
 */
uint32_t pit_wait_us(uint32_t usec)
{
    uint32_t done = 0, step;

    while (done < usec) {
        step = usec - done > PIT_WINDOW_USEC ? PIT_WINDOW_USEC : usec - done;
        if (udelay_window(step) == 0) {
            break;
        }
        done += step;
    }
    return done;
}

uint32_t k_udelay_measured(uint32_t usec)
{
    return udelay_wait ? udelay_wait(usec) : pit_wait_us(usec);
}

void k_udelay(uint32_t usec)
{
    (void)k_udelay_measured(usec);
}
