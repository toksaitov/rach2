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
 * TSC's against the PM timer when one of its three windows had none. With
 * each window's end read of the PM timer held up 800 us, the TSC's
 * calibration gives no rate, never one 2% low, and with the first three
 * held, the rate of those after. Other rows check that a scale past 32
 * bits gives all ones, that a reference that stops gives no calibration,
 * when the TSC is measured at all, that `k_udelay` on a TSC that stops
 * ends (`tsc_wait_rows`), and that the clock follows KVM's clock
 * pages (`kvm_rows`), with their stable bit and without it, whose time a
 * processor with a TSC ahead, a stop of the processor or a tick a minute
 * late does not move, with a platform counter and without one, and that
 * the clock sources are named and ordered as Linux rates kvm-clock and the
 * TSC.
 *
 * Timer rows use a 1 GHz APIC model, one count per nanosecond, on one
 * shared vector. They check a common monotonic scheduler phase, pending
 * periodic interrupts, late and early ticks, coincident tick and event
 * expiry, nested request holds, idle transitions, coarse-clock lag and
 * stable whole-tick deadlines. Clock readings bracket the pending bit or
 * phase so a forced host stop cannot shift a later event by that stop.
 */

#include "refclock.c"
#include "pit.c"
#include "udelay.c"
#include "apictimer.c"

extern int printf(const char *fmt, ...);
extern int strcmp(const char *a, const char *b);

static uint64_t deadline_at;
static uint64_t coarse_model = (uint64_t)-1;

static int fails;
static int guest_model;

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
static uint64_t phase_lag_ns, phase_stop_ns;
static unsigned long accesses, stop_at;
static uint64_t stop_ns;
static unsigned long irq_on_accesses;
static int irq_off;
static unsigned long marks[MARKS_MAX];
static int nmarks, marking;

/* The devices: the PM timer's count at the clock's zero and whether it
 * counts; the 8254's latch, its latched status byte (-1 for none) and the
 * byte of the count the next read gives; the APIC
 * timer's start, initial count, rate and whether it runs; and the TSC's
 * count at the clock's zero and its rate. */
static uint32_t pm_base;
static int pm_frozen;
static uint32_t pm_frozen_value;
static uint16_t pit_latch;
static int pit_status = -1;
static int pit_byte;
static uint64_t apic_start_ns;
static uint32_t apic_initial;
/* The writes that started the timer counting, `lapic_timer_set`'s and
 * `lapic_timer_count`'s: each a programming of the timer. */
static uint32_t apic_writes;
static uint64_t apic_khz;
static int apic_running;
/* With `hr_model`, the rows of the timer between ticks: the timer's entry's
 * vector and mode as last written, and the vector the row has raised and
 * not yet taken, 0 for none. */
static int hr_model;
static uint8_t apic_vector;
static int apic_periodic;
static uint8_t raised;
/* With `hr_model`, the processor stops for `hr_stop_ns` after the
 * `hr_stop_read`-th reading of the clock (0 for none), counted in
 * `hr_reads`. */
static unsigned long hr_reads, hr_stop_read;
static uint64_t hr_stop_ns;
static uint64_t tsc_base;
static uint64_t tsc_khz;
static int tsc_one_rate_bit;

/* A TSC that stops: whether it reads `tsc_still_value`, and the reads made
 * since it stopped. After TSC_STILL_GIVEUP of them it runs again, so that a
 * wait with no escape ends, too late, and its row fails instead of hanging. */
#define TSC_STILL_GIVEUP (4ul * TSC_STALL_READS)
static int tsc_still;
static uint64_t tsc_still_value;
static unsigned long tsc_still_reads;

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

/* The held reads of the PM timer: while `pm_holds` is not 0, a read that
 * ends a window of the TSC's calibration, 20 ms of the PM timer past the
 * read that began it, `pm_window_start`, is held up `pm_hold_ns` before the
 * PM timer is read, and `pm_holds` counts down unless it is negative. */
static uint64_t pm_hold_ns;
static int pm_holds;
static int pm_in_window;
static uint32_t pm_window_start;

/** The PM timer's count at the clock, in 24 bits. */
static uint32_t pm_count(void)
{
    return (uint32_t)(pm_base + now_ns * 3579545u / 1000000000u) & 0xFFFFFFu;
}

/* The reads of the PM timer's register so far. */
static unsigned long pm_reads;

/** The PM timer's register: its count at the clock, in 24 bits, the read
 * held up where it ends a window and the row holds one. */
int acpi_gas_read(const struct acpi_gas *g, uint32_t *value)
{
    (void)g;
    pm_reads++;
    elapse();
    if (!pm_frozen) {
        pm_frozen_value = pm_count();
        if (pm_holds != 0 && !pm_in_window) {
            pm_in_window = 1;
            pm_window_start = pm_frozen_value;
        } else if (pm_holds != 0 &&
                   ((pm_frozen_value - pm_window_start) & 0xFFFFFFu) >= 3579545u / 50u) {
            now_ns += pm_hold_ns;
            pm_frozen_value = pm_count();
            pm_in_window = 0;
            if (pm_holds > 0) {
                pm_holds--;
            }
        }
    }
    *value = pm_frozen_value;
    return 0;
}

/** The count channel 0 has fallen at the clock, twice the input clock's
 * counts. */
static uint64_t pit_fallen(void)
{
    return now_ns * (2u * 1193182u) / 1000000000u;
}

/** Channel 0's count at the clock: it falls by `pit_reload` in each sweep
 * and reloads at zero. */
static uint16_t pit_live(void)
{
    return (uint16_t)(pit_reload - (uint32_t)(pit_fallen() % pit_reload));
}

/** A port write to port 0x43: 0x00 latches channel 0's count, and 0xC2,
 * the read-back command, its status and count together, the status read
 * first; nothing else is written. The status of mode 3, read and written
 * low byte first, has OUT (bit 7) high through the first sweep of each
 * period. */
void outb(uint16_t port, uint8_t val)
{
    elapse();
    CHECK(port == 0x43 && (val == 0x00 || val == 0xC2), "the 8254 is only latched or read back");
    pit_latch = pit_live();
    pit_status = -1;
    if (val == 0xC2) {
        pit_status = 0x36 | ((pit_fallen() / pit_reload) % 2u == 0 ? 0x80 : 0x00);
    }
    pit_byte = 0;
}

/** A read of port 0x40: the latched status, when read back, and then the
 * latched count, low byte first. */
uint8_t inb(uint16_t port)
{
    uint8_t status;

    elapse();
    CHECK(port == 0x40, "only channel 0 is read");
    if (pit_status >= 0) {
        status = (uint8_t)pit_status;
        pit_status = -1;
        return status;
    }
    return pit_byte++ == 0 ? (uint8_t)(pit_latch & 0xFFu) : (uint8_t)(pit_latch >> 8);
}

/** Start the APIC timer: one-shot and masked is all a calibration asks; the
 * rows of the timer between ticks keep its vector and mode. */
void lapic_timer_set(uint8_t vector, int periodic, int masked, uint32_t initial)
{
    if (hr_model) {
        CHECK(!masked, "the timer between ticks is not masked");
        apic_vector = vector;
        apic_periodic = periodic;
    } else {
        CHECK(vector == APIC_TIMER_VECTOR && !periodic && masked,
              "the timer is one-shot and masked");
    }
    apic_start_ns = now_ns;
    apic_initial = initial;
    apic_running = 1;
    apic_writes++;
}

/** Start the APIC timer again from `initial` counts, its entry kept; only
 * the timer between ticks does. */
void lapic_timer_count(uint32_t initial)
{
    CHECK(hr_model, "the timer is not started again from a count");
    apic_start_ns = now_ns;
    apic_initial = initial;
    apic_writes++;
}

/** Make the APIC timer one-shot at `vector` from `initial` counts, its
 * divide register kept; only the timer between ticks does. */
void lapic_timer_oneshot(uint8_t vector, uint32_t initial)
{
    CHECK(hr_model, "the timer is not made one-shot without its divide");
    apic_vector = vector;
    apic_periodic = 0;
    apic_start_ns = now_ns;
    apic_initial = initial;
    apic_running = 1;
    apic_writes++;
}

/* The processors' records, which the timer reads for the idle stop of the
 * tick; no row's processor waits idle, so no tick stops. */
struct percpu cpu_table[NR_CPUS];

/** The host divides 64 bits itself. */
uint64_t tick_div64(uint64_t n, uint64_t d, uint64_t *rem)
{
    if (rem) {
        *rem = n % d;
    }
    return n / d;
}

/** Stop the APIC timer. */
void lapic_timer_stop(void)
{
    apic_running = 0;
}

/** The APIC timer's count at the clock, which stays at 0 once reached, or
 * in periodic mode reloads; the rows of the timer between ticks take no
 * time to read it. */
uint32_t lapic_timer_current(void)
{
    uint64_t gone;

    if (hr_model) {
        gone = (now_ns - apic_start_ns) * apic_khz / 1000000u;
        if (apic_periodic) {
            return apic_initial - (uint32_t)(gone % apic_initial);
        }
        return gone >= apic_initial ? 0 : apic_initial - (uint32_t)gone;
    }
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
    if (tsc_still) {
        if (++tsc_still_reads < TSC_STILL_GIVEUP) {
            v = tsc_still_value;
        } else {
            tsc_still = 0;
        }
    }
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
    if (leaf == 1 && guest_model) {
        *c = 1u << 31;
    } else if (leaf == 0x80000000u) {
        *a = 0x80000008u;
    } else if (leaf == 0x80000007u && tsc_one_rate_bit) {
        *d = 1u << 8;
    } else if (leaf == 0) {
        *a = 7;
    }
}

/* Whether `cpuid` reports IA32_TSC_ADJUST, as the rows of the clock
 * sources' names set it. */
static int tsc_adjust_bit;

/** Leaf 7's sub-leaf 0: EBX bit 1 as the row sets it. */
void kcpu_id_sub(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    *a = *b = *c = *d = 0;
    if (leaf == 7 && sub == 0 && tsc_adjust_bit) {
        *b = 1u << 1;
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

/** 1: the boot processor alone runs, so every TSC passed the check, which
 * none of these rows asks (`timer_cycles_agree`). */
int tsc_synced(int *cpu, uint64_t *behind)
{
    *cpu = 0;
    *behind = 0;
    return 1;
}

/** The vector for the interrupts between ticks: 64 in their rows, and -1,
 * none, for the calibrations. */
int isr_alloc_vector(void)
{
    return hr_model ? 64 : -1;
}

/** The handler of the interrupts between ticks, which only their rows set. */
void set_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)handler;
    CHECK(hr_model && vector == 64 && strcmp(name, "hrtimer") == 0,
          "only the rows between ticks set a handler, hrtimer's");
}

/** The flags of the handler between ticks: without the big lock. */
void isr_register_flags(uint8_t vector, isr_handler_t handler, unsigned int flags)
{
    (void)handler;
    CHECK(hr_model && vector == 64 && flags == ISR_UNLOCKED,
          "the handler between ticks runs without the big lock");
}

/** The model's clock in the rows of the timer between ticks, with the
 * row's stop after one reading; 0, unread, for the calibrations. */
int64_t rtc_monotonic_ns(void)
{
    int64_t t = (int64_t)now_ns;

    if (!hr_model) {
        return 0;
    }
    if (++hr_reads == hr_stop_read) {
        now_ns += hr_stop_ns;
    }
    return t;
}

static int64_t pass_next;

/** Model the timer queue's next event at the end of the pass. */
void timer_hr_pass(void)
{
    if (pass_next != 0) {
        timer_wake_at(pass_next);
        pass_next = 0;
    }
}

/** Take the device entry and its unlocked handler as the IRQ router does. */
static int take_hr_event(uint32_t vector)
{
    int result = apic_hr_entry(vector);

    if (vector == APIC_TIMER_VECTOR && (result & APIC_HR_EVENT) != 0) {
        apic_hr_interrupt(NULL);
    }
    return result;
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

/** Whether the row has raised `vector` and not yet taken it. */
int lapic_requested(uint8_t vector)
{
    return raised != 0 && vector == raised;
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

/** Zero: the model is no KVM guest, so the TSC is measured. */
uint32_t kvmclock_counts_per_tick(void)
{
    return 0;
}

/* KVM's clock pages as `kvm_rows` gives them: whether they have changed
 * since the tick last read one, their rate in counts a tick, 0 outside the
 * rows, and their stable bit; how far the TSC of the processor counting the
 * tick is ahead of the boot processor's, in counts; and the reading of
 * KVM's clock after which the processor stops for `kvm_stop_ns`, 0 for
 * none, counted in `kvm_reads`. */
static int kvm_changed;
static uint32_t kvm_counts_model;
static int kvm_stable_model;
static uint64_t kvm_tsc_ahead;
static unsigned long kvm_reads, kvm_stop_read;
static uint64_t kvm_stop_ns;

/** A reading of the page on the processor counting the tick: its own TSC,
 * one access, and KVM's clock at it, the machine's clock, which is one clock
 * on every processor; then the stop when this is the reading it follows. */
static void kvm_read(uint64_t *tsc, uint64_t *ns)
{
    uint32_t lo, hi;

    host_tsc_read(&lo, &hi);
    *tsc = (((uint64_t)hi << 32) | lo) + kvm_tsc_ahead;
    *ns = now_ns;
    if (++kvm_reads == kvm_stop_read) {
        now_ns += kvm_stop_ns;
    }
}

/** The page's reading, with its rate and stable bit as the row has them
 * and whether the row changed them since the last. */
int kvmclock_sample(struct kvmclock_sample *s)
{
    if (kvm_counts_model == 0) {
        return KVMCLOCK_NONE;
    }
    kvm_read(&s->tsc, &s->ns);
    s->counts = kvm_counts_model;
    s->stable = kvm_stable_model;
    if (!kvm_changed) {
        return KVMCLOCK_SAME;
    }
    kvm_changed = 0;
    return KVMCLOCK_CHANGED;
}

/** The TSC and KVM's clock alone, from the same reading. */
int kvmclock_now(uint64_t *tsc, uint64_t *ns)
{
    if (kvm_counts_model == 0) {
        return 0;
    }
    kvm_read(tsc, ns);
    return 1;
}

/** Whether the row keeps KVM's pages: those that state a rate, with the
 * stable bit or without it. */
int kvmclock_kept(void)
{
    return kvm_counts_model != 0;
}

/* The vDSO's clock as the rows of the clock sources' names set it. */
static uint32_t vclock_mode_model = VCLOCK_NONE;

/** What the vDSO's clock counts: nothing but in those rows, so the clock
 * source named is the reference's. */
uint32_t vclock_mode(void)
{
    return vclock_mode_model;
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
        tsc_one_rate_bit = 1;
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

/**
 * The TSC's calibration against the PM timer with each window's end read
 * held up 800 us before the PM timer is read, the first `windows` windows
 * or, for -1, every one: the middle of that end's spread is 400 us before
 * the read, which makes a window's rate 2% low. Return the calibration's
 * counts in a tick.
 */
static uint32_t held_calibrate(int windows)
{
    reference(1);
    now_ns = 1000000u;
    accesses = 0;
    stop_at = 0;
    tsc_per_tick = 0;
    tsc_measured = 0;
    ref_kind = REF_NONE;
    tsc_one_rate_bit = 1;
    pm_hold_ns = 800000u;
    pm_holds = windows;
    pm_in_window = 0;
    return timer_calibrate();
}

/** Windows held up whose spread passes its bound give no rate, never one
 * 2% low; the windows after the held ones give the right rate. */
static void held_rows(uint32_t truth)
{
    uint32_t counts;

    counts = held_calibrate(-1);
    CHECK(counts == 0, "a calibration whose every window's end was held up 800 us gives no rate");
    counts = held_calibrate((int)TSC_WINDOWS);
    CHECK(close_to(counts, truth), "one whose first three windows' ends were held up gives the "
                                   "rate of the windows after them");
    pm_holds = 0;
    pm_hold_ns = 0;
}

/* The rows' times, in milliseconds on the model's clock. */
#define EDGE(x)         ((uint64_t)((x) * (double)TICK_NS + 0.5))
#define APIC_COUNTS(ns) ((uint32_t)(ns) + 1u)

/** The first time asked between ticks from tick 10 ticks, the interrupts
 * before and at it, and times asked while one is armed. */
static void hr_wake_rows(void)
{
    struct hr_state *c = &hr_cpu[0];

    hr_model = 1;
    apic_khz = 1000000u;
    ref_kind = REF_TSC;
    raised = 0;
    now_ns = EDGE(10);
    apic_timer_start(TICK_NS, TICK_HZ);
    apic_hr_init(TICK_NS);
    CHECK(hr_vector == APIC_TIMER_VECTOR && apic_vector == APIC_TIMER_VECTOR && apic_periodic,
          "the timer ticks periodically, with the vector between ticks taken");

    now_ns = EDGE(10.3);
    timer_wake_at((int64_t)EDGE(10.6));
    CHECK(c->oneshot && c->tick == (int64_t)EDGE(11) && c->want == (int64_t)EDGE(10.6),
          "the first time asked makes it one-shot, its tick at 11 ticks as the count says");
    CHECK(apic_vector == APIC_TIMER_VECTOR && !apic_periodic &&
              apic_initial == APIC_COUNTS(EDGE(0.3)),
          "and counts to the time asked, rounded up, on the vector between ticks");

    now_ns = EDGE(10.599);
    CHECK(take_hr_event(APIC_TIMER_VECTOR) && c->want == (int64_t)EDGE(10.6) &&
              apic_initial == APIC_COUNTS(EDGE(0.001)),
          "an interrupt a short interval early counts to the time again");
    now_ns = EDGE(10.6);
    CHECK(take_hr_event(APIC_TIMER_VECTOR) && c->want == 0 && apic_vector == APIC_TIMER_VECTOR &&
              apic_initial == APIC_COUNTS(EDGE(0.4)),
          "the one at the time serves it and counts to the tick");

    now_ns = EDGE(10.7);
    timer_wake_at((int64_t)EDGE(10.9));
    timer_wake_at((int64_t)EDGE(10.8));
    timer_wake_at((int64_t)EDGE(10.85));
    CHECK(c->want == (int64_t)EDGE(10.8) && apic_vector == APIC_TIMER_VECTOR &&
              apic_initial == APIC_COUNTS(EDGE(0.1)),
          "a time asked while one is armed replaces it only when earlier");
    now_ns = EDGE(10.8);
    CHECK(take_hr_event(APIC_TIMER_VECTOR) && apic_vector == APIC_TIMER_VECTOR &&
              apic_initial == APIC_COUNTS(EDGE(0.2)),
          "its interrupt counts to the tick");
}

/** Ticks late, early and behind, and a wake at the tick's time, after
 * `hr_wake_rows`. */
static void hr_tick_rows(void)
{
    struct hr_state *c = &hr_cpu[0];

    now_ns = EDGE(11.2);
    CHECK(take_hr_event(APIC_TIMER_VECTOR) && c->tick == (int64_t)EDGE(12) &&
              apic_initial == APIC_COUNTS(EDGE(0.8)),
          "a tick 0.2 ticks late is the tick, and the next is at 12 ticks, its phase kept");
    now_ns = EDGE(11.5);
    CHECK(!(take_hr_event(APIC_TIMER_VECTOR) & APIC_HR_TICK) && c->tick == (int64_t)EDGE(12) &&
              apic_initial == APIC_COUNTS(EDGE(0.5)),
          "an interrupt at the tick's vector half a tick before its time is not the tick");

    now_ns = EDGE(11.99);
    timer_wake_at((int64_t)EDGE(11.995));
    now_ns = EDGE(12.01);
    CHECK(take_hr_event(APIC_TIMER_VECTOR) == (APIC_HR_TICK | APIC_HR_EVENT) && c->want == 0 &&
              c->tick == (int64_t)EDGE(13) && apic_vector == APIC_TIMER_VECTOR &&
              apic_initial == APIC_COUNTS(EDGE(0.99)),
          "a shared-vector interrupt past both deadlines serves the event and tick once");
    now_ns = EDGE(12.011);
    CHECK(!(take_hr_event(APIC_TIMER_VECTOR) & APIC_HR_TICK) && c->tick == (int64_t)EDGE(13),
          "a later stale interrupt cannot serve that tick again");

    now_ns = EDGE(13.95);
    CHECK(take_hr_event(APIC_TIMER_VECTOR) && c->tick == (int64_t)EDGE(14) &&
              apic_initial == APIC_COUNTS(EDGE(0.05)),
          "a tick 0.95 ticks late moves the next to 14 ticks, a fraction of a tick away, not past "
          "it");
    now_ns = EDGE(14) - 30000u;
    CHECK(take_hr_event(APIC_TIMER_VECTOR) && c->tick == (int64_t)EDGE(15),
          "an interrupt 30 us before the tick's time is that tick, and the next is at 15 ticks");

    now_ns = EDGE(5013);
    CHECK(take_hr_event(APIC_TIMER_VECTOR) && c->tick == (int64_t)EDGE(5014),
          "a processor 5000 ticks behind starts its tick a tick from the clock");
}

/** A tick raised and not yet taken as the timer turns one-shot, from a start
 * again at 20 ticks. */
static void hr_switch_rows(void)
{
    struct hr_state *c = &hr_cpu[0];

    now_ns = EDGE(20);
    apic_timer_start(TICK_NS, TICK_HZ);
    CHECK(!c->oneshot && apic_periodic, "a timer started again is periodic");
    now_ns = EDGE(21.0002);
    raised = APIC_TIMER_VECTOR;
    timer_wake_at((int64_t)EDGE(21.5));
    CHECK(c->tick == (int64_t)EDGE(21) && apic_vector == APIC_TIMER_VECTOR && apic_initial == 1u,
          "a tick raised and not taken as it turns one-shot is the tick before the count's next");
    raised = 0;
    now_ns = EDGE(21.0003);
    CHECK(take_hr_event(APIC_TIMER_VECTOR) && c->tick == (int64_t)EDGE(22) &&
              apic_vector == APIC_TIMER_VECTOR,
          "taken, it is the tick, once, and the timer counts to the time asked");
    now_ns = EDGE(21.5);
    CHECK(take_hr_event(APIC_TIMER_VECTOR) && apic_vector == APIC_TIMER_VECTOR &&
              apic_initial == APIC_COUNTS(EDGE(0.5)),
          "whose interrupt counts to the tick at 22 ticks");
    now_ns = EDGE(22);
    CHECK(take_hr_event(APIC_TIMER_VECTOR) && c->tick == (int64_t)EDGE(23) && !apic_periodic,
          "which is the tick, the timer one-shot still");

    now_ns = EDGE(30);
    apic_timer_start(TICK_NS, TICK_HZ);
    now_ns = EDGE(30.3);
    hr_reads = 0;
    hr_stop_read = 1;
    hr_stop_ns = EDGE(0.2);
    timer_wake_at((int64_t)EDGE(30.9));
    hr_stop_read = 0;
    CHECK(c->tick == (int64_t)EDGE(31) && apic_vector == APIC_TIMER_VECTOR &&
              apic_initial == APIC_COUNTS(EDGE(0.4)),
          "a stop between the clock's reading and the count's read moves no tick's phase");
}

/** Times asked during a pass (`timer_wake_hold`), an inner pass's too,
 * program nothing; the outermost release programs the earliest once, as
 * Linux's hrtimer_interrupt programs the next event once at its end
 * (kernel/time/hrtimer.c:1863-1893); from a start again at 40 ticks. Without
 * the hold, the three times asked here would program the timer twice. */
static void hr_hold_rows(void)
{
    struct hr_state *c = &hr_cpu[0];
    uint32_t writes;

    now_ns = EDGE(40);
    apic_timer_start(TICK_NS, TICK_HZ);
    now_ns = EDGE(40.1);
    writes = apic_writes;
    timer_wake_hold();
    timer_wake_at((int64_t)EDGE(40.7));
    timer_wake_hold();
    timer_wake_at((int64_t)EDGE(40.5));
    timer_wake_at((int64_t)EDGE(40.6));
    timer_wake_release();
    CHECK(apic_writes == writes && c->want == 0 && c->held == (int64_t)EDGE(40.5) && apic_periodic,
          "times asked during a pass, an inner pass's too, program nothing, the earliest kept");
    timer_wake_release();
    CHECK(apic_writes == writes + 1u && c->want == (int64_t)EDGE(40.5) && c->held == 0 &&
              c->hold == 0 && apic_vector == APIC_TIMER_VECTOR &&
              apic_initial == APIC_COUNTS(EDGE(0.4)),
          "the outermost release programs the earliest once, 0.4 ticks away");
    now_ns = EDGE(40.5);
    CHECK(take_hr_event(APIC_TIMER_VECTOR) && c->want == 0 && apic_vector == APIC_TIMER_VECTOR,
          "and its interrupt serves it");
}

/** Start the reference a TSC of 2 GHz, stable by KVM's pages, calls for
 * (`ref_start`), with the tick in progress beginning now and the
 * microsecond clock at 0. */
static void kvm_tsc_start(void)
{
    uint32_t raw;

    tsc_khz = 2000000u;
    tsc_per_tick = 2u * TICK_NS;
    kvm_counts_model = 2000000u;
    kvm_stable_model = 1;
    kvm_changed = 0;
    now_floor = 0;
    ref_kind = REF_NONE;
    CHECK(ref_start() && ref_kind == REF_KVM, "KVM's pages make KVM's clock the reference");
    ref.edge = ref_count(&raw);
    ref.frac = 0;
    ref.ticks = 0;
    ref_us_base = 0;
}

/** Whether the microsecond clock moved from `us0` to `us1` by the time the
 * machine's clock moved from `ns0` to `ns1`, within 10 us. */
static int kvm_kept_time(uint32_t us0, uint64_t ns0, uint32_t us1, uint64_t ns1)
{
    uint32_t gone = us1 - us0, want = (uint32_t)((ns1 - ns0) / 1000u);

    return gone + 10u >= want && gone <= want + 10u;
}

/**
 * KVM's clock pages followed at the tick, with the PM timer as the
 * platform's counter and KVM's clock keeping time: a stable bit withdrawn
 * a third of a tick into a tick keeps KVM's clock the reference, as Linux
 * goes on reading kvm-clock without the bit, counts no tick, reads the PM
 * timer not once, and the microsecond clock goes on without a step; KVM's
 * clock counts the next tick; the bit set again counts none; and a new
 * rate of 2.5 GHz becomes the TSC's counts in a tick, while the next tick
 * is counted by KVM's clock.
 */
static void kvm_follow_rows(void)
{
    uint32_t us0, us1, us2, us3;
    uint64_t at, ns0, ns1, ns2, ns3;
    unsigned long reads;

    now_ns = EDGE(100);
    kvm_tsc_start();
    CHECK(strcmp(timer_reference(), "kvm-clock") == 0, "the reference is named kvm-clock");
    now_ns += EDGE(1.0 / 3.0);
    us0 = timer_now_us();
    ns0 = now_ns;
    kvm_stable_model = 0;
    kvm_changed = 1;
    reads = pm_reads;
    CHECK(timer_tick_clock() == 0 && ref_kind == REF_KVM && pm_reads == reads,
          "a withdrawn stable bit keeps KVM's clock the reference and reads no PM timer");
    us1 = timer_now_us();
    ns1 = now_ns;
    CHECK(kvm_kept_time(us0, ns0, us1, ns1), "and the microsecond clock goes on without a step");
    now_ns += EDGE(1);
    CHECK(timer_tick_clock() == 1 && ref_kind == REF_KVM,
          "KVM's clock counts the next tick without the bit");
    us2 = timer_now_us();
    ns2 = now_ns;
    CHECK(kvm_kept_time(us1, ns1, us2, ns2), "a tick later by KVM's clock");
    kvm_stable_model = 1;
    kvm_changed = 1;
    CHECK(timer_tick_clock() == 0 && ref_kind == REF_KVM, "the bit set again counts no tick");
    us3 = timer_now_us();
    ns3 = now_ns;
    CHECK(kvm_kept_time(us2, ns2, us3, ns3), "again without a step");
    at = tsc_base + now_ns * tsc_khz / 1000000u;
    tsc_khz = 2500000u;
    tsc_base = at - now_ns * tsc_khz / 1000000u;
    kvm_counts_model = 2500000u;
    kvm_changed = 1;
    CHECK(timer_tick_clock() == 0 && tsc_per_tick == 2500000u && ref.tick == NS_PER_TICK,
          "a new rate becomes the TSC's counts in a tick");
    now_ns += EDGE(1);
    CHECK(timer_tick_clock() == 1, "and the next tick is counted by KVM's clock");
}

/**
 * The tick counted without the stable bit on a processor whose TSC is a
 * second ahead of the boot processor's, which read the edge: the time
 * since the edge is KVM's clock's, so the tick counts none and the
 * microsecond clock goes on without a step. A processor stopped for 100
 * ticks after its reading of KVM's clock counts them at the next tick, and
 * the microsecond clock goes on by the 100 ticks.
 */
static void kvm_ahead_rows(void)
{
    uint32_t us0, us1;
    uint64_t ns0, ns1;

    now_ns = EDGE(200);
    tsc_base = 0;
    kvm_tsc_start();
    now_ns += EDGE(1.0 / 3.0);
    us0 = timer_now_us();
    ns0 = now_ns;
    kvm_tsc_ahead = 2000000000u;
    kvm_stable_model = 0;
    kvm_changed = 1;
    CHECK(timer_tick_clock() == 0 && ref_kind == REF_KVM,
          "a TSC a second ahead on the processor counting the tick counts no tick");
    us1 = timer_now_us();
    ns1 = now_ns;
    CHECK(kvm_kept_time(us0, ns0, us1, ns1), "and the microsecond clock goes on without a step");
    kvm_tsc_ahead = 0;

    now_ns = EDGE(300);
    kvm_tsc_start();
    kvm_stable_model = 0;
    kvm_changed = 1;
    now_ns += EDGE(1.0 / 3.0);
    us0 = timer_now_us();
    ns0 = now_ns;
    kvm_reads = 0;
    kvm_stop_read = 1;
    kvm_stop_ns = EDGE(100);
    CHECK(timer_tick_clock() == 0 && ref_kind == REF_KVM,
          "a stop of 100 ticks after the tick's reading counts nothing at that tick");
    kvm_stop_read = 0;
    CHECK(timer_tick_clock() == 100u && ref_kind == REF_KVM,
          "and its 100 ticks at the next, by KVM's clock without the bit");
    us1 = timer_now_us();
    ns1 = now_ns;
    CHECK(kvm_kept_time(us0, ns0, us1, ns1), "and the microsecond clock keeps the 100 ticks");
}

/**
 * A tick after 65 s without one, the stable bit withdrawn meanwhile: the
 * pass counts the bound's 60 s by KVM's clock, which keeps the other 5 s
 * behind its edge, and the next pass counts them; KVM's clock stays the
 * reference and the microsecond clock keeps all 65 s.
 */
static void kvm_catchup_rows(void)
{
    uint32_t us0, us1;
    uint64_t ns0, ns1;

    now_ns = EDGE(500);
    kvm_tsc_start();
    us0 = timer_now_us();
    ns0 = now_ns;
    now_ns += EDGE(65.0 * TICK_HZ);
    kvm_stable_model = 0;
    kvm_changed = 1;
    CHECK(timer_tick_clock() == REF_DUE_MAX && ref_kind == REF_KVM,
          "a tick 65 s late counts the bound");
    CHECK(timer_tick_clock() == 5u * TICK_HZ && ref_kind == REF_KVM,
          "the next pass counts the other 5 s, by KVM's clock still");
    us1 = timer_now_us();
    ns1 = now_ns;
    CHECK(kvm_kept_time(us0, ns0, us1, ns1), "and the microsecond clock keeps the 65 s");
}

/** Whether the clock sources listed are `first`, `second` and `third` in
 * that order and no more, and the one in use is `current`. */
static int kvm_sources_are(const char *current, const char *first, const char *second,
                           const char *third)
{
    return strcmp(timer_clocksource(-1), current) == 0 &&
           strcmp(timer_clocksource(0), first) == 0 && strcmp(timer_clocksource(1), second) == 0 &&
           strcmp(timer_clocksource(2), third) == 0 && timer_clocksource(3) == NULL;
}

/**
 * The clock sources' names under KVM's pages, with the PM timer present, as
 * Linux rates them (kvmclock_init; check_system_tsc_reliable): without a
 * TSC that `cpuid` says runs at one rate, kvm-clock is rated above the TSC
 * and is the one in use; with one, the TSC is rated above kvm-clock, which
 * watches it, and is the one in use while the vDSO's clock counts it,
 * kvm-clock once that clock left it. IA32_TSC_ADJUST changes none of it,
 * since the kernel keeps no check of that register as Linux does, and
 * `rach2.tscreliable`, as Linux's `tsc=reliable`, only leaves a TSC rated
 * above kvm-clock unwatched and rates nothing. KVM's pages vouch for the
 * TSC's rate either way, so no watch measures it against the PM timer.
 */
static void kvm_source_rows(void)
{
    now_ns = EDGE(900);
    kvm_tsc_start();
    tsc_one_rate_bit = 0;
    tsc_adjust_bit = 1;
    tsc_kvm_rank = -1;
    CHECK(timer_tsc_kvm() == TSC_KVM_BELOW &&
              kvm_sources_are("kvm-clock", "kvm-clock", "tsc", "acpi_pm"),
          "without a TSC of one rate kvm-clock rates above the TSC and is the clock source");
    vclock_mode_model = VCLOCK_PVCLOCK;
    CHECK(strcmp(timer_clocksource(-1), "kvm-clock") == 0,
          "as it is while the vDSO reads KVM's page");
    vclock_mode_model = VCLOCK_NONE;

    tsc_one_rate_bit = 1;
    tsc_adjust_bit = 0;
    tsc_kvm_rank = -1;
    CHECK(timer_tsc_kvm() == TSC_KVM_WATCHED &&
              kvm_sources_are("kvm-clock", "tsc", "kvm-clock", "acpi_pm"),
          "a TSC of one rate rates above kvm-clock, which watches it");
    vclock_mode_model = VCLOCK_TSC;
    CHECK(strcmp(timer_clocksource(-1), "tsc") == 0,
          "and is the clock source while the vDSO's clock counts it");
    vclock_mode_model = VCLOCK_NONE;

    tsc_adjust_bit = 1;
    tsc_kvm_rank = -1;
    CHECK(timer_tsc_kvm() == TSC_KVM_WATCHED &&
              kvm_sources_are("kvm-clock", "tsc", "kvm-clock", "acpi_pm"),
          "with IA32_TSC_ADJUST, which nothing here checks, kvm-clock still watches it");
    tsc_adjust_bit = 0;
    tsc_kvm_rank = -1;
    cmdline_flag = "rach2.tscreliable";
    CHECK(timer_tsc_kvm() == TSC_KVM_RELIABLE &&
              kvm_sources_are("kvm-clock", "tsc", "kvm-clock", "acpi_pm"),
          "under rach2.tscreliable it goes unwatched, rated as before");
    tsc_one_rate_bit = 0;
    tsc_adjust_bit = 1;
    tsc_kvm_rank = -1;
    CHECK(timer_tsc_kvm() == TSC_KVM_BELOW &&
              kvm_sources_are("kvm-clock", "kvm-clock", "tsc", "acpi_pm"),
          "and rach2.tscreliable rates no TSC above kvm-clock that has not one rate");
    tsc_adjust_bit = 0;
    cmdline_flag = NULL;
    tsc_kvm_rank = -1;
    CHECK(timer_tsc_trusted(), "KVM's pages vouch for the TSC's rate");
    tsc_watch_measured = 0;
    CHECK(timer_tsc_watch() == 0, "so no watch measures it against the PM timer");
    tsc_watch_measured = 0;
    tsc_one_rate_bit = 1;
}

/**
 * KVM's pages with no platform counter: a withdrawn stable bit keeps KVM's
 * clock the reference, and the tick, counted on a processor whose TSC is a
 * second ahead of the boot processor's, counts KVM's nanoseconds, so that
 * the withdrawal counts no tick and the next tick one.
 */
static void kvm_alone_rows(void)
{
    uint32_t us0, us1;
    uint64_t ns0, ns1;

    reference(0);
    now_ns = EDGE(70000);
    kvm_tsc_start();
    now_ns += EDGE(1.0 / 3.0);
    us0 = timer_now_us();
    ns0 = now_ns;
    kvm_tsc_ahead = 2000000000u;
    kvm_stable_model = 0;
    kvm_changed = 1;
    CHECK(timer_tick_clock() == 0 && ref_kind == REF_KVM,
          "with no platform counter a withdrawn bit keeps KVM's clock, and a TSC ahead counts no "
          "tick");
    now_ns += EDGE(1);
    CHECK(timer_tick_clock() == 1, "the next tick is counted by KVM's clock");
    us1 = timer_now_us();
    ns1 = now_ns;
    CHECK(kvm_kept_time(us0, ns0, us1, ns1), "and the microsecond clock goes on without a step");
    kvm_tsc_ahead = 0;
}

/** `k_udelay` on the TSC: a wait as long as asked while the TSC runs, and
 * one that ends after TSC_STALL_READS reads once it stops, which would
 * otherwise spin for good with interrupts off. The model put back. */
static void tsc_wait_rows(void)
{
    void (*saved_wait)(uint32_t) = udelay_wait;
    uint32_t saved_per_tick = tsc_per_tick;
    uint64_t from;

    tsc_khz = 2893417u;
    tsc_per_tick = (uint32_t)(tsc_khz * 1000u / TICK_HZ);
    udelay_wait = tsc_wait_us;
    stop_at = 0;
    from = now_ns;
    k_udelay(1000u);
    CHECK(now_ns - from >= 1000000u, "a wait on a running TSC lasts as long as asked");

    tsc_still_value = tsc_base + now_ns * tsc_khz / 1000000u;
    tsc_still_reads = 0;
    tsc_still = 1;
    k_udelay(500000u);
    CHECK(tsc_still && tsc_still_reads == TSC_STALL_READS + 1,
          "a TSC that stops ends the wait on it after TSC_STALL_READS reads");
    tsc_still = 0;
    tsc_per_tick = saved_per_tick;
    udelay_wait = saved_wait;
}

/** The rows of KVM's clock pages, and the model put back. */
static void kvm_rows(void)
{
    void (*saved_wait)(uint32_t) = udelay_wait;

    reference(1);
    now_floor = 0;
    tsc_base = 0;
    kvm_follow_rows();
    kvm_ahead_rows();
    kvm_catchup_rows();
    kvm_source_rows();
    kvm_alone_rows();
    ref_kind = REF_NONE;
    kvm_counts_model = 0;
    kvm_stable_model = 0;
    tsc_per_tick = 0;
    udelay_wait = saved_wait;
}

/** The timer between ticks, as the header says, and the model put back. */
static void hr_rows(void)
{
    hr_wake_rows();
    hr_tick_rows();
    hr_switch_rows();
    hr_hold_rows();
    hr_model = 0;
    ref_kind = REF_NONE;
    hr_vector = 0;
    apic_running = 0;
}

/* Whether the service query allows the model to stop the tick. */
static int idle_allowed = 1;
static int64_t prepare_first, prepare_second;

/** Check idle timer transitions against the modeled device. */
static void idle_transitions(void)
{
    struct hr_state *c = &hr_cpu[0];
    uint32_t writes;

    hr_model = 1;
    apic_khz = 1000000u;
    ref_kind = REF_TSC;
    raised = 0;
    now_ns = EDGE(100);
    apic_timer_start(TICK_NS, TICK_HZ);
    apic_hr_init(TICK_NS);
    CHECK(timer_idle_enter() && c->idle && c->oneshot && apic_initial == 0,
          "an idle processor with no event stops its timer");
    now_ns = EDGE(101);
    CHECK(!(take_hr_event(APIC_TIMER_VECTOR) & APIC_HR_TICK),
          "a stale pending tick does not count in idle");
    writes = apic_writes;
    timer_wake_at((int64_t)EDGE(250));
    CHECK(c->want == (int64_t)EDGE(250) && apic_writes == writes,
          "idle callbacks retain an event without programming before selection");
    CHECK(timer_idle_enter() && apic_vector == APIC_TIMER_VECTOR &&
              apic_initial == APIC_COUNTS(EDGE(149.0)),
          "the next event is armed for its full interval without intervening ticks");
    writes = apic_writes;
    CHECK(timer_idle_enter() && apic_writes == writes,
          "an unchanged idle event is not written to the device again");
    now_ns = EDGE(250) + 1;
    CHECK(take_hr_event(APIC_TIMER_VECTOR) && !c->want && lapic_timer_current() == 0,
          "the event is served and the timer stops again");
    timer_idle_exit();
    CHECK(!c->idle && c->tick > (int64_t)now_ns && c->tick <= (int64_t)now_ns + TICK_NS &&
              apic_vector == APIC_TIMER_VECTOR,
          "waking restarts the phased tick without replaying idle ticks");
    timer_wake_at((int64_t)EDGE(250.1));
    now_ns = EDGE(250.1) + 1;
    pass_next = (int64_t)EDGE(250.2);
    writes = apic_writes;
    CHECK(take_hr_event(APIC_TIMER_VECTOR) && c->want == (int64_t)EDGE(250.2) &&
              apic_writes == writes + 1,
          "a busy event programs once after its queues choose an earlier deadline");
    now_ns = EDGE(250.2) + 1;
    CHECK(take_hr_event(APIC_TIMER_VECTOR), "the queued event is taken before the next idle wait");
    timer_wake_at((int64_t)EDGE(250.98));
    now_ns = EDGE(250.98) + 1;
    CHECK(take_hr_event(APIC_TIMER_VECTOR) == APIC_HR_EVENT && c->tick == (int64_t)EDGE(251),
          "a short event inside tick slack does not take the future scheduler tick");
    timer_wake_at((int64_t)EDGE(251));
    now_ns = EDGE(251) + 1;
    CHECK(take_hr_event(APIC_TIMER_VECTOR) == (APIC_HR_TICK | APIC_HR_EVENT) && c->want == 0 &&
              c->tick == (int64_t)EDGE(252),
          "one physical interrupt serves a coincident tick and event once each");
    now_ns = EDGE(251.25);
    CHECK(timer_idle_enter(), "a later idle wait stops the tick again");
    timer_wake_tick(300);
    CHECK(c->want == (int64_t)EDGE(300), "a whole-tick deadline keeps its absolute time");
    now_ns = EDGE(301);
    CHECK(take_hr_event(APIC_TIMER_VECTOR), "the whole-tick event is taken");
    hr_deadline = 1;
    tsc_per_tick = 2u * TICK_NS;
    tsc_measured = 1;
    tsc_khz = 2000000u;
    timer_wake_at((int64_t)EDGE(400));
    CHECK(timer_idle_enter() && deadline_at == tsc_ordered() + 2u * EDGE(99) + 1u,
          "TSC-deadline mode uses the TSC rate for the event");
    now_ns = EDGE(400) + 1;
    deadline_at = 0; /* The APIC clears the MSR when it raises the interrupt. */
    CHECK(take_hr_event(APIC_TIMER_VECTOR) && deadline_at == 0,
          "an idle TSC deadline is cleared after its event");
    timer_idle_exit();
    CHECK(deadline_at > tsc_ordered(), "a busy processor has its next deadline armed");
    hr_deadline = 0;
}

/** Continue with coarse-lag and first-deadline transitions. */
static void idle_coarse(void)
{
    struct hr_state *c = &hr_cpu[0];

    now_ns = EDGE(600.75);
    apic_timer_start(TICK_NS, TICK_HZ);
    CHECK(timer_idle_enter(), "idle stops before a tick-only expiry during a slew");
    ref.mult = 1;
    ref.tick = 2u * TICK_NS;
    ref.edge = tsc_ordered() - 3u * TICK_NS / 2u;
    ref.frac = 0;
    coarse_model = 599;
    timer_wake_tick(600);
    CHECK(c->want == (int64_t)EDGE(601) + 1 && c->want > (int64_t)now_ns,
          "a coarse tick still to count waits for the raw edge, not a time already passed");
    coarse_model = (uint64_t)-1;
    now_ns = EDGE(601.001);
    CHECK(take_hr_event(APIC_TIMER_VECTOR) && c->want == 0,
          "the deferred whole-tick event is taken");
    timer_idle_exit();
    apic_timer_start(TICK_NS, TICK_HZ);
    hr_deadline = 1;
    CHECK(timer_idle_enter() && !apic_periodic && deadline_at == 0,
          "first idle without work leaves periodic mode before clearing a TSC deadline");
    idle_allowed = 0;
    CHECK(!timer_idle_enter() && !c->idle && deadline_at > tsc_ordered(),
          "a failed prepare restarts an already stopped idle timer");
    idle_allowed = 1;
    hr_deadline = 0;
}

/** Continue with stable conversions, held tags and replacements. */
static void idle_tick_cache(void)
{
    struct hr_state *c = &hr_cpu[0];
    uint32_t writes;

    now_ns = EDGE(700.25);
    apic_timer_start(TICK_NS, TICK_HZ);
    CHECK(timer_idle_enter(), "stop before converting a repeated whole-tick event");
    phase_lag_ns = TICK_NS / 1000u;
    timer_wake_tick(800);
    writes = apic_writes;
    phase_lag_ns = 0;
    timer_wake_tick(800);
    CHECK(c->want == (int64_t)EDGE(800.001) && apic_writes == writes,
          "one live whole-tick request keeps its first conversion and device program");
    now_ns = EDGE(800.002);
    CHECK(take_hr_event(APIC_TIMER_VECTOR), "the cached whole-tick event is consumed");
    now_ns = EDGE(900.25);
    phase_stop_ns = EDGE(10);
    timer_wake_tick(1000);
    CHECK(c->want == (int64_t)EDGE(1000),
          "a stop between the phase and clock reads does not postpone the target");
    now_ns = EDGE(1000.002);
    CHECK(take_hr_event(APIC_TIMER_VECTOR) && !c->want_tick,
          "consuming an event clears its whole-tick tag");
    now_ns = EDGE(1100.25);
    apic_timer_start(TICK_NS, TICK_HZ);
    CHECK(timer_idle_enter(), "stop before a held whole-tick request");
    timer_wake_hold();
    phase_lag_ns = TICK_NS / 1000u;
    timer_wake_tick(1200);
    phase_lag_ns = 0;
    timer_wake_tick(1200);
    CHECK(c->held == (int64_t)EDGE(1200.001) && c->held_tick == 1200,
          "a held tick also keeps its first conversion");
    timer_wake_release();
    CHECK(c->want == (int64_t)EDGE(1200.001) && c->want_tick == 1200 && !c->held_tick,
          "release transfers the deadline and tag together");
    timer_wake_at((int64_t)EDGE(1100.5));
    CHECK(!c->want_tick, "an earlier direct time replaces the tag with zero");
    now_ns = EDGE(1100.501);
    CHECK(take_hr_event(APIC_TIMER_VECTOR) && !c->want_tick, "the direct event clears its request");
    timer_wake_tick(1200);
    CHECK(c->want == (int64_t)EDGE(1200) && c->want_tick == 1200,
          "the replaced tick may be requested again without a stale cache hit");
}

/** Continue with a staggered start, a long gap and a pending tick. */
static void idle_phase(void)
{
    struct hr_state *c = &hr_cpu[0];

    now_ns = EDGE(1700.25);
    apic_timer_start(TICK_NS, TICK_HZ);
    now_ns = EDGE(1700.3);
    timer_wake_at((int64_t)EDGE(1750));
    CHECK(c->tick == (int64_t)EDGE(1701),
          "a staggered periodic timer joins the common monotonic tick phase");
    CHECK(timer_idle_enter(), "stop the aligned scheduler tick");
    now_ns = EDGE(1750.001);
    CHECK(take_hr_event(APIC_TIMER_VECTOR), "the idle event is served on the shared vector");
    now_ns = EDGE(6700.4);
    timer_idle_exit();
    CHECK(c->tick == (int64_t)EDGE(6701), "a long idle span keeps the common tick phase");
    CHECK(timer_idle_enter(), "stop before an exact-edge restart");
    now_ns = EDGE(6701);
    timer_idle_exit();
    CHECK(c->tick == (int64_t)EDGE(6702), "an exact edge restarts at the strictly later edge");
    now_ns = EDGE(6800.25);
    apic_timer_start(TICK_NS, TICK_HZ);
    now_ns = EDGE(6801.3);
    raised = APIC_TIMER_VECTOR;
    timer_wake_at((int64_t)EDGE(6850));
    raised = 0;
    CHECK(c->tick == (int64_t)EDGE(6801) &&
              (take_hr_event(APIC_TIMER_VECTOR) & APIC_HR_TICK) != 0 &&
              c->tick == (int64_t)EDGE(6802),
          "a pending periodic interrupt serves the previous common edge once");
}

/** Keep an already idle CPU stopped when ready work declines the halt,
 * and combine its service requests into one device write. */
static void idle_prepare_rows(void)
{
    struct hr_state *c = &hr_cpu[0];
    uint32_t writes;

    now_ns = EDGE(7000);
    raised = 0;
    apic_timer_start(TICK_NS, TICK_HZ);
    idle_allowed = -1;
    writes = apic_writes;
    CHECK(!timer_idle_enter() && !c->idle && apic_writes == writes,
          "ready work declines a first idle stop without writing the timer");
    idle_allowed = 1;
    CHECK(timer_idle_enter(), "enter idle before testing a declined reentry");
    idle_allowed = -1;
    writes = apic_writes;
    CHECK(timer_idle_enter() && c->idle && apic_writes == writes,
          "a declined reentry preserves the stopped device until selection");
    timer_wake_at((int64_t)EDGE(9500));
    CHECK(c->want == (int64_t)EDGE(9500) && apic_writes == writes,
          "a woken idle CPU keeps a new desired event without writing the device");
    idle_allowed = 1;
    prepare_first = (int64_t)EDGE(9000);
    prepare_second = (int64_t)EDGE(8000);
    writes = apic_writes;
    CHECK(timer_idle_enter() && c->want == prepare_second && apic_writes == writes + 1,
          "idle preparation programs its earliest service once after the queries");
    prepare_first = prepare_second = 0;
}

/** Physical CPUs and guests share the 250 Hz phase and independent events. */
static void idle_guest_period(void)
{
    struct hr_state *c = &hr_cpu[0];
    int guest;

    for (guest = 0; guest <= 1; guest++) {
        guest_model = guest;
        apic_hr_init(TICK_NS);
        now_ns = EDGE(10000.2);
        apic_timer_start(TICK_NS, TICK_HZ);
        CHECK(timer_tick_period() == 1, "one logical tick per scheduler period");
        now_ns = EDGE(10001.2);
        timer_wake_at((int64_t)now_ns + 1000000);
        CHECK(c->oneshot && c->tick == (int64_t)EDGE(10002),
              "a fine event joins the common four-millisecond phase");
        now_ns = EDGE(10001.2) + 1000000u;
        CHECK(take_hr_event(APIC_TIMER_VECTOR) == APIC_HR_EVENT,
              "a fine event takes no future scheduler tick");
        now_ns = EDGE(10002);
        CHECK((take_hr_event(APIC_TIMER_VECTOR) & APIC_HR_TICK) != 0 &&
                  c->due == (int64_t)EDGE(10002) && c->tick == (int64_t)EDGE(10003),
              "one scheduler event advances one four-millisecond unit");
        CHECK(timer_idle_enter(), "stop before a long idle span");
        now_ns = EDGE(15000.4);
        timer_idle_exit();
        CHECK(c->tick == (int64_t)EDGE(15001), "idle preserves the common phase");
    }
    guest_model = 0;
}

/** Deferred requests in a retained idle wait are committed at a halt or
 * selection, including held events when stopped preparation fails. The
 * process pointer is used only as a unique identity by this clock model. */
static void idle_context_rows(void)
{
    struct hr_state *c = &hr_cpu[0];
    uint32_t writes;

    now_ns = EDGE(16000);
    apic_timer_start(TICK_NS, TICK_HZ);
    timer_wake_at((int64_t)EDGE(16050));
    cpu_table[0].current = cpu_table[0].idle = (struct process *)(void *)c;
    cpu_table[0].idle_wait = 1;
    writes = apic_writes;
    timer_wake_at((int64_t)EDGE(16000.1));
    CHECK(apic_writes == writes && c->programmed != 0 && timer_idle_pending(),
          "an earlier retained-idle request is dirty despite an older nonzero device expiry");
    timer_idle_retain();
    CHECK(apic_writes == writes + 1 && !timer_idle_pending() && !c->idle,
          "retained halt commits the desired event once without stopping the tick");
    prepare_first = (int64_t)EDGE(16000.02);
    prepare_second = (int64_t)EDGE(16000.03);
    idle_allowed = 0;
    writes = apic_writes;
    CHECK(!timer_idle_enter() && c->want == prepare_first && !timer_idle_pending() &&
              apic_writes == writes + 1,
          "failed stopped preparation merges held requests before committing the running timer");
    prepare_first = prepare_second = 0;
    timer_wake_at((int64_t)EDGE(16000.01));
    writes = apic_writes;
    timer_idle_exit();
    CHECK(apic_writes == writes + 1 && !timer_idle_pending(),
          "selection commits a retained idle timer even when its tick was never stopped");
    cpu_table[0].current = cpu_table[0].idle = NULL;
    cpu_table[0].idle_wait = 0;
    idle_allowed = 1;
}

/** Run the idle-event scenario and restore the calibration model. */
static void idle_rows(void)
{
    idle_transitions();
    idle_coarse();
    idle_tick_cache();
    idle_phase();
    idle_prepare_rows();
    idle_guest_period();
    idle_context_rows();
    hr_model = 0;
    ref_kind = REF_NONE;
    hr_vector = 0;
}

/** Run the rows: 0 when every check passes, 1 otherwise. */
int main(int argc, char **argv)
{
    uint32_t counts;

    if (argc == 2 && strcmp(argv[1], "--hz250") == 0) {
        hr_model = 1;
        apic_khz = 1000000u;
        ref_kind = REF_TSC;
        idle_guest_period();
        printf("timercal 250 Hz: %s\n", fails ? "FAIL" : "OK");
        return fails != 0;
    }
    if (argc == 2 && strcmp(argv[1], "--idle") == 0) {
        idle_rows();
        printf("timercal idle: %s\n", fails ? "FAIL" : "OK");
        return fails != 0;
    }

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
    held_rows((uint32_t)((uint64_t)tsc_khz * 1000u / TICK_HZ));

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
    tsc_one_rate_bit = 0;
    CHECK(timer_calibrate() == 0, "a TSC whose rate varies is not measured");
    tsc_measured = 0;
    cmdline_flag = "rach2.tscreliable";
    CHECK(timer_calibrate() != 0, "unless rach2.tscreliable vouches for it");
    cmdline_flag = NULL;
    tsc_per_tick = 0;
    reference(1);
    tsc_measured = 0;
    tsc_one_rate_bit = 1;
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

    tsc_wait_rows();
    kvm_rows();
    hr_rows();
    idle_rows();

    if (fails) {
        printf("timercal_hosttest: %d check(s) failed\n", fails);
        return 1;
    }
    printf("timercal_hosttest: all checks passed\n");
    return 0;
}

/** No device services constrain the modeled idle timer. */
int timer_idle_prepare(void)
{
    if (prepare_first) {
        timer_wake_at(prepare_first);
        timer_wake_at(prepare_second);
    }
    return idle_allowed;
}

/** The model's TSC, without an access delay. */
uint64_t tsc_ordered(void)
{
    return tsc_base + now_ns * tsc_khz / 1000000u;
}

/** The deadline written to the modeled TSC's timer. */

/** Keep the deadline MSR; no other MSR is used by the timer. */
void kcpu_wrmsr(uint32_t msr, uint32_t lo, uint32_t hi)
{
    CHECK(msr == 0x6E0u, "only the TSC deadline is written");
    deadline_at = ((uint64_t)hi << 32) | lo;
}

/** Select the model's deadline mode. */
void lapic_timer_deadline(uint8_t vector)
{
    apic_vector = vector;
    apic_periodic = 0;
}

/** The modeled clock's whole ticks and fractional tick. */
void rtc_tick_phase(uint64_t *ticks, uint32_t *phase)
{
    uint64_t at = now_ns - phase_lag_ns;

    *ticks = at / TICK_NS;
    *phase = (uint32_t)(at % TICK_NS);
    now_ns += phase_stop_ns;
    phase_stop_ns = 0;
}

/** The model's coarse count, or its explicit lag behind the smooth clock. */
uint64_t tick_count(void)
{
    return coarse_model == (uint64_t)-1 ? now_ns / TICK_NS : coarse_model;
}

/** No slew of the model's raw interval; its coarse lag is set separately. */
int64_t rtc_mono_rate(void)
{
    return 0;
}

/** Convert a raw interval in the model's unchanged rate. */
uint64_t rtc_slewed(uint64_t ns, int64_t rate)
{
    (void)rate;
    return ns;
}

/** No watchdog is modeled by the APIC timer rows. */
void watchdog_event(isr_params_t *params)
{
    (void)params;
}
