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
 * the stop carry the bases by KVM's clock, not by that TSC; that the clock
 * started again while the tick's clock is still behind the stop's time
 * begins at the floors (`stop_rows`); and that the code of both vDSOs, the
 * x86-64 one and the 32-bit one, reads the page while KVM's page has its
 * stable bit and makes the system call once it is withdrawn (`abi_rows`).
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

/**
 * The layout at `at`: KVM's page, its version even and its stable bit
 * `stable`, and the clock's data page as the kernel keeps it, with a base
 * count no TSC reaches, so that a reading of the TSC reads the base's time
 * exactly.
 */
static void abi_pages(uint32_t at, int stable)
{
    struct vdso_data *page = (struct vdso_data *)(low + at + PAGE_SIZE);
    unsigned int i;

    for (i = 0; i < 32u; i++) {
        low[at + i] = 0;
    }
    low[at] = 2;
    low[at + 29u] = stable ? 1u : 0u;
    low_copy(at + PAGE_SIZE, (const unsigned char *)data, PAGE_SIZE);
    page->cycle_base = ~(uint64_t)0;
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
 * data page in VCLOCK_PVCLOCK: with KVM's stable bit each reads the page's
 * base, and with it withdrawn each makes its system call, so that a
 * vdsocode.s without the check of the bit fails the second set.
 */
static void abi64_rows(void)
{
    static const uint32_t calls[] = {96u, 201u, 228u, 229u, 309u};
    uint32_t cgt = abi_fn(LOW_ABI64, vdso_text_start, vdso_clock_gettime);
    uint32_t gtod = abi_fn(LOW_ABI64, vdso_text_start, vdso_gettimeofday);
    uint32_t tm = abi_fn(LOW_ABI64, vdso_text_start, vdso_time);
    struct ts64 ts, tv;
    int stable, ok;

    ok = abi_code64(LOW_ABI64, vdso_text_start, vdso_text_end, calls, 5) == 5;
    CHECK(ok, "the x86-64 vDSO's code fits and its five system calls are found and replaced");
    if (!ok) {
        return;
    }
    for (stable = 1; stable >= 0; stable--) {
        abi_pages(LOW_ABI64, stable);
        if (stable) {
            ok = vhost_call64(cgt, 1, (uint64_t)(uintptr_t)&ts) == 0 && ts.sec == data->mono_sec &&
                 (uint64_t)ts.nsec == data->mono_nsec;
            ok = ok && vhost_call64(cgt, 0, (uint64_t)(uintptr_t)&ts) == 0 &&
                 ts.sec == data->real_sec && (uint64_t)ts.nsec == data->real_nsec;
            ok = ok && vhost_call64(cgt, 4, (uint64_t)(uintptr_t)&ts) == 0 &&
                 ts.sec == data->raw_sec && (uint64_t)ts.nsec == data->raw_nsec;
            ok = ok && vhost_call64(gtod, (uint64_t)(uintptr_t)&tv, 0) == 0 &&
                 tv.sec == data->real_sec && (uint64_t)tv.nsec == data->real_nsec / 1000u;
            ok = ok && (int64_t)vhost_call64(tm, 0, 0) == data->real_sec;
            CHECK(ok, "with KVM's stable bit the x86-64 vDSO reads each clock from the page");
        } else {
            ok = (uint32_t)vhost_call64(cgt, 1, (uint64_t)(uintptr_t)&ts) == ~228u;
            ok = ok && (uint32_t)vhost_call64(cgt, 0, (uint64_t)(uintptr_t)&ts) == ~228u;
            ok = ok && (uint32_t)vhost_call64(cgt, 4, (uint64_t)(uintptr_t)&ts) == ~228u;
            ok = ok && (uint32_t)vhost_call64(gtod, (uint64_t)(uintptr_t)&tv, 0) == ~96u;
            ok = ok && (uint32_t)vhost_call64(tm, 0, 0) == ~201u;
            CHECK(ok, "with the bit withdrawn the x86-64 vDSO makes the system call for each");
        }
    }
}

/**
 * The 32-bit vDSO's clock_gettime, clock_gettime64, gettimeofday and time
 * of an i386 program, run from the code of vdso32code.s through the gate,
 * as abi64_rows runs the x86-64 code; its system calls go through
 * __kernel_vsyscall, whose `int $0x80` is the one replaced.
 */
static void abi32_rows(void)
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
    int stable, ok, same = 1;

    CHECK(n <= LAYOUT_ROOM, "the 32-bit vDSO's code fits its layout");
    if (n > LAYOUT_ROOM) {
        return;
    }
    low_copy(LOW_ABI32 + LAYOUT_CODE, vdso32_text_start, n);
    for (i = 0; i < sizeof entry; i++) {
        same = same && call[i] == entry[i];
    }
    CHECK(same, "__kernel_vsyscall's entry is four nops and int $0x80");
    if (!same) {
        return;
    }
    call[4] = 0xF7;
    call[5] = 0xD0;
    for (stable = 1; stable >= 0; stable--) {
        abi_pages(LOW_ABI32, stable);
        if (stable) {
            ok = gate_call(cgt, 1, out, 0, 0) == 0 && t32->sec == (int32_t)data->mono_sec &&
                 (uint64_t)t32->nsec == data->mono_nsec;
            ok = ok && gate_call(cgt64, 4, out, 0, 0) == 0 && t64->sec == data->raw_sec &&
                 (uint64_t)t64->nsec == data->raw_nsec;
            ok = ok && gate_call(gtod, out, 0, 0, 0) == 0 && t32->sec == (int32_t)data->real_sec &&
                 (uint64_t)t32->nsec == data->real_nsec / 1000u;
            ok = ok && gate_call(tm, 0, 0, 0, 0) == (uint32_t)data->real_sec;
            CHECK(ok, "with KVM's stable bit the 32-bit vDSO reads each clock from the page");
        } else {
            ok = gate_call(cgt, 1, out, 0, 0) == ~265u;
            ok = ok && gate_call(cgt64, 4, out, 0, 0) == ~403u;
            ok = ok && gate_call(gtod, out, 0, 0, 0) == ~78u;
            ok = ok && gate_call(tm, 0, 0, 0, 0) == ~13u;
            CHECK(ok, "with the bit withdrawn the 32-bit vDSO makes the system call for each");
        }
    }
}

/** The rows of both vDSOs' code, on the clock as stop_rows leaves it,
 * running in VCLOCK_PVCLOCK. */
static void abi_rows(void)
{
    void *m =
        mmap(0, LOW_SIZE, HOST_PROT_RWX, HOST_MAP_PRIVATE | HOST_MAP_ANON | HOST_MAP_32BIT, -1, 0);

    CHECK(m != (void *)-1 && (uintptr_t)m + LOW_SIZE <= 0xFFFFFFFFu,
          "memory below 4 GiB for the vDSOs' code");
    if (m == (void *)-1) {
        return;
    }
    low = m;
    CHECK(data->mode == VCLOCK_PVCLOCK, "the vDSOs read a clock in VCLOCK_PVCLOCK");
    low_copy(LOW_GATE, vhost_gate, (unsigned long)(vhost_gate_end - vhost_gate));
    abi64_rows();
    if (!gate_works()) {
        CHECK(0, "the host runs 32-bit code, for the 32-bit vDSO's rows");
        return;
    }
    abi32_rows();
}
#endif

/** Run the rows: 0 when every check passes, 1 otherwise. */
int main(void)
{
    page_rows();
    stop_rows();
#ifdef VCLOCK_ABI_ROWS
    abi_rows();
#endif
    return fails != 0;
}
