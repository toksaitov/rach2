/**
 * The microsecond wait, driven on the host against a model of the 8254
 * (sys/arch/x86/udelay.c).
 *
 * The file is included whole so the sample the wait takes of the counter is
 * reachable. The model is channel 0 in mode 3: a count that descends by a
 * few units at every port access, since the counter runs while the processor
 * talks to it, and reloads at zero; a latch that holds the count for the two
 * reads that follow; and reads with no latch pending that return the live
 * count a byte at a time, which is what the chip does.
 *
 * The fault the test looks for is an interrupt handler that latches the
 * counter itself, as the tick's clock and the USB handler's stamps do,
 * running between the wait's latch and its reads. The model runs one
 * whenever a latch is pending at a read taken with interrupts on: the
 * handler takes the latch, and the wait's reads come back live and torn
 * across a borrow, 256 below the true count. The wait would then count a
 * larger drop than the counter made and end early. With the sample taken
 * with interrupts off the handler cannot run between the latch and the
 * reads, and the wait takes its full time whatever is armed.
 *
 * host-deps: tests/host/compat/atomic_host.c
 *
 * Interrupts off do not keep out another processor, which latches channel 0
 * as it delays too. The model runs one whenever a latch is pending at a read
 * and `pit_latch`'s lock is free, as it would be with no lock, with the same
 * torn reads; with the lock it waits until the wait's reads are done. A
 * latch whose holder was stopped inside it, from the start of the wait or
 * from its middle, leaves the rest of the wait to the HPET or the PM timer,
 * modelled as a counter that runs or does not, after PIT_LATCH_TRIES tries;
 * the wait lasts and returns all it was asked while that counter runs, and
 * ends at once, with what it measured and the chip untouched since, where it
 * does not, where spinning on the latch would never end.
 */

#include "../../rach2/sys/arch/x86/udelay.c"

int printf(const char *format, ...);

static int fails;

#define CHECK(what, cond)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

uint32_t pit_reload = 11931u;

/* The model's counter: where it is, how far it has descended in all, and
 * whether a latch is held with the bytes it holds. */
static uint32_t live;
static uint32_t descended;
static int latch_pending;
static uint16_t latched;
static int latch_byte;
static int irq_off;
static int reads_with_irq_on;
static int handler_armed;
static int tears;
/* Another processor that latches channel 0 when it can, and the spins at a
 * held latch. */
static int rival_armed;
static unsigned long spins;
/* The port accesses made. */
static unsigned long accesses;
/* The access after which another processor takes the latch and stops
 * inside it, 0 for never. */
static unsigned long holder_stops_at;
/* Whether the HPET or the PM timer runs, and the microseconds asked of it. */
static int ref_runs;
static uint32_t ref_asked;

/** Move the counter down `counts` counts, reloading it at zero; every port
 * access costs three. */
static void counter_runs(uint32_t counts)
{
    while (counts-- > 0) {
        if (live == 0) {
            live = pit_reload;
        }
        live--;
        descended++;
    }
}

/** Turn the model's interrupts off; 0x200, IF, when they were on. Each
 * latch starts here, so this is where another processor takes the latch
 * and stops inside it, once `holder_stops_at` accesses are made. */
uint32_t kirq_save(void)
{
    uint32_t was = irq_off ? 0 : 0x200u;

    if (holder_stops_at != 0 && accesses >= holder_stops_at) {
        latch_held = 1;
    }

    irq_off = 1;
    return was;
}

/** Turn the model's interrupts on when `flags` has IF. */
void kirq_restore(uint32_t flags)
{
    if (flags & 0x200u) {
        irq_off = 0;
    }
}

/** A port write, which runs the counter; 0x00 to port 0x43 latches
 * channel 0. */
void outb(uint16_t port, uint8_t val)
{
    accesses++;
    counter_runs(3);
    if (port == 0x43 && val == 0x00) {
        latch_pending = 1;
        latched = (uint16_t)live;
        latch_byte = 0;
    }
}

/** A read of port 0x40, channel 0: the latched count a byte at a time, or
 * the live count when no latch is held; 0 for any other port. */
uint8_t inb(uint16_t port)
{
    uint8_t value;

    if (port != 0x40) {
        return 0;
    }
    accesses++;
    if (rival_armed && latch_pending && latch_held == 0) {
        /* The other processor's latch and reads, consuming the wait's. */
        latch_pending = 0;
        tears++;
    }
    if (!irq_off) {
        reads_with_irq_on++;
        if (handler_armed && latch_pending) {
            /* The handler's own latch and reads, consuming the wait's. */
            latch_pending = 0;
            tears++;
        }
    }
    if (latch_pending) {
        value = latch_byte == 0 ? (uint8_t)(latched & 0xFFu) : (uint8_t)(latched >> 8);
        if (++latch_byte == 2) {
            latch_pending = 0;
        }
        counter_runs(3);
        return value;
    }
    /* Live: the high byte is read after the counter passed a borrow. */
    if (latch_byte == 0) {
        value = (uint8_t)(live & 0xFFu);
        latch_byte = 1;
        counter_runs((live & 0xFFu) + 8u);
        return value;
    }
    latch_byte = 0;
    value = (uint8_t)(live >> 8);
    counter_runs(3);
    return value;
}

/** A spin at a held latch, counted. */
void arch_relax(void)
{
    spins++;
}

/** The counts a wait of `usec` must let pass, as the wait computes them: a
 * window at a time, since a product past PIT_WINDOW_USEC overflows. */
static uint32_t counts_for(uint32_t usec)
{
    uint32_t counts = 0, step;

    while (usec > 0) {
        step = usec > PIT_WINDOW_USEC ? PIT_WINDOW_USEC : usec;
        counts += (step * PIT_COUNTS_PER_SEC + 999999u) / 1000000u;
        usec -= step;
    }
    return counts;
}

/** The HPET or the PM timer's wait, as refclock.c's: while it runs, `usec`
 * passes, which the 8254 counts too, and it returns `usec`; else it returns
 * 0 at once. */
uint32_t refclock_wait_us(uint32_t usec)
{
    ref_asked += usec;
    if (!ref_runs) {
        return 0;
    }
    counter_runs(counts_for(usec));
    return usec;
}

/** Reset the model with the counter at `start`, interrupts on, no handler
 * or other processor armed, the latch free and no other counter. */
static void model_reset(uint32_t start)
{
    live = start;
    descended = 0;
    latch_pending = 0;
    latch_byte = 0;
    irq_off = 0;
    reads_with_irq_on = 0;
    handler_armed = 0;
    tears = 0;
    rival_armed = 0;
    spins = 0;
    accesses = 0;
    holder_stops_at = 0;
    latch_held = 0;
    ref_runs = 0;
    ref_asked = 0;
}

/**
 * Run every row; the exit status is the count of failed checks, at most 100.
 *
 * With no handler armed a wait lets its counts pass and few more, across the
 * reload too, and takes every sample of the counter with interrupts off.
 * With the handler armed no sample is torn and the wait takes its full time,
 * and so does a wait longer than one pass of the counter. With another
 * processor latching the counter beside the wait, with interrupts off, no
 * sample is torn either. A latch held by a processor stopped inside it,
 * after PIT_LATCH_TRIES tries and with no port touched after, leaves the
 * rest of the wait to the other counter: with it running the wait lasts and
 * returns its whole time, whether the holder stopped before the wait or in
 * its middle, and with none the wait ends with what it measured.
 */
int main(void)
{
    uint32_t got;

    /* A quiet counter. */
    model_reset(9000u);
    k_udelay(500u);
    CHECK("a wait lets its counts pass", descended >= counts_for(500u));
    CHECK("and not many more", descended < counts_for(500u) + 64u);
    CHECK("every sample is taken with interrupts off", reads_with_irq_on == 0);

    /* Across the reload. */
    model_reset(40u);
    k_udelay(200u);
    CHECK("a wait across the reload lets its counts pass", descended >= counts_for(200u));

    /* The handler armed. */
    model_reset(9000u);
    handler_armed = 1;
    k_udelay(2000u);
    CHECK("an interrupt that latches the counter cannot tear a sample", tears == 0);
    CHECK("and the wait takes its full time", descended >= counts_for(2000u));

    /* A wait longer than one pass of the counter. */
    model_reset(9000u);
    handler_armed = 1;
    k_udelay(5000u);
    CHECK("a long wait takes its full time as well", descended >= counts_for(5000u));

    /* Another processor latching beside the wait. */
    model_reset(9000u);
    irq_off = 1;
    rival_armed = 1;
    k_udelay(2000u);
    CHECK("another processor's latch cannot tear a sample", tears == 0);
    CHECK("and the wait beside it takes its full time", descended >= counts_for(2000u));

    /* A latch whose holder was stopped inside it, and no other counter. */
    model_reset(9000u);
    latch_held = 1;
    CHECK("a latch held by a stopped processor, with no other counter, ends the wait with 0 "
          "measured",
          k_udelay_measured(1000u) == 0u);
    CHECK("after PIT_LATCH_TRIES tries, with no port touched",
          spins == PIT_LATCH_TRIES - 1u && accesses == 0);

    /* The same with the HPET or the PM timer running. */
    model_reset(9000u);
    latch_held = 1;
    ref_runs = 1;
    CHECK("a latch held by a stopped processor leaves the wait to the other counter, which "
          "returns its whole time",
          k_udelay_measured(2500u) == 2500u && ref_asked == 2500u);
    CHECK("and the wait lasts its whole time, after PIT_LATCH_TRIES tries and no port touched",
          descended >= counts_for(2500u) && spins == PIT_LATCH_TRIES - 1u && accesses == 0);

    /* A holder that stops in the middle of the wait, the other counter
     * running and not. */
    model_reset(9000u);
    holder_stops_at = 1200u;
    ref_runs = 1;
    CHECK("a holder that stops in the middle of a wait leaves the rest to the other counter",
          k_udelay_measured(3000u) == 3000u && ref_asked > 0u && ref_asked < 3000u &&
              ref_asked % PIT_WINDOW_USEC == 0u);
    CHECK("and the wait lasts its whole time", descended >= counts_for(3000u));
    model_reset(9000u);
    holder_stops_at = 1200u;
    got = k_udelay_measured(3000u);
    CHECK("with no other counter it returns the windows it measured, at most the time that "
          "passed",
          got < 3000u && got % PIT_WINDOW_USEC == 0u && descended >= counts_for(got));
    model_reset(9000u);

    if (fails == 0) {
        printf("udelay_hosttest: ok\n");
    }
    return fails > 100 ? 100 : fails;
}
