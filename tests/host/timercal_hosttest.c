/*
 * timercal_hosttest.c: the local APIC timer's calibration against the PM
 * timer and against the 8254, and the TSC's against the same two, with the
 * processor stopped at any point of the measurement.
 *
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * The sources are included whole: `refclock.c`, `pit.c`, `udelay.c` and
 * `apictimer.c`, so that a row can start each calibration afresh. The
 * machine is a clock of nanoseconds, and every device the calibrations read
 * gives its value at that clock: the PM timer (3.579545 MHz, 24 bits), the
 * 8254's channel 0 in mode 3 (its count falling at twice 1.193182 MHz and
 * reloading at `pit_reload`), the APIC timer in one-shot mode and the TSC,
 * each at a rate the row sets. Each access to a device takes ACCESS_NS.
 *
 * A row stops the processor once, before the access it names, for 3 ms,
 * 23 ms or 700 ms, as a hypervisor deschedules one: long enough to put the
 * reference and the timer read on either side of the stop at different
 * times, and, from 5 ms, to hide sweeps of the 8254 from `k_udelay`. The
 * positions are every access within STOP_NEAR of the start and the end of
 * the measurement and of each read of the APIC timer or the TSC, and every
 * STOP_STRIDE-th elsewhere. Each result must be within TOLERANCE of the
 * rate the row set. A run with two stops is not a row: the calibration
 * against the 8254 is right when one of its three waits had none, and the
 * TSC's against the PM timer when one of its three windows had none. Other
 * rows check that a scale past 32 bits gives all ones, that a reference
 * that stops gives no calibration, and when the TSC is measured at all.
 */

#include "refclock.c"
#include "pit.c"
#include "udelay.c"
#include "apictimer.c"

extern int printf(const char *fmt, ...);
extern int strcmp(const char *a, const char *b);

static int fails;

#define CHECK(cond, what)                   \
    do {                                    \
        if (!(cond)) {                      \
            printf("  FAIL: %s\n", (what)); \
            fails++;                        \
        }                                   \
    } while (0)

/* The time one access to a device takes, in nanoseconds. */
#define ACCESS_NS 5000u

/* The positions of a stop: every access this near a read of the timer or of
 * the TSC, or the start or the end, and every STOP_STRIDE-th elsewhere. */
#define STOP_NEAR   8ul
#define STOP_STRIDE 37ul

/* A result is right within one part in this many of the rate the row set. */
#define TOLERANCE 400u

/* The reads of the APIC timer or the TSC a run marks, at most. */
#define MARKS_MAX 64

/* The machine's clock; the accesses to devices since the row started; the
 * access before which the processor stops (0 for none) and for how long;
 * the accesses made with interrupts on; and the accesses that read the APIC
 * timer or the TSC, which a run without a stop marks. */
static uint64_t now_ns;
static unsigned long accesses, stop_at;
static uint64_t stop_ns;
static unsigned long irq_on_accesses;
static int irq_off;
static unsigned long marks[MARKS_MAX];
static int nmarks, marking;

/* The devices: the PM timer's count at the clock's zero and whether it
 * counts; the 8254's latch and the byte of it the next read gives; the APIC
 * timer's start, initial count, rate and whether it runs; and the TSC's
 * count at the clock's zero and its rate. */
static uint32_t pm_base;
static int pm_frozen;
static uint32_t pm_frozen_value;
static uint16_t pit_latch;
static int pit_byte;
static uint64_t apic_start_ns;
static uint32_t apic_initial;
static uint64_t apic_khz;
static int apic_running;
static uint64_t tsc_base;
static uint64_t tsc_khz;
static int tsc_invariant;

/** One access to a device: the stop when this is the access it comes
 * before, then the access's own time. */
static void elapse(void)
{
    if (++accesses == stop_at) {
        now_ns += stop_ns;
    }
    now_ns += ACCESS_NS;
    if (!irq_off) {
        irq_on_accesses++;
    }
}

/** Note this access as a read of the APIC timer or the TSC. */
static void mark(void)
{
    if (marking && nmarks < MARKS_MAX) {
        marks[nmarks++] = accesses;
    }
}

/** Turn the model's interrupts off; 0x200, IF, when they were on. */
uint32_t kirq_save(void)
{
    uint32_t was = irq_off ? 0 : 0x200u;

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

/** The PM timer's register: its count at the clock, in 24 bits. */
int acpi_gas_read(const struct acpi_gas *g, uint32_t *value)
{
    (void)g;
    elapse();
    if (!pm_frozen) {
        pm_frozen_value = (uint32_t)(pm_base + now_ns * 3579545u / 1000000000u) & 0xFFFFFFu;
    }
    *value = pm_frozen_value;
    return 0;
}

/** Channel 0's count at the clock: it falls by `pit_reload` in each sweep
 * and reloads at zero. */
static uint16_t pit_live(void)
{
    uint64_t fallen = now_ns * (2u * 1193182u) / 1000000000u;

    return (uint16_t)(pit_reload - (uint32_t)(fallen % pit_reload));
}

/** A port write: 0x00 to port 0x43 latches channel 0, and nothing else is
 * written. */
void outb(uint16_t port, uint8_t val)
{
    elapse();
    CHECK(port == 0x43 && val == 0x00, "the 8254 is only latched");
    pit_latch = pit_live();
    pit_byte = 0;
}

/** A read of port 0x40: the latched count, low byte first. */
uint8_t inb(uint16_t port)
{
    elapse();
    CHECK(port == 0x40, "only channel 0 is read");
    return pit_byte++ == 0 ? (uint8_t)(pit_latch & 0xFFu) : (uint8_t)(pit_latch >> 8);
}

/** Start the APIC timer: one-shot and masked is all a calibration asks. */
void lapic_timer_set(uint8_t vector, int periodic, int masked, uint32_t initial)
{
    CHECK(vector == APIC_TIMER_VECTOR && !periodic && masked, "the timer is one-shot and masked");
    apic_start_ns = now_ns;
    apic_initial = initial;
    apic_running = 1;
}

/** Stop the APIC timer. */
void lapic_timer_stop(void)
{
    apic_running = 0;
}

/** The APIC timer's count at the clock, which stays at 0 once reached. */
uint32_t lapic_timer_current(void)
{
    uint64_t gone;

    elapse();
    mark();
    if (!apic_running) {
        return 0;
    }
    gone = (now_ns - apic_start_ns) * apic_khz / 1000000u;
    return gone >= apic_initial ? 0 : apic_initial - (uint32_t)gone;
}

/** The TSC's count at the clock, in halves. */
void host_tsc_read(uint32_t *lo, uint32_t *hi)
{
    uint64_t v;

    elapse();
    mark();
    v = tsc_base + now_ns * tsc_khz / 1000000u;
    *lo = (uint32_t)v;
    *hi = (uint32_t)(v >> 32);
}

/** The pause of a spin, which the model's clock does not need. */
void arch_relax(void)
{}

/* The one flag the row puts on the command line, or NULL for none. */
static const char *cmdline_flag;

/** Whether the command line has `flag`: only the row's `cmdline_flag`. */
int kcmdline_has(const char *flag)
{
    return cmdline_flag != NULL && strcmp(flag, cmdline_flag) == 0;
}

/** `cpuid`'s highest extended leaf, 0x80000008, and leaf 0x80000007's
 * invariant TSC bit as the row sets it. */
void kcpu_id(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    *a = *b = *c = *d = 0;
    if (leaf == 0x80000000u) {
        *a = 0x80000008u;
    } else if (leaf == 0x80000007u && tsc_invariant) {
        *d = 1u << 8;
    }
}

/* What the calibrations do not reach: the log, the HPET's mapping, the
 * APIC's other registers, the pending lines and the tick's sequence. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

uintptr_t map_mmio(uintptr_t phys, size_t size)
{
    (void)phys;
    (void)size;
    CHECK(0, "no HPET is mapped");
    return 0;
}

uint32_t lapic_this_id(void)
{
    return 0;
}

void lapic_set_lint(int lint, uint32_t entry)
{
    (void)lint;
    (void)entry;
}

uint32_t lapic_timer_initial(void)
{
    return apic_initial;
}

int lapic_requested(uint8_t vector)
{
    (void)vector;
    return 0;
}

int ioapic_line_pending(int line)
{
    (void)line;
    return 0;
}

int pic_irq_pending(uint8_t irq)
{
    (void)irq;
    return 0;
}

uint32_t tick_read_begin(void)
{
    return 0;
}

int tick_read_retry(uint32_t seq)
{
    (void)seq;
    return 0;
}

/** The clock does not run from the TSC here, so the clock source named is
 * the reference's. */
int vclock_running(void)
{
    return 0;
}

/* The calibrations, and the reference each is made against. */
#define APIC_BY_PM  0
#define APIC_BY_PIT 1
#define TSC_BY_PIT  2
#define TSC_BY_PM   3

static const char *const path_names[] = {
    "the APIC timer against the PM timer", "the APIC timer against the 8254",
    "the TSC against the 8254", "the TSC against the PM timer"};

/** Take the PM timer as the reference, or none. */
static void reference(int pm)
{
    struct acpi_info a;
    unsigned int i;

    for (i = 0; i < sizeof(a); i++) {
        ((unsigned char *)&a)[i] = 0;
    }
    a.fadt = pm;
    a.pm_timer.space = ACPI_SPACE_IO;
    a.pm_timer.address = pm ? 0x608u : 0u;
    a.pm_timer_bits = 24;
    pm_frozen = 0;
    CHECK(refclock_init(&a) == (pm ? 0 : -1), "the PM timer is the reference when it is there");
}

/** Run the calibration `path` from the clock at 1 ms, with the stop the row
 * set; return its counts in a tick. */
static uint32_t calibrate(int path)
{
    const char *against = "";
    uint32_t counts;

    reference(path == APIC_BY_PM || path == TSC_BY_PM);
    now_ns = 1000000u;
    accesses = 0;
    irq_on_accesses = 0;
    if (path == TSC_BY_PIT || path == TSC_BY_PM) {
        tsc_per_tick = 0;
        tsc_measured = 0;
        ref_kind = REF_NONE;
        tsc_invariant = 1;
        return timer_calibrate();
    }
    counts = apic_timer_calibrate(TICK_HZ, &against);
    CHECK(strcmp(against, path == APIC_BY_PM ? "pm timer" : "pit") == 0,
          "the calibration names its reference");
    return counts;
}

/** Whether `counts` is within TOLERANCE of `truth`. */
static int close_to(uint32_t counts, uint32_t truth)
{
    uint32_t off = counts > truth ? counts - truth : truth - counts;

    return off <= truth / TOLERANCE;
}

/** Whether a stop before access `p` of a run of `n` is a row. */
static int stop_position(unsigned long p, unsigned long n)
{
    int i;

    if (p <= STOP_NEAR || p + STOP_NEAR >= n || p % STOP_STRIDE == 0) {
        return 1;
    }
    for (i = 0; i < nmarks; i++) {
        if (p + STOP_NEAR >= marks[i] && p <= marks[i] + STOP_NEAR) {
            return 1;
        }
    }
    return 0;
}

/**
 * Run `path` once without a stop, then with a stop of each length at each
 * position, and check each result against the rate the row set, `truth`
 * counts in a tick. Print the first few that are wrong.
 */
static void stop_rows(int path, uint32_t truth)
{
    static const uint64_t lengths[] = {3000000u, 23000000u, 700000000u};
    unsigned long n, p, rows = 0, wrong = 0;
    unsigned int l;
    uint32_t counts;

    stop_at = 0;
    nmarks = 0;
    marking = 1;
    counts = calibrate(path);
    marking = 0;
    n = accesses;
    if (!close_to(counts, truth)) {
        printf("  %s: %u counts a tick for %u without a stop\n", path_names[path], counts, truth);
        CHECK(0, "a calibration without a stop is right");
    }
    CHECK(irq_on_accesses == 0, "a calibration reads its counters with interrupts off");
    for (l = 0; l < sizeof(lengths) / sizeof(lengths[0]); l++) {
        stop_ns = lengths[l];
        for (p = 1; p <= n; p++) {
            if (!stop_position(p, n)) {
                continue;
            }
            stop_at = p;
            counts = calibrate(path);
            rows++;
            if (!close_to(counts, truth) && wrong++ < 4) {
                printf("  %s: %u counts a tick for %u with a stop of %u ms before access %lu of "
                       "%lu\n",
                       path_names[path], counts, truth, (unsigned int)(stop_ns / 1000000u), p, n);
            }
        }
    }
    stop_at = 0;
    if (wrong) {
        printf("  %s: %lu of %lu stops gave a wrong result\n", path_names[path], wrong, rows);
    }
    CHECK(wrong == 0, "a stop anywhere in a calibration leaves its result right");
}

/** Run the rows: 0 when every check passes, 1 otherwise. */
int main(void)
{
    uint32_t counts;

    printf("timercal\n");

    /* A bus clock of 1 GHz, as QEMU's, and one of 133.337 MHz divided by
     * 16; a PM timer that wraps during the measurement. */
    pm_base = 0xFFF000u;
    apic_khz = 1000000u;
    stop_rows(APIC_BY_PM, (uint32_t)(apic_khz * 1000u / TICK_HZ));
    stop_rows(APIC_BY_PIT, (uint32_t)(apic_khz * 1000u / TICK_HZ));
    apic_khz = 133337u / 16u;
    stop_rows(APIC_BY_PM, (uint32_t)(apic_khz * 1000u / TICK_HZ));
    stop_rows(APIC_BY_PIT, (uint32_t)(apic_khz * 1000u / TICK_HZ));

    /* A TSC of 2.893417 GHz whose low half wraps during the measurement. */
    tsc_base = 0xFFF00000u;
    tsc_khz = 2893417u;
    stop_rows(TSC_BY_PIT, (uint32_t)((uint64_t)tsc_khz * 1000u / TICK_HZ));
    stop_rows(TSC_BY_PM, (uint32_t)((uint64_t)tsc_khz * 1000u / TICK_HZ));

    /* A rate past 32 bits, which a stop longer than the reference's wrap can
     * give, is no measurement, and no division fault on i686. */
    CHECK(refclock_scale(3000000000u, 3u, 2u) == 0xFFFFFFFFu &&
              refclock_scale(3000000000u, 2u, 3u) == 2000000000u &&
              refclock_scale(1u, 1u, 0u) == 0xFFFFFFFFu,
          "a quotient past 32 bits, or a division by 0, is all ones");

    /* A PM timer that stops after the first sample is no reference. */
    apic_khz = 1000000u;
    reference(1);
    pm_frozen = 1;
    now_ns = 1000000u;
    {
        const char *against;

        counts = apic_timer_calibrate(TICK_HZ, &against);
    }
    CHECK(counts == 0, "a reference that stops gives no calibration");

    /* The TSC is measured only where it runs at one rate and the command
     * line does not forbid it, and once. */
    reference(0);
    tsc_per_tick = 0;
    tsc_measured = 0;
    tsc_invariant = 0;
    CHECK(timer_calibrate() == 0, "a TSC whose rate varies is not measured");
    tsc_measured = 0;
    cmdline_flag = "rach2.tscreliable";
    CHECK(timer_calibrate() != 0, "unless rach2.tscreliable vouches for it");
    cmdline_flag = NULL;
    tsc_per_tick = 0;
    reference(1);
    tsc_measured = 0;
    tsc_invariant = 1;
    cmdline_flag = "rach2.notsc";
    nmarks = 0;
    marking = 1;
    CHECK(timer_calibrate() == 0 && nmarks == 0, "nor one under rach2.notsc");
    marking = 0;
    cmdline_flag = NULL;
    tsc_measured = 0;
    counts = timer_calibrate();
    nmarks = 0;
    marking = 1;
    CHECK(counts != 0 && timer_calibrate() == counts && nmarks == 0,
          "a second calibration gives the first's counts and reads nothing");
    marking = 0;

    if (fails) {
        printf("timercal_hosttest: %d check(s) failed\n", fails);
        return 1;
    }
    printf("timercal_hosttest: all checks passed\n");
    return 0;
}
