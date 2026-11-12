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
 * kept, give each processor's TSC with KVM's clock at it, and the boot
 * processor's page, which the vDSO checks, its stable bit, which KVM
 * withdraws before the tick reads its own page. The tick's clocks, which
 * the clock is steered toward, read the machine's clock, less a lag a row
 * may set, and `rtc_vclock_change` keeps clock.c's floors as clock.c does.
 *
 * The rows check that a timer interrupt reading the clock at any step of
 * the data page's build, before and after the frame is cleared, reads
 * nothing and does not wait on the frame's count (`page_rows`); that a
 * reading the vDSO makes between the stable bit's withdrawal and the stop,
 * on a processor whose TSC is a second ahead of the one the page's base
 * was read on, reads nothing, so that the system call reads the clock,
 * carried on by KVM's clock; that the stop then raises the floors to its
 * moment by KVM's clock and no further, at or past every reading given;
 * that a setting of real time and a tick made between the withdrawal and
 * the stop carry the bases by KVM's clock, not by that TSC; and that the
 * clock started again while the tick's clock is still behind the stop's
 * time begins at the floors (`stop_rows`).
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

/* Whether the boot processor's page has withdrawn its stable bit, which
 * the vDSO reads inside each reading; and how far the tick's clocks are
 * behind the machine's, in nanoseconds. */
static int kvm_boot_withdrawn;
static uint64_t tick_lag_ns;

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

/** The running processor's page read whole: its TSC, KVM's clock at it
 * and its stable bit, which KVM withdraws on every page together, while
 * pages are kept. */
int tsc_kvm_read(struct tsc_kvm_sample *s)
{
    if (!kvm_kept_model) {
        return 0;
    }
    s->tsc = tsc_ordered();
    s->ns = now_ns;
    s->counts = 2u * TICK_NS;
    s->stable = !kvm_boot_withdrawn;
    return 1;
}

/** The TSC read while the boot processor's page keeps its stable bit. */
int tsc_kvm_boot_stable(uint64_t *tsc)
{
    if (!kvm_kept_model || kvm_boot_withdrawn) {
        return 0;
    }
    *tsc = tsc_ordered();
    return 1;
}

/** Whether the pages have withdrawn their stable bit, as the last tick
 * read them. */
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

/** The tick's clocks: the machine's clock, less the lag. */
void rtc_tick_both(uint64_t *mono_ns, uint64_t *raw_ns)
{
    *mono_ns = now_ns - tick_lag_ns;
    *raw_ns = now_ns - tick_lag_ns;
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

/** clock.c's hand-over: `change` is given the floors, which are raised to
 * what it gives back and never lowered, as `rtc_floor` keeps them. */
void rtc_vclock_change(void (*change)(uint64_t *mono_ns, uint64_t *raw_ns))
{
    uint64_t mono = floor_mono, raw = floor_raw;

    change(&mono, &raw);
    if (mono > floor_mono) {
        floor_mono = mono;
    }
    if (raw > floor_raw) {
        floor_raw = raw;
    }
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

/** The monotonic clock as the system call reads it, clock.c's
 * `rtc_monotonic`: the clock, or the tick's clock while it does not run,
 * held to the floor, which it raises. */
static uint64_t syscall_mono(void)
{
    int64_t sec, nsec;
    uint64_t ns, raw;

    if (vclock_read(VCLOCK_READ_MONO, 0, &sec, &nsec)) {
        ns = (uint64_t)sec * NSEC_PER_SEC + (uint64_t)nsec;
    } else {
        rtc_tick_both(&ns, &raw);
    }
    if (ns < floor_mono) {
        ns = floor_mono;
    }
    floor_mono = ns;
    return ns;
}

/**
 * Ticks a millisecond apart, then, 300 us after the last, KVM's stable bit
 * withdrawn on the boot processor's page, which the tick has not read yet,
 * and a program on a processor whose TSC is a second ahead reading the
 * clock: the system call reads the clock carried on by KVM's clock, and
 * pairs no TSC count with the time; 5 us later the vDSO's reading,
 * `vclock_pass` as vdsocode.s makes it, reads nothing, where without the
 * check of the page it reads the TSC a second ahead, 6.003305 s, which
 * updates no floor. A setting of real time 2 us later, which leaves it
 * where it was, as clock.c's step of nothing does, and 3 us after that a
 * tick that read its page before the withdrawal, each carry the monotonic
 * and raw bases to their moment by KVM's clock through the base's anchor,
 * where the TSC a second ahead would add a second to each. The stop, 10 us
 * later on the same processor, makes the floors the moment of the stop, by
 * KVM's clock, 5.00332 s, at or past every reading given and not the
 * second ahead. The bit set again, on a
 * processor whose TSC agrees, starts the clock while the tick's clock, a
 * millisecond behind the machine's, has not reached the stop's time, so
 * that only the floor puts the clock's first reading at or past it.
 */
static void stop_rows(void)
{
    uint64_t vdso_ns = 0, sys_ns, ns, cycles, ref, raw;
    int64_t sec;
    int found;
    int i;

    for (i = 0; i < 3; i++) {
        now_ns += TICK_NS;
        vclock_tick();
    }
    CHECK(data->mode == VCLOCK_PVCLOCK, "the clock reads KVM's page where its pages are kept");
    now_ns += 300000u;
    tsc_ahead = 2000000000u;
    kvm_boot_withdrawn = 1;
    sys_ns = syscall_mono();
    CHECK(near(sys_ns, now_ns, 1000u),
          "after the withdrawal the system call reads the clock carried by KVM's clock, "
          "not the TSC a second ahead");
    CHECK(!vclock_read_cycles(VCLOCK_READ_RAW, &ns, &cycles),
          "and the kernel pairs no TSC count with the time");
    now_ns += 5000u;
    found = vclock_pass(VCLOCK_READ_MONO, 0, &sec, &ns, &cycles);
    if (found == VREAD_OK) {
        vdso_ns = (uint64_t)sec * NSEC_PER_SEC + ns;
    }
    CHECK(found != VREAD_OK, "the vDSO's reading after the withdrawal makes the system call");
    now_ns += 2000u;
    vclock_set_wall(0, 0);
    CHECK(near(mono_base_ns, now_ns, 1000u) && near(raw_base_ns, now_ns, 1000u),
          "a setting of real time after the withdrawal carries the bases by KVM's clock, "
          "not by the TSC a second ahead");
    now_ns += 3000u;
    vclock_tick();
    CHECK(vclock_running() && near(mono_base_ns, now_ns, 1000u) && near(raw_base_ns, now_ns, 1000u),
          "and so does a tick that read its page before the withdrawal");
    now_ns += 10000u;
    kvm_withdrawn_model = 1;
    vclock_tick();
    CHECK(!vclock_running() && data->mode == VCLOCK_NONE, "a withdrawn stable bit stops the clock");
    CHECK(near(floor_mono, now_ns, 1000u),
          "the monotonic floor is the stop's moment by KVM's clock, not the TSC a second ahead");
    CHECK(near(floor_raw, now_ns, 1000u), "and so is the raw floor");
    CHECK(floor_mono >= vdso_ns && floor_mono >= sys_ns,
          "and no reading given before the stop is later than the floor");
    tsc_ahead = 0;
    kvm_boot_withdrawn = 0;
    kvm_withdrawn_model = 0;
    tick_lag_ns = TICK_NS;
    now_ns += 100000u;
    rtc_tick_both(&ref, &raw);
    CHECK(ref < floor_mono, "the tick's clock has not reached the stop's time at the start");
    vclock_tick();
    CHECK(vclock_running() && data->mode == VCLOCK_PVCLOCK, "the bit set again starts the clock");
    found = vclock_pass(VCLOCK_READ_MONO, 0, &sec, &ns, &cycles);
    CHECK(found == VREAD_OK && (uint64_t)sec * NSEC_PER_SEC + ns >= floor_mono,
          "and the vDSO reads it at or past the floor");
}

/** Run the rows: 0 when every check passes, 1 otherwise. */
int main(void)
{
    page_rows();
    stop_rows();
    return fails != 0;
}
