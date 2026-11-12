/*
 * vclock_hosttest.c: the x86-64 vDSO's clock of rach2/sys/arch/x86_64/
 * vclock.c, built, started, ticked and stopped against a model machine.
 *
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/kirq_none.c
 * host-cflags: -DRACH2_HOST_X86_64
 *
 * The source is included whole, so that a row reaches the page and the
 * clock's state. The machine is a clock of nanoseconds, KVM's clock, which
 * is one clock on every processor, and a TSC of 2 GHz, two counts a
 * nanosecond, on the boot processor; the processor running the code may
 * have a TSC some counts ahead of it (`tsc_ahead`). KVM's clock pages, when
 * kept, give each processor's TSC with KVM's clock at it. The tick's clocks,
 * which the clock is steered toward, read the machine's clock exactly, and
 * `rtc_vclock_change` keeps clock.c's floors as clock.c does.
 *
 * The rows check that a timer interrupt reading the clock at any step of
 * the data page's build, before and after the frame is cleared, reads
 * nothing and does not wait on the frame's count (`page_rows`); and that
 * the stop, made on a processor whose TSC is a second ahead of the one the
 * page's base was read on, carries the clock on by KVM's clock and raises
 * the floors to the moment of the stop and no further, and that the clock
 * started again begins at the floors (`stop_rows`).
 */

#include "arch/x86_64/vclock.c"

extern int printf(const char *fmt, ...);

static int fails;

/** Count a failure when `cond` is false, and print what it checks. */
#define CHECK(cond, what)                 \
    do {                                  \
        if (!(cond)) {                    \
            fails++;                      \
            printf("  FAIL: %s\n", what); \
        }                                 \
    } while (0)

/* The machine: KVM's clock in nanoseconds; how far the running processor's
 * TSC is ahead of the boot processor's, in counts; whether KVM's pages are
 * kept and have withdrawn their stable bit. */
static uint64_t now_ns;
static uint64_t tsc_ahead;
static int kvm_kept_model;
static int kvm_withdrawn_model;

/* The interrupt model: whether a step of the page's build reads the clock
 * as a timer interrupt would, the readings it made, those that read
 * something, and the spins a reading made on the page's count. */
static int irq_model;
static int irq_reads, irq_read_something, irq_spins;

/* clock.c's floors as `rtc_vclock_change` keeps them. */
static uint64_t floor_mono, floor_raw;

/* The data page's frame, uncleared: every byte all ones, so its count is
 * odd. */
static union {
    unsigned char bytes[PAGE_SIZE];
    uint64_t align;
} frame;

/** A timer interrupt's reading of the clock, when the row models one. */
static void irq_read(void)
{
    int64_t sec, nsec;

    if (!irq_model) {
        return;
    }
    irq_reads++;
    if (vclock_read(VCLOCK_READ_MONO, 0, &sec, &nsec)) {
        irq_read_something++;
    }
}

/** The running processor's TSC. */
uint64_t tsc_ordered(void)
{
    return now_ns * 2u + tsc_ahead;
}

/** The running processor's TSC with KVM's clock at it, while pages are
 * kept. */
int tsc_kvm_now(uint64_t *tsc, uint64_t *ns)
{
    if (!kvm_kept_model) {
        return 0;
    }
    *tsc = tsc_ordered();
    *ns = now_ns;
    return 1;
}

/** Whether the pages have withdrawn their stable bit. */
int tsc_kvm_withdrawn(void)
{
    return kvm_kept_model && kvm_withdrawn_model;
}

/** The TSC's counts in a tick: 2 GHz. */
uint32_t tsc_counts_per_tick(void)
{
    return 2u * TICK_NS;
}

/** Every processor's TSC agreed at its start. */
int tsc_synced(int *cpu, uint64_t *behind)
{
    *cpu = 0;
    *behind = 0;
    return 1;
}

/** `rdtscp` is there; a step of the page's build. */
int tsc_has_rdtscp(void)
{
    irq_read();
    return 1;
}

/** Nothing to set on the model's processor. */
void tsc_cpu_setup(int cpu)
{
    (void)cpu;
}

/** The images build. */
int vdso_build(void)
{
    return 0;
}

/** The frame, uncleared. */
void *alloc_kernel_frame(void)
{
    unsigned int i;

    for (i = 0; i < PAGE_SIZE; i++) {
        frame.bytes[i] = 0xFF;
    }
    return frame.bytes;
}

/** The frame's address; a step of the page's build. */
phys_addr_t mmu_virt_to_phys(const volatile void *v)
{
    irq_read();
    return (phys_addr_t)(uintptr_t)v;
}

/** Fill `n` bytes at `p` with `c`, an interrupt first, while the frame is
 * uncleared. */
void *k_memset(void *p, int c, size_t n)
{
    unsigned char *b = p;

    irq_read();
    while (n-- > 0) {
        *b++ = (unsigned char)c;
    }
    irq_read();
    return p;
}

/** A spin on the page's count: counted, and the count made even so that the
 * reading goes on. */
void arch_relax(void)
{
    irq_spins++;
    if (data) {
        data->seq &= ~1u;
    }
}

/** The tick's clocks: the machine's clock. */
void rtc_tick_both(uint64_t *mono_ns, uint64_t *raw_ns)
{
    *mono_ns = now_ns;
    *raw_ns = now_ns;
}

/** The NTP state slews nothing. */
uint64_t rtc_slewed(uint64_t ns, int64_t rate)
{
    (void)rate;
    return ns;
}

int64_t rtc_mono_rate(void)
{
    return 0;
}

/** clock.c's hand-over: `change` is given the floors and raises them. */
void rtc_vclock_change(void (*change)(uint64_t *mono_ns, uint64_t *raw_ns))
{
    uint64_t mono = floor_mono, raw = floor_raw;

    change(&mono, &raw);
    floor_mono = mono;
    floor_raw = raw;
}

/** Discard the log. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Whether `a` and `b` are within `slack` nanoseconds of each other. */
static int near(uint64_t a, uint64_t b, uint64_t slack)
{
    return a > b ? a - b <= slack : b - a <= slack;
}

/**
 * The page built and the clock started with KVM's pages kept and a timer
 * interrupt reading the clock at each step of the build: no reading reads
 * anything or spins on the uncleared frame's odd count; the started clock
 * then reads the machine's clock.
 */
static void page_rows(void)
{
    int64_t sec, nsec;

    now_ns = 5000000000ULL;
    kvm_kept_model = 1;
    irq_model = 1;
    vclock_start();
    irq_model = 0;
    CHECK(irq_reads >= 4 && irq_read_something == 0,
          "an interrupt at each step of the page's build reads nothing");
    CHECK(irq_spins == 0, "and waits on no count of the uncleared frame");
    CHECK(vclock_running() && data->seq % 2u == 0, "the clock starts once the page is whole");
    CHECK(vclock_read(VCLOCK_READ_MONO, 0, &sec, &nsec) && sec == 5 && nsec == 0,
          "and reads the machine's clock");
}

/**
 * Ticks a millisecond apart, then KVM's stable bit withdrawn and the stop
 * made 300 us after the last tick on a processor whose TSC is a second
 * ahead: the floors become the moment of the stop, by KVM's clock, and not
 * the second ahead. The bit set again, on a processor whose TSC agrees,
 * starts the clock at the floors.
 */
static void stop_rows(void)
{
    int64_t sec, nsec;
    int i;

    for (i = 0; i < 3; i++) {
        now_ns += TICK_NS;
        vclock_tick();
    }
    now_ns += 300000u;
    tsc_ahead = 2000000000u;
    kvm_withdrawn_model = 1;
    vclock_tick();
    CHECK(!vclock_running() && data->mode == VCLOCK_NONE, "a withdrawn stable bit stops the clock");
    CHECK(near(floor_mono, now_ns, 1000u),
          "the monotonic floor is the stop's moment by KVM's clock, not the TSC a second ahead");
    CHECK(near(floor_raw, now_ns, 1000u), "and so is the raw floor");
    tsc_ahead = 0;
    kvm_withdrawn_model = 0;
    now_ns += TICK_NS;
    vclock_tick();
    CHECK(vclock_running() && data->mode == VCLOCK_TSC, "the bit set again starts the clock");
    CHECK(vclock_read(VCLOCK_READ_MONO, 0, &sec, &nsec) &&
              (uint64_t)sec * NSEC_PER_SEC + (uint64_t)nsec >= floor_mono,
          "at or past the floor");
}

/** Run the rows: 0 when every check passes, 1 otherwise. */
int main(void)
{
    page_rows();
    stop_rows();
    return fails != 0;
}
