/*
 * vclock_hosttest.c: the x86-64 vDSO's clock of rach2/sys/arch/x86_64/
 * vclock.c, built, started, ticked and stopped against a model machine.
 *
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/kirq_none.c
 * host-cflags: -DRACH2_HOST_X86_64
 * host-inputs: rach2/sys/arch/x86_64/vdsocode.s
 * host-inputs: rach2/sys/arch/x86_64/vdso32code.s
 *
 * The source is included whole, so that a row reaches the page and the
 * clock's state. The machine is a clock of nanoseconds, KVM's clock, which
 * is one clock on every processor, and a TSC of 2 GHz, two counts a
 * nanosecond, on the boot processor, less the counts a row holds back to
 * model KVM moving its clock while the TSC stays (`tsc_held`); the
 * processor running the code may have a TSC some counts ahead of it
 * (`tsc_ahead`). KVM's clock pages, when kept, convert each processor's
 * TSC into KVM's clock, which a row may have the running processor's page
 * read some nanoseconds behind (`kvm_back_ns`), and give their stable bits,
 * the boot processor's page's, which the vDSO and the clock's stop and
 * start test, apart from the running processor's. The tick's clocks, which
 * the clock is steered toward, read the machine's clock, less a lag a row
 * may set, and `rtc_vclock_change` keeps clock.c's floors as clock.c does.
 *
 * The rows check that a timer interrupt reading the clock at any step of
 * the data page's build, before and after the frame is cleared, reads
 * nothing and does not wait on the frame's count (`page_rows`); that the
 * page counts KVM's clock, so that KVM moving its clock with the stable
 * bit kept and the TSC still moves the clock with it; that a tick whose
 * own processor's page withdrew its bit while the boot processor's keeps it
 * leaves the clock running, carried by KVM's clock; that a reading the
 * vDSO makes between the boot page's withdrawal and the stop, on a
 * processor whose TSC is a second ahead of the one the page's base was
 * read on, reads nothing, so that the system call reads the clock, carried
 * on by KVM's clock; that a reading of KVM's clock behind an earlier one
 * leaves the base where it was and counts no time twice; that the stop
 * then raises the floors to its moment by KVM's clock and no further, at
 * or past every reading given; that a setting of real time made between
 * the withdrawal and the stop carries the bases by KVM's clock, not by
 * that TSC; that a start the tick decided on, overtaken by a new
 * withdrawal, leaves the clock stopped and the floors where they were; and
 * that the clock started again while the tick's clock is still behind the
 * stop's time goes on from the stop's time at KVM's rate (`stop_rows`);
 * that readings without the bit, one behind another, and then the bit set
 * again before any tick, with the boot processor's page behind the last
 * reading the kernel gave, read no earlier than that reading, in the
 * kernel and in both vDSOs (`recovery_rows`, `abi_rows`); that the code of
 * both vDSOs, the x86-64 one and the 32-bit one, converts the TSC through
 * KVM's page, its stamp, rate, shift either way and system time, while the
 * page has its stable bit, and makes the system call once it is withdrawn
 * (`abi_rows`); that both vDSOs' gettimeofday give the time zone
 * settimeofday set from the data page, with no system call even where the
 * time needs one (`abi_rows`); and that a boot whose start finds the bit
 * withdrawn starts the clock at the tick once the bit is back, with a time
 * zone set before the page was built on it (`boot_rows`).
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
 * TSC is ahead of the boot processor's, and the counts every TSC is held
 * back by, in counts; how far behind KVM's clock the running processor's
 * page and the boot processor's page read it; and whether KVM's pages are
 * kept. */
static uint64_t now_ns;
static uint64_t tsc_ahead;
static uint64_t tsc_held;
static uint64_t kvm_back_ns;
static uint64_t kvm_boot_back_ns;
static int kvm_kept_model;

/* Whether the boot processor's page, which the vDSO and the clock's stop
 * and start read, and the running processor's page have withdrawn their
 * stable bits; whether KVM withdraws both inside the next
 * `rtc_vclock_change`, after the tick decided on it; and how far the
 * tick's clocks are behind the machine's, in nanoseconds. */
static int kvm_boot_withdrawn;
static int kvm_here_withdrawn;
static int withdraw_in_change;
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
    return now_ns * 2u + tsc_ahead - tsc_held;
}

/** The running processor's page read whole: its TSC, KVM's clock it
 * converts it to, `kvm_back_ns` behind, and its stable bit, while pages
 * are kept. */
int tsc_kvm_read(struct tsc_kvm_sample *s)
{
    if (!kvm_kept_model) {
        return 0;
    }
    s->tsc = tsc_ordered();
    s->ns = now_ns - kvm_back_ns;
    s->counts = 2u * TICK_NS;
    s->stable = !kvm_here_withdrawn;
    return 1;
}

/** The TSC and KVM's clock it converts to by the boot processor's page,
 * `kvm_boot_back_ns` behind, while that page keeps its stable bit. */
int tsc_kvm_boot_read(uint64_t *tsc, uint64_t *ns)
{
    if (!kvm_kept_model || kvm_boot_withdrawn) {
        return 0;
    }
    *tsc = tsc_ordered();
    *ns = now_ns - kvm_boot_back_ns;
    return 1;
}

/** Whether the boot processor's page has withdrawn its stable bit now. */
int tsc_kvm_boot_withdrawn(void)
{
    return kvm_kept_model && kvm_boot_withdrawn;
}

/** The TSC's counts in a tick: 2 GHz. */
uint32_t tsc_counts_per_tick(void)
{
    return 2u * TICK_NS;
}

/** The clock's rate, the trusted TSC's. */
uint32_t tsc_clock_per_tick(void)
{
    return tsc_counts_per_tick();
}

/** tick.c's 64-bit division, which the host does itself. */
uint64_t tick_div64(uint64_t n, uint64_t d, uint64_t *rem)
{
    if (rem) {
        *rem = n % d;
    }
    return n / d;
}

/** The watch's refinement of the TSC's rate, which the model does not
 * keep. */
void timer_tsc_watch_refine(uint32_t per_tick)
{
    (void)per_tick;
}

/** The reference clock's name in the records. */
const char *refclock_name(void)
{
    return "hpet";
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

/** The barrier of `sched_mb`: an exchange, on the one thread. */
unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value)
{
    unsigned int old = *p;

    *p = value;
    return old;
}

/* The kicks the clock sent other processors, the set of the last one and
 * the generation it went after. */
static int kicks;
static uint64_t kicked_cpus;
static uint32_t kicked_gen;

/** The clock's kick of other processors: recorded. */
void sched_kick_cpus(uint64_t cpus)
{
    kicks++;
    kicked_cpus = cpus;
    kicked_gen = generation;
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
 * what it gives back and never lowered, as `rtc_floor` keeps them; KVM
 * withdraws its bits first where the row asks (`withdraw_in_change`). */
void rtc_vclock_change(void (*change)(uint64_t *mono_ns, uint64_t *raw_ns))
{
    uint64_t mono = floor_mono, raw = floor_raw;

    if (withdraw_in_change) {
        kvm_boot_withdrawn = 1;
        kvm_here_withdrawn = 1;
    }
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

/** The monotonic clock as the kernel reads it, `vclock_read`, without
 * clock.c's floor, in nanoseconds; 0 while it does not run. */
static uint64_t kernel_mono(void)
{
    int64_t sec, nsec;

    if (!vclock_read(VCLOCK_READ_MONO, 0, &sec, &nsec)) {
        return 0;
    }
    return (uint64_t)sec * NSEC_PER_SEC + (uint64_t)nsec;
}

/** The monotonic clock as the vDSO reads it, `vclock_pass` as vdsocode.s
 * makes it, in nanoseconds into `*ns`: what the pass found. */
static int vdso_mono(uint64_t *ns)
{
    uint64_t nsec, cycles;
    int64_t sec;
    int found = vclock_pass(VCLOCK_READ_MONO, 0, &sec, &nsec, &cycles);

    *ns = found == VREAD_OK ? (uint64_t)sec * NSEC_PER_SEC + nsec : 0u;
    return found;
}

/**
 * Ticks a millisecond apart; then KVM moving its clock a second on with
 * the stable bit kept and the TSC still, as KVM_SET_CLOCK may: the vDSO's
 * next reading is a second on, since the page counts KVM's clock, where a
 * count of the TSC reads no time gone. A tick whose own processor's page
 * withdrew its bit, on a processor whose TSC is a second ahead, while the
 * boot processor's page keeps it, leaves the clock running and carries the
 * bases by KVM's clock.
 *
 * Then, 300 us after that tick, KVM's stable bit withdrawn on the boot
 * processor's page and a program on that processor reading the clock: the
 * system call reads the clock carried on by KVM's clock, and pairs no TSC
 * count with the time; 5 us later the vDSO's reading reads nothing, where
 * without the check of the page it reads the TSC a second ahead. A setting
 * of real time 2 us later, which leaves it where it was, as clock.c's step
 * of nothing does, carries the monotonic and raw bases to their moment by
 * KVM's clock, where the TSC a second ahead would add a second to each.
 * Two more settings, one with the running processor's page reading KVM's
 * clock 100 ns behind the first's moment and one at that moment again,
 * leave the bases where they were, with the page's bit withdrawn and with
 * it set: a base moved back by the first would have the second count the
 * 100 ns again. The stop, 10 us later, decided by the boot processor's page
 * alone, makes the floors the moment of the stop, by KVM's clock, at or
 * past every reading given and not the second ahead.
 *
 * The bit set again, on a processor whose TSC agrees, and withdrawn again
 * after the tick decided to start the clock: the clock stays stopped and
 * the floors stay. The bit set for good: the clock starts while the tick's
 * clock, a millisecond behind the machine's, has not reached the stop's
 * time, and its first reading is the stop's time, the floor, and one 1000
 * ns later 1000 ns past it.
 */
static void stop_rows(void)
{
    uint64_t vdso_ns = 0, sys_ns, ns, cycles, ref, raw, before, mono0, raw0, floor0;
    int found;
    uint32_t gen;
    int i;

    for (i = 0; i < 3; i++) {
        now_ns += TICK_NS;
        vclock_tick();
    }
    CHECK(data->mode == VCLOCK_PVCLOCK, "the clock reads KVM's page where its pages are kept");
    found = vdso_mono(&before);
    now_ns += NSEC_PER_SEC;
    tsc_held += 2u * NSEC_PER_SEC;
    CHECK(found == VREAD_OK && vdso_mono(&ns) == VREAD_OK && ns - before == NSEC_PER_SEC,
          "KVM moving its clock a second on with the stable bit kept and the TSC still moves "
          "the vDSO's reading a second on");
    vclock_tick();
    CHECK(vclock_running() && near(mono_base_ns, now_ns, 1000u),
          "and the tick carries the base by KVM's clock");
    now_ns += TICK_NS;
    tsc_ahead = 2000000000u;
    kvm_here_withdrawn = 1;
    vclock_tick();
    CHECK(vclock_running() && data->mode == VCLOCK_PVCLOCK,
          "a tick whose own page withdrew its bit, the boot processor's keeping it, "
          "leaves the clock running");
    CHECK(near(mono_base_ns, now_ns, 1000u) && near(raw_base_ns, now_ns, 1000u),
          "and carries the bases by KVM's clock, not the TSC a second ahead");
    now_ns += 300000u;
    kvm_boot_withdrawn = 1;
    sys_ns = syscall_mono();
    CHECK(near(sys_ns, now_ns, 1000u),
          "after the withdrawal the system call reads the clock carried by KVM's clock, "
          "not the TSC a second ahead");
    CHECK(!vclock_read_cycles(VCLOCK_READ_RAW, &ns, &cycles),
          "and the kernel pairs no TSC count with the time");
    now_ns += 5000u;
    found = vdso_mono(&ns);
    if (found == VREAD_OK) {
        vdso_ns = ns;
    }
    CHECK(found != VREAD_OK, "the vDSO's reading after the withdrawal makes the system call");
    now_ns += 2000u;
    vclock_set_wall(0, 0);
    CHECK(near(mono_base_ns, now_ns, 1000u) && near(raw_base_ns, now_ns, 1000u),
          "a setting of real time after the withdrawal carries the bases by KVM's clock, "
          "not by the TSC a second ahead");
    mono0 = mono_base_ns;
    raw0 = raw_base_ns;
    for (i = 0; i < 2; i++) {
        kvm_here_withdrawn = i == 0;
        kvm_back_ns = 100u;
        vclock_set_wall(0, 0);
        kvm_back_ns = 0;
        vclock_set_wall(0, 0);
        CHECK(mono_base_ns == mono0 && raw_base_ns == raw0 && data->cycle_base == now_ns,
              i == 0 ? "a reading of KVM's clock behind an earlier one, without the bit, moves "
                       "no base back and counts no time twice"
                     : "and so does one with the bit");
    }
    now_ns += 10000u;
    kicks = 0;
    gen = generation;
    vclock_tick();
    CHECK(!vclock_running() && data->mode == VCLOCK_NONE,
          "the boot processor's withdrawn bit stops the clock, its own page's set");
    CHECK(kicks == 1 && kicked_cpus == ~(uint64_t)0 && kicked_gen == gen + 1u,
          "and interrupts every other processor once, after the generation went up, so that "
          "a vCPU in its guest exits and decides its master clock again");
    CHECK(near(floor_mono, now_ns, 1000u),
          "the monotonic floor is the stop's moment by KVM's clock, not the TSC a second ahead");
    CHECK(near(floor_raw, now_ns, 1000u), "and so is the raw floor");
    CHECK(floor_mono >= vdso_ns && floor_mono >= sys_ns,
          "and no reading given before the stop is later than the floor");
    tsc_ahead = 0;
    kvm_boot_withdrawn = 0;
    tick_lag_ns = TICK_NS;
    now_ns += 100000u;
    floor0 = floor_mono;
    withdraw_in_change = 1;
    vclock_tick();
    withdraw_in_change = 0;
    CHECK(!vclock_running() && data->mode == VCLOCK_NONE && floor_mono == floor0,
          "a start the tick decided on, overtaken by a new withdrawal, leaves the clock "
          "stopped and the floors where they were");
    kvm_boot_withdrawn = 0;
    kvm_here_withdrawn = 0;
    rtc_tick_both(&ref, &raw);
    CHECK(ref < floor_mono, "the tick's clock has not reached the stop's time at the start");
    vclock_tick();
    CHECK(vclock_running() && data->mode == VCLOCK_PVCLOCK, "the bit set again starts the clock");
    CHECK(kicks == 1 && generation == gen + 2u,
          "the start raises the generation and interrupts no processor, as Linux kicks only "
          "when the clock leaves the TSC");
    found = vdso_mono(&ns);
    CHECK(found == VREAD_OK && ns == floor_mono,
          "and the vDSO reads it from the stop's time, the floor");
    now_ns += 1000u;
    found = vdso_mono(&ns);
    CHECK(found == VREAD_OK && ns == floor_mono + 1000u, "and on from there at KVM's rate");
}

/* The time the kernel gave without the bit that `recovery_rows` leaves the
 * page's base at, which no vDSO's reading after it may go behind. */
static uint64_t recovered_ns;

/**
 * KVM's stable bit withdrawn on every page a tick after the clock ran on,
 * and a reading 100 ns past the base the tick left; then a second reading
 * without the bit, of KVM's clock 50 ns behind the first and so still past
 * that base: it reads no earlier than the first. The bit set again before
 * any tick, with the boot processor's page converting to KVM's clock 50 ns
 * behind the first reading: the vDSO's reading of that page and the
 * kernel's read no earlier than the first, the time a system call gave,
 * where a base left at the tick's count would have them read 50 ns
 * earlier. The page is left so for `abi_rows`.
 */
static void recovery_rows(void)
{
    uint64_t base0, t1, t2, ns;
    int found;

    now_ns += TICK_NS;
    vclock_tick();
    base0 = data->cycle_base;
    kvm_boot_withdrawn = 1;
    kvm_here_withdrawn = 1;
    now_ns += 100u;
    t1 = kernel_mono();
    CHECK(t1 != 0 && data->cycle_base == base0 + 100u,
          "a reading without the bit reads the clock 100 ns past the tick's base");
    kvm_back_ns = 50u;
    t2 = kernel_mono();
    CHECK(t2 >= t1, "a second reading without the bit, of KVM's clock 50 ns behind the first "
                    "and past the tick's base, reads no earlier than the first");
    kvm_back_ns = 0;
    kvm_boot_withdrawn = 0;
    kvm_here_withdrawn = 0;
    kvm_boot_back_ns = 50u;
    found = vdso_mono(&ns);
    CHECK(found == VREAD_OK && ns >= t1,
          "the bit set again before any tick, the vDSO's reading of the boot processor's page "
          "50 ns behind reads no earlier than the system call's without the bit");
    CHECK(kernel_mono() >= t1, "and neither does the kernel's");
    kvm_boot_back_ns = 0;
    recovered_ns = t1;
}

/*
 * The vDSOs' own code, vdsocode.s for x86-64 programs and vdso32code.s for
 * i386 ones, assembled into this program where the host is an x86-64
 * Linux, and run there: copied into memory below 4 GiB laid out as a
 * program sees it, KVM's clock page, then the data page, then the image
 * with the code at VDSO_TEXT_OFFSET, and its system calls replaced by
 * `notl %eax`, so that a fallback returns the call's number inverted and
 * makes no call. The 32-bit code runs in the host kernel's 32-bit code
 * segment, entered and left by far returns (`vhost_gate`). Elsewhere, on
 * macOS on Apple's processors and on Windows, the host runs no x86-64
 * Linux code, and only the C reader's rows run.
 */
#if defined(__x86_64__) && defined(__linux__) && defined(__GNUC__)
#define VCLOCK_ABI_ROWS 1

/* The assembler finds the files from the repository's root, where the
 * host tests compile. */
__asm__(".pushsection .text\n"
        ".include \"rach2/sys/arch/x86_64/vdsocode.s\"\n"
        ".include \"rach2/sys/arch/x86_64/vdso32code.s\"\n"
        ".code64\n"
        /* uint64_t vhost_call64(uint64_t fn, uint64_t a, uint64_t b): fn(a, b),
         * a direct call for the sanitizers. */
        ".p2align 4\n"
        ".globl vhost_call64\n"
        ".hidden vhost_call64\n"
        "vhost_call64:\n"
        "    movq %rdi, %rax\n"
        "    movq %rsi, %rdi\n"
        "    movq %rdx, %rsi\n"
        "    jmp *%rax\n"
        /* The gate, copied below 4 GiB: given a block below 4 GiB in %rdi
         * (`struct gate_block`), it saves the stack pointer there, moves to
         * the block's stack, far-returns to the 32-bit code segment 0x23,
         * calls the block's function with its four arguments by the i386
         * convention, keeps EAX and EDX, and far-returns to the 64-bit
         * segment 0x33, where it takes its stack back and returns EAX. */
        ".p2align 4\n"
        ".globl vhost_gate\n"
        ".hidden vhost_gate\n"
        "vhost_gate:\n"
        "    pushq %rbx\n"
        "    pushq %rbp\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    movl %edi, %ebx\n"
        "    movq %rsp, 0(%rbx)\n"
        "    leaq vhost_gate_back(%rip), %rax\n"
        "    movl %eax, 40(%rbx)\n"
        "    movl 8(%rbx), %esp\n"
        "    leaq vhost_gate32(%rip), %rax\n"
        "    pushq $0x23\n"
        "    pushq %rax\n"
        "    lretq\n"
        ".code32\n"
        "vhost_gate32:\n"
        "    movl $0x2b, %eax\n"
        "    movl %eax, %ds\n"
        "    movl %eax, %es\n"
        "    pushl 28(%ebx)\n"
        "    pushl 24(%ebx)\n"
        "    pushl 20(%ebx)\n"
        "    pushl 16(%ebx)\n"
        "    call *12(%ebx)\n"
        "    addl $16, %esp\n"
        "    movl %eax, 32(%ebx)\n"
        "    movl %edx, 36(%ebx)\n"
        "    pushl $0x33\n"
        "    pushl 40(%ebx)\n"
        "    lret\n"
        /* A 32-bit function that returns 0x5a5a, for the probe. */
        ".globl vhost_probe32\n"
        ".hidden vhost_probe32\n"
        "vhost_probe32:\n"
        "    movl $0x5a5a, %eax\n"
        "    ret\n"
        ".code64\n"
        "vhost_gate_back:\n"
        "    movl %ebx, %ebx\n"
        "    movq 0(%rbx), %rsp\n"
        "    movl 32(%rbx), %eax\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbp\n"
        "    popq %rbx\n"
        "    ret\n"
        ".globl vhost_gate_end\n"
        ".hidden vhost_gate_end\n"
        "vhost_gate_end:\n"
        ".popsection\n");

extern const unsigned char vdso_text_start[], vdso_text_end[];
extern const unsigned char vdso_clock_gettime[], vdso_gettimeofday[], vdso_time[];
extern const unsigned char vdso32_text_start[], vdso32_text_end[], vdso32_vsyscall_entry[];
extern const unsigned char vdso32_clock_gettime[], vdso32_clock_gettime64[];
extern const unsigned char vdso32_gettimeofday[], vdso32_time[];
extern const unsigned char vhost_gate[], vhost_gate_end[], vhost_probe32[];
extern uint64_t vhost_call64(uint64_t fn, uint64_t a, uint64_t b);

/* The host's calls, declared here, since a host header would define the
 * kernel's types again; the constants are x86-64 Linux's. */
extern void *mmap(void *addr, unsigned long length, int prot, int flags, int fd, long offset);
extern int fork(void);
extern int waitpid(int pid, int *status, int options);
extern void _exit(int status);
#define HOST_PROT_RWX    7
#define HOST_MAP_PRIVATE 0x02
#define HOST_MAP_ANON    0x20
#define HOST_MAP_32BIT   0x40

/* The memory below 4 GiB, as offsets: each ABI's KVM page, data page and
 * image, whose code is at VDSO_TEXT_OFFSET and may run on past the image's
 * page up to the next layout; then the gate, the block and the 32-bit
 * code's stack. */
#define LOW_ABI64   0x0000u
#define LOW_ABI32   0x4000u
#define LOW_GATE    0x8000u
#define LOW_BLOCK   0x9000u
#define LOW_OUT     0x9100u
#define LOW_STACK   0xC000u
#define LOW_SIZE    0xC000u
#define LAYOUT_CODE (2u * PAGE_SIZE + VDSO_TEXT_OFFSET)
#define LAYOUT_ROOM (LOW_ABI32 - LOW_ABI64 - LAYOUT_CODE)

/* The block the gate reads, at LOW_BLOCK. */
struct gate_block {
    uint64_t saved_rsp; /* the 64-bit stack pointer, kept by the gate */
    uint32_t stack;     /* the top of the 32-bit code's stack */
    uint32_t fn;        /* the 32-bit function */
    uint32_t arg[4];    /* its arguments, the first lowest */
    uint32_t eax, edx;  /* what it returned */
    uint32_t back;      /* where the gate comes back to 64-bit code */
};

/* A timespec and a timeval of each ABI. */
struct ts64 {
    int64_t sec, nsec;
};
struct ts32 {
    int32_t sec, nsec;
};

/* A struct timezone, the same in both ABIs, and the zone the rows have the
 * kernel keep: six hours east of UTC, with a summer time. */
struct tz32 {
    int32_t mw, dst;
};
#define ABI_TZ_MW  (-360)
#define ABI_TZ_DST 1

static unsigned char *low;

/** The address below 4 GiB at offset `off` of the low memory. */
static uint32_t low_at(uint32_t off)
{
    return (uint32_t)(uintptr_t)(low + off);
}

/** Copy `n` bytes from `from` to the low memory at `off`. */
static void low_copy(uint32_t off, const unsigned char *from, unsigned long n)
{
    unsigned long i;

    for (i = 0; i < n; i++) {
        low[off + i] = from[i];
    }
}

/* A KVM clock page's fields, which `abi_pages` lays out. */
struct pvti_fields {
    uint64_t stamp;  /* tsc_timestamp */
    uint64_t system; /* system_time */
    uint32_t mul;    /* tsc_to_system_mul */
    int shift;       /* tsc_shift */
};

/* The data page's base count in the layouts, KVM's clock in nanoseconds,
 * and the rate that makes a nanosecond of it one of each clock's. */
#define ABI_BASE  1000000000000ull
#define ABI_SHIFT 30u

/** The host's TSC, read as the vDSO reads it. */
static uint64_t host_tsc(void)
{
    uint32_t lo, hi;

    __asm__ __volatile__("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

/** KVM's clock that the page `f` converts the TSC's count `tsc` to,
 * Linux's __pvclock_read_cycles, a count before the stamp as the stamp. */
static uint64_t pvti_convert(const struct pvti_fields *f, uint64_t tsc)
{
    uint64_t d = tsc > f->stamp ? tsc - f->stamp : 0u;

    d = f->shift < 0 ? d >> -f->shift : d << f->shift;
    return f->system + (d >> 32) * f->mul + (((d & 0xFFFFFFFFu) * f->mul) >> 32);
}

/** Store the `n` low bytes of `v` at `p`, the lowest first. */
static void put_le(unsigned char *p, uint64_t v, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        p[i] = (unsigned char)(v >> (8 * i));
    }
}

/** The layout at `at`: KVM's page, its version even, its fields `f` and
 * its stable bit `stable`, and the clock's data page as the kernel keeps
 * it. */
static void abi_kvm_page(uint32_t at, int stable, const struct pvti_fields *f)
{
    unsigned int i;

    for (i = 0; i < 32u; i++) {
        low[at + i] = 0;
    }
    low[at] = 2;
    put_le(low + at + 8u, f->stamp, 8);
    put_le(low + at + 16u, f->system, 8);
    put_le(low + at + 24u, f->mul, 4);
    low[at + 28u] = (unsigned char)(signed char)f->shift;
    low[at + 29u] = stable ? 1u : 0u;
    low_copy(at + PAGE_SIZE, (const unsigned char *)data, PAGE_SIZE);
}

/**
 * The layout at `at`: KVM's page as `abi_kvm_page` lays it out, and the
 * clock's data page in VCLOCK_PVCLOCK with its base at KVM's clock
 * ABI_BASE and each clock's rate one nanosecond a count, so that a reading
 * is each clock's base plus KVM's clock's time since ABI_BASE.
 */
static void abi_pages(uint32_t at, int stable, const struct pvti_fields *f)
{
    struct vdso_data *page = (struct vdso_data *)(low + at + PAGE_SIZE);

    abi_kvm_page(at, stable, f);
    page->mode = VCLOCK_PVCLOCK;
    page->cycle_base = ABI_BASE;
    page->shift = ABI_SHIFT;
    page->mult = 1u << ABI_SHIFT;
    page->real_mult = 1u << ABI_SHIFT;
    page->raw_mult = 1u << ABI_SHIFT;
}

/* The pages the rows convert through: one that moves KVM's clock a second
 * past the base whatever the TSC, as KVM_SET_CLOCK may with the stable bit
 * kept, then two that convert the TSC's counts since a stamp 2^33 counts
 * back, shifted left and right, so that both halves of the product count.
 * `abi_rows` sets the stamps. */
static struct pvti_fields abi_fields[3] = {
    {0u, ABI_BASE + 1000000000u, 0u, 0},
    {0u, ABI_BASE, 0x80000000u, 1},
    {0u, ABI_BASE, 0xC0000000u, -2},
};

/** Whether `ns` falls between what the page `f` converts the TSC's counts
 * `t0` and `t1` to, as the clock based at `base_ns` reads them. */
static int abi_between(uint64_t ns, uint64_t base_ns, const struct pvti_fields *f, uint64_t t0,
                       uint64_t t1)
{
    return ns >= base_ns + (pvti_convert(f, t0) - ABI_BASE) &&
           ns <= base_ns + (pvti_convert(f, t1) - ABI_BASE);
}

/**
 * Copy the code from `start` to `end` to the layout at `at` and replace
 * each `movl $N, %eax; syscall` whose N is in `calls` with `notl %eax`:
 * the replacements made, or -1 when the code does not fit.
 */
static int abi_code64(uint32_t at, const unsigned char *start, const unsigned char *end,
                      const uint32_t *calls, int ncalls)
{
    unsigned long n = (unsigned long)(end - start), i;
    unsigned char *code = low + at + LAYOUT_CODE;
    int made = 0, k;

    if (n > LAYOUT_ROOM) {
        return -1;
    }
    low_copy(at + LAYOUT_CODE, start, n);
    for (i = 0; i + 7u <= n; i++) {
        uint32_t imm = (uint32_t)code[i + 1] | (uint32_t)code[i + 2] << 8 |
                       (uint32_t)code[i + 3] << 16 | (uint32_t)code[i + 4] << 24;

        if (code[i] != 0xB8 || code[i + 5] != 0x0F || code[i + 6] != 0x05) {
            continue;
        }
        for (k = 0; k < ncalls; k++) {
            if (imm == calls[k]) {
                code[i + 5] = 0xF7;
                code[i + 6] = 0xD0;
                made++;
            }
        }
    }
    return made;
}

/** The layout at `at` for the recovery: the clock's data page as
 * `recovery_rows` left it, and KVM's page with its stable bit, converting
 * every TSC count to KVM's clock 50 ns behind the page's base. */
static void abi_recovered(uint32_t at)
{
    struct pvti_fields f;

    f.stamp = 0u;
    f.system = data->cycle_base - 50u;
    f.mul = 0u;
    f.shift = 0;
    abi_kvm_page(at, 1, &f);
}

/** Whether the time `sec` and `nsec` is no earlier than `recovered_ns`. */
static int abi_not_before(int64_t sec, int64_t nsec)
{
    return (uint64_t)sec * NSEC_PER_SEC + (uint64_t)nsec >= recovered_ns;
}

/** The address below 4 GiB of the function `fn` of the code from `start`
 * copied to the layout at `at`. */
static uint32_t abi_fn(uint32_t at, const unsigned char *start, const unsigned char *fn)
{
    return low_at(at + LAYOUT_CODE + (uint32_t)(fn - start));
}

/** The 32-bit function at `fn` called through the gate with the four
 * arguments: what it returned in EAX. */
static uint32_t gate_call(uint32_t fn, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    struct gate_block *g = (struct gate_block *)(low + LOW_BLOCK);

    g->stack = low_at(LOW_STACK);
    g->fn = fn;
    g->arg[0] = a;
    g->arg[1] = b;
    g->arg[2] = c;
    g->arg[3] = d;
    return (uint32_t)vhost_call64(low_at(LOW_GATE), low_at(LOW_BLOCK), 0);
}

/** Whether the host runs 32-bit code: the gate tried in a child, which
 * a host without the 32-bit code segment ends with a signal, so that the
 * row fails with its message where the gate would end the test. */
static int gate_works(void)
{
    int pid, status = 0;

    pid = fork();
    if (pid == 0) {
        _exit(gate_call(low_at(LOW_GATE + (uint32_t)(vhost_probe32 - vhost_gate)), 0, 0, 0, 0) ==
                      0x5a5au
                  ? 0
                  : 1);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid) {
        return 0;
    }
    return (status & 0x7F) == 0 && ((status >> 8) & 0xFF) == 0;
}

/**
 * The x86-64 vDSO's clock_gettime of the monotonic, real and raw clocks,
 * gettimeofday and time, run from the code of vdsocode.s over the kernel's
 * data page in VCLOCK_PVCLOCK. With KVM's stable bit and a page whose
 * system time is a second past the base, each reads its base and a second;
 * through the two pages that convert the TSC, the monotonic clock reads
 * what they convert the TSC to before and after the call, or between; and
 * with the bit withdrawn each makes its system call, so that a vdsocode.s
 * that counted the TSC or did not check the bit fails a row. 1 when the
 * code is in its layout, for `abi_tz_rows`.
 */
static int abi64_rows(void)
{
    static const uint32_t calls[] = {96u, 201u, 228u, 229u, 309u};
    uint32_t cgt = abi_fn(LOW_ABI64, vdso_text_start, vdso_clock_gettime);
    uint32_t gtod = abi_fn(LOW_ABI64, vdso_text_start, vdso_gettimeofday);
    uint32_t tm = abi_fn(LOW_ABI64, vdso_text_start, vdso_time);
    uint64_t mono_ns = (uint64_t)data->mono_sec * NSEC_PER_SEC + data->mono_nsec, t0, t1;
    struct ts64 ts, tv;
    int k, ok;

    ok = abi_code64(LOW_ABI64, vdso_text_start, vdso_text_end, calls, 5) == 5;
    CHECK(ok, "the x86-64 vDSO's code fits and its five system calls are found and replaced");
    if (!ok) {
        return 0;
    }
    abi_recovered(LOW_ABI64);
    ok = vhost_call64(cgt, 1, (uint64_t)(uintptr_t)&ts) == 0 && abi_not_before(ts.sec, ts.nsec);
    CHECK(ok, "the bit set again, the x86-64 vDSO's reading of a page behind the base reads no "
              "earlier than the system call's without the bit");
    abi_pages(LOW_ABI64, 1, &abi_fields[0]);
    ok = vhost_call64(cgt, 1, (uint64_t)(uintptr_t)&ts) == 0 && ts.sec == data->mono_sec + 1 &&
         (uint64_t)ts.nsec == data->mono_nsec;
    ok = ok && vhost_call64(cgt, 0, (uint64_t)(uintptr_t)&ts) == 0 &&
         ts.sec == data->real_sec + 1 && (uint64_t)ts.nsec == data->real_nsec;
    ok = ok && vhost_call64(cgt, 4, (uint64_t)(uintptr_t)&ts) == 0 && ts.sec == data->raw_sec + 1 &&
         (uint64_t)ts.nsec == data->raw_nsec;
    ok = ok && vhost_call64(gtod, (uint64_t)(uintptr_t)&tv, 0) == 0 &&
         tv.sec == data->real_sec + 1 && (uint64_t)tv.nsec == data->real_nsec / 1000u;
    ok = ok && (int64_t)vhost_call64(tm, 0, 0) == data->real_sec + 1;
    CHECK(ok, "with KVM's stable bit the x86-64 vDSO reads each clock by KVM's system time, "
              "a second past the base with the TSC counting nothing");
    for (k = 1; k < 3; k++) {
        abi_pages(LOW_ABI64, 1, &abi_fields[k]);
        t0 = host_tsc();
        ok = vhost_call64(cgt, 1, (uint64_t)(uintptr_t)&ts) == 0;
        t1 = host_tsc();
        ok = ok && abi_between((uint64_t)ts.sec * NSEC_PER_SEC + (uint64_t)ts.nsec, mono_ns,
                               &abi_fields[k], t0, t1);
        CHECK(ok, k == 1 ? "the x86-64 vDSO converts the TSC through KVM's page, shifted left"
                         : "and shifted right");
    }
    abi_pages(LOW_ABI64, 0, &abi_fields[0]);
    ok = (uint32_t)vhost_call64(cgt, 1, (uint64_t)(uintptr_t)&ts) == ~228u;
    ok = ok && (uint32_t)vhost_call64(cgt, 0, (uint64_t)(uintptr_t)&ts) == ~228u;
    ok = ok && (uint32_t)vhost_call64(cgt, 4, (uint64_t)(uintptr_t)&ts) == ~228u;
    ok = ok && (uint32_t)vhost_call64(gtod, (uint64_t)(uintptr_t)&tv, 0) == ~96u;
    ok = ok && (uint32_t)vhost_call64(tm, 0, 0) == ~201u;
    CHECK(ok, "with the bit withdrawn the x86-64 vDSO makes the system call for each");
    return 1;
}

/**
 * The 32-bit vDSO's clock_gettime, clock_gettime64, gettimeofday and time
 * of an i386 program, run from the code of vdso32code.s through the gate,
 * as abi64_rows runs the x86-64 code; its system calls go through
 * __kernel_vsyscall, whose `int $0x80` is the one replaced.
 */
static int abi32_rows(void)
{
    static const unsigned char entry[] = {0x90, 0x90, 0x90, 0x90, 0xCD, 0x80};
    unsigned long n = (unsigned long)(vdso32_text_end - vdso32_text_start), i;
    unsigned char *call =
        low + LOW_ABI32 + LAYOUT_CODE + (vdso32_vsyscall_entry - vdso32_text_start);
    uint32_t cgt = abi_fn(LOW_ABI32, vdso32_text_start, vdso32_clock_gettime);
    uint32_t cgt64 = abi_fn(LOW_ABI32, vdso32_text_start, vdso32_clock_gettime64);
    uint32_t gtod = abi_fn(LOW_ABI32, vdso32_text_start, vdso32_gettimeofday);
    uint32_t tm = abi_fn(LOW_ABI32, vdso32_text_start, vdso32_time);
    struct ts32 *t32 = (struct ts32 *)(low + LOW_OUT);
    struct ts64 *t64 = (struct ts64 *)(low + LOW_OUT);
    uint32_t out = low_at(LOW_OUT);
    uint64_t mono_ns = (uint64_t)data->mono_sec * NSEC_PER_SEC + data->mono_nsec, t0, t1;
    int k, ok, same = 1;

    CHECK(n <= LAYOUT_ROOM, "the 32-bit vDSO's code fits its layout");
    if (n > LAYOUT_ROOM) {
        return 0;
    }
    low_copy(LOW_ABI32 + LAYOUT_CODE, vdso32_text_start, n);
    for (i = 0; i < sizeof entry; i++) {
        same = same && call[i] == entry[i];
    }
    CHECK(same, "__kernel_vsyscall's entry is four nops and int $0x80");
    if (!same) {
        return 0;
    }
    call[4] = 0xF7;
    call[5] = 0xD0;
    abi_recovered(LOW_ABI32);
    ok = gate_call(cgt64, 1, out, 0, 0) == 0 && abi_not_before(t64->sec, t64->nsec);
    CHECK(ok, "the bit set again, the 32-bit vDSO's reading of a page behind the base reads no "
              "earlier than the system call's without the bit");
    abi_pages(LOW_ABI32, 1, &abi_fields[0]);
    ok = gate_call(cgt, 1, out, 0, 0) == 0 && t32->sec == (int32_t)data->mono_sec + 1 &&
         (uint64_t)t32->nsec == data->mono_nsec;
    ok = ok && gate_call(cgt64, 4, out, 0, 0) == 0 && t64->sec == data->raw_sec + 1 &&
         (uint64_t)t64->nsec == data->raw_nsec;
    ok = ok && gate_call(gtod, out, 0, 0, 0) == 0 && t32->sec == (int32_t)data->real_sec + 1 &&
         (uint64_t)t32->nsec == data->real_nsec / 1000u;
    ok = ok && gate_call(tm, 0, 0, 0, 0) == (uint32_t)data->real_sec + 1u;
    CHECK(ok, "with KVM's stable bit the 32-bit vDSO reads each clock by KVM's system time, "
              "a second past the base with the TSC counting nothing");
    for (k = 1; k < 3; k++) {
        abi_pages(LOW_ABI32, 1, &abi_fields[k]);
        t0 = host_tsc();
        ok = gate_call(cgt64, 1, out, 0, 0) == 0;
        t1 = host_tsc();
        ok = ok && abi_between((uint64_t)t64->sec * NSEC_PER_SEC + (uint64_t)t64->nsec, mono_ns,
                               &abi_fields[k], t0, t1);
        CHECK(ok, k == 1 ? "the 32-bit vDSO converts the TSC through KVM's page, shifted left"
                         : "and shifted right");
    }
    abi_pages(LOW_ABI32, 0, &abi_fields[0]);
    ok = gate_call(cgt, 1, out, 0, 0) == ~265u;
    ok = ok && gate_call(cgt64, 4, out, 0, 0) == ~403u;
    ok = ok && gate_call(gtod, out, 0, 0, 0) == ~78u;
    ok = ok && gate_call(tm, 0, 0, 0, 0) == ~13u;
    CHECK(ok, "with the bit withdrawn the 32-bit vDSO makes the system call for each");
    return 1;
}

/** gettimeofday of the x86-64 vDSO at `g64`, or for `abi32` the 32-bit
 * one at `g32`, on `tv` and `tz`, addresses below 4 GiB or 0: what it
 * returned, in 32 bits. */
static uint32_t abi_gtod(int abi32, uint32_t g64, uint32_t g32, uint64_t tv, uint64_t tz)
{
    return abi32 ? gate_call(g32, (uint32_t)tv, (uint32_t)tz, 0, 0)
                 : (uint32_t)vhost_call64(g64, tv, tz);
}

/**
 * Both vDSOs' gettimeofday with a time zone, from the code `abi64_rows` and
 * `abi32_rows` put in their layouts, `abi64` and `abi32` where each did: on
 * a page with KVM's stable bit it gives the zone the data page holds with
 * the time and for a null tv, with no system call; with the bit withdrawn,
 * and in VCLOCK_NONE, it leaves a time and its zone to the system call and
 * gives a zone alone itself, so that a reading that kept tz on the stack
 * and did not pop it before the call returns nowhere.
 */
static void abi_tz_rows(int abi64, int abi32)
{
    uint32_t g64 = abi_fn(LOW_ABI64, vdso_text_start, vdso_gettimeofday);
    uint32_t g32 = abi_fn(LOW_ABI32, vdso32_text_start, vdso32_gettimeofday);
    uint64_t tv = (uint64_t)(uintptr_t)(low + LOW_OUT), tz = tv + 64u;
    uint32_t at[2] = {LOW_ABI64, LOW_ABI32};
    struct tz32 *z = (struct tz32 *)(low + LOW_OUT + 64u);
    int k, mode, ok;

    for (k = 0; k < 2; k++) {
        if (!(k ? abi32 : abi64)) {
            continue;
        }
        abi_pages(at[k], 1, &abi_fields[0]);
        z->mw = z->dst = 0;
        ok = abi_gtod(k, g64, g32, tv, tz) == 0 && z->mw == ABI_TZ_MW && z->dst == ABI_TZ_DST;
        z->mw = z->dst = 0;
        ok = ok && abi_gtod(k, g64, g32, 0, tz) == 0 && z->mw == ABI_TZ_MW && z->dst == ABI_TZ_DST;
        CHECK(ok, k ? "the 32-bit vDSO's gettimeofday gives the data page's time zone, with the "
                      "time and for a null tv, making no system call"
                    : "the x86-64 vDSO's gettimeofday gives the data page's time zone, with the "
                      "time and for a null tv, making no system call");
        abi_pages(at[k], 0, &abi_fields[0]);
        for (mode = 0; mode < 2; mode++) {
            ((struct vdso_data *)(low + at[k] + PAGE_SIZE))->mode =
                mode ? VCLOCK_NONE : VCLOCK_PVCLOCK;
            z->mw = z->dst = 0;
            ok = abi_gtod(k, g64, g32, tv, tz) == ~(k ? 78u : 96u) && z->mw == 0;
            ok = ok && abi_gtod(k, g64, g32, 0, tz) == 0 && z->mw == ABI_TZ_MW &&
                 z->dst == ABI_TZ_DST;
            CHECK(ok, mode ? "in VCLOCK_NONE it leaves a time and its zone to the system call, "
                             "and gives a zone alone itself"
                           : "with the bit withdrawn it leaves a time and its zone to the "
                             "system call, and gives a zone alone itself");
        }
    }
}

/** The rows of both vDSOs' code, on the clock as recovery_rows leaves it,
 * running in VCLOCK_PVCLOCK. */
static void abi_rows(void)
{
    uint64_t stamp;
    int abi64, abi32;
    void *m =
        mmap(0, LOW_SIZE, HOST_PROT_RWX, HOST_MAP_PRIVATE | HOST_MAP_ANON | HOST_MAP_32BIT, -1, 0);

    CHECK(m != (void *)-1 && (uintptr_t)m + LOW_SIZE <= 0xFFFFFFFFu,
          "memory below 4 GiB for the vDSOs' code");
    if (m == (void *)-1) {
        return;
    }
    low = m;
    CHECK(data->mode == VCLOCK_PVCLOCK, "the vDSOs read a clock in VCLOCK_PVCLOCK");
    stamp = host_tsc();
    stamp = stamp > (1ull << 34) ? stamp - (1ull << 33) : 0u;
    abi_fields[1].stamp = stamp;
    abi_fields[2].stamp = stamp;
    low_copy(LOW_GATE, vhost_gate, (unsigned long)(vhost_gate_end - vhost_gate));
    vclock_set_tz(ABI_TZ_MW, ABI_TZ_DST);
    CHECK(data->tz_minuteswest == ABI_TZ_MW && data->tz_dsttime == ABI_TZ_DST,
          "settimeofday's time zone reaches the data page");
    abi64 = abi64_rows();
    if (!gate_works()) {
        CHECK(0, "the host runs 32-bit code, for the 32-bit vDSO's rows");
        abi_tz_rows(abi64, 0);
        return;
    }
    abi32 = abi32_rows();
    abi_tz_rows(abi64, abi32);
}
#endif

/**
 * A second boot, the clock's state cleared, whose start finds the boot
 * processor's page without its stable bit: the clock does not start, and
 * the tick leaves it stopped while the bit stays withdrawn; the bit set
 * again starts it at the next tick, from the tick's clock, where a refusal
 * the tick does not try again would leave the system call to every
 * reading until the next boot.
 */
static void boot_rows(void)
{
    int64_t sec, nsec;

    running = 0;
    kvm_stopped = 0;
    data = 0;
    floor_mono = 0;
    floor_raw = 0;
    tick_lag_ns = 0;
    tsc_ahead = 0;
    tsc_held = 0;
    kvm_back_ns = 0;
    kvm_here_withdrawn = 0;
    now_ns = 7000000000ULL;
    kvm_boot_withdrawn = 1;
    vclock_set_tz(300, 0);
    vclock_start();
    CHECK(!vclock_running() && data && data->mode == VCLOCK_NONE,
          "a start at boot that finds the boot processor's bit withdrawn leaves the clock "
          "stopped");
    CHECK(data && data->tz_minuteswest == 300 && data->tz_dsttime == 0,
          "a time zone set before the data page is built is on the page");
    now_ns += TICK_NS;
    vclock_tick();
    CHECK(!vclock_running(), "and the tick leaves it stopped while the bit stays withdrawn");
    kvm_boot_withdrawn = 0;
    now_ns += TICK_NS;
    vclock_tick();
    CHECK(vclock_running() && data->mode == VCLOCK_PVCLOCK,
          "the bit set again starts the clock at the next tick");
    CHECK(vclock_read(VCLOCK_READ_MONO, 0, &sec, &nsec) && sec == 7 &&
              nsec == (int64_t)(2u * TICK_NS),
          "and it reads the tick's clock");
}

/** Run the rows: 0 when every check passes, 1 otherwise. */
int main(void)
{
    page_rows();
    stop_rows();
    recovery_rows();
#ifdef VCLOCK_ABI_ROWS
    abi_rows();
#endif
    boot_rows();
    return fails != 0;
}
