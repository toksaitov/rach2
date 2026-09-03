/**
 * The microsecond wait, driven on the host against a model of the 8254
 * (sys/udelay.c).
 *
 * The file is included whole so the sample the wait takes of the counter is
 * reachable. The model is channel 0 in mode 3: a count that descends by a
 * few units at every port access, since the counter runs while the processor
 * talks to it, and reloads at zero; a latch that holds the count for the two
 * reads that follow; and reads with no latch pending that return the live
 * count a byte at a time, which is what the chip does.
 *
 * The case the notebook produced: an interrupt handler that latches the
 * counter itself, the tick's clock or the USB handler's stamps, running
 * between the wait's latch and its reads. The model stages it whenever a
 * latch is pending at a read taken with interrupts enabled: the handler
 * consumes the latch, and the wait's reads come back live and torn across a
 * borrow, 256 below the truth. The wait then believes the counter dropped
 * further than it did and ends early. With the sample taken under interrupts
 * off the handler cannot get between the latch and the reads, and the wait
 * takes its full time whatever is armed.
 */

#include "../../rach2/sys/udelay.c"

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

/* Every port access costs the counter three of its counts. */
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

uint32_t kirq_save(void)
{
    uint32_t was = irq_off ? 0 : 0x200u;

    irq_off = 1;
    return was;
}

void kirq_restore(uint32_t flags)
{
    if (flags & 0x200u) {
        irq_off = 0;
    }
}

void outb(uint16_t port, uint8_t val)
{
    counter_runs(3);
    if (port == 0x43 && val == 0x00) {
        latch_pending = 1;
        latched = (uint16_t)live;
        latch_byte = 0;
    }
}

uint8_t inb(uint16_t port)
{
    uint8_t value;

    if (port != 0x40) {
        return 0;
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
    /* Live: the low byte now, and the high byte after the counter has moved
       past a borrow, which is the torn reading. */
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
}

/* The counts a wait of `usec` must let pass, as the wait computes them. */
static uint32_t counts_for(uint32_t usec)
{
    return (usec * PIT_COUNTS_PER_SEC + 999999u) / 1000000u;
}

int main(void)
{
    /* A quiet counter: the wait lets at least its counts pass, and every
       sample of the counter was taken with interrupts off. */
    model_reset(9000u);
    k_udelay(500u);
    CHECK("a wait lets its counts pass", descended >= counts_for(500u));
    CHECK("and not many more", descended < counts_for(500u) + 64u);
    CHECK("every sample is taken with interrupts off", reads_with_irq_on == 0);

    /* Across the reload. */
    model_reset(40u);
    k_udelay(200u);
    CHECK("a wait across the reload lets its counts pass", descended >= counts_for(200u));

    /* The handler armed: with interrupts held off around each sample it
       never gets between a latch and its reads, no sample is torn, and the
       wait takes its full time. */
    model_reset(9000u);
    handler_armed = 1;
    k_udelay(2000u);
    CHECK("an interrupt that latches the counter cannot tear a sample", tears == 0);
    CHECK("and the wait takes its full time", descended >= counts_for(2000u));

    /* Windows longer than one pass are made of passes, each measured. */
    model_reset(9000u);
    handler_armed = 1;
    k_udelay(5000u);
    CHECK("a long wait takes its full time as well", descended >= counts_for(5000u));

    if (fails == 0) {
        printf("udelay_hosttest: ok\n");
    }
    return fails > 100 ? 100 : fails;
}
