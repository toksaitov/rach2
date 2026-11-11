/*
 * Count a guest's windows of retired instructions on one of its counters,
 * placed on physical fixed counter 0 or on a general counter, under each
 * value of kvm_intel's pmu_steps, to find which of Linux's steps keeps
 * Haswell's fixed counter 0 exact (81-9.28). Built on the host:
 * gcc -O1 -o pmcwin pmcwin.c
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/kvm.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define STEPS_FILE  "/sys/module/kvm_intel/parameters/pmu_steps"
#define PLACED_FILE "/sys/module/kvm_intel/parameters/pmu_placed"
#define STEP_ORDER  0x10u /* pmu_steps' Linux order, fixed counters first */
#define MEM_SIZE   0x400000u /* the guest's memory, from address 0 */
#define CODE_GPA   0x1000u
#define OUT_GPA    0x20000u /* a 32-bit delta for each window */
#define MAX_WIN    200000u

#define PMC0        0x0C1u
#define EVENTSEL0   0x186u
#define FIXED0      0x309u
#define FIXED_CTRL  0x38Du
#define GLOBAL_CTRL 0x38Fu
#define EV_ENABLE   (1u << 22)
#define EV_OS       (1u << 17)
#define EV_USR      (1u << 16)

/*
 * The guest, 32-bit protected mode with no paging. ESI is the RDPMC index,
 * EDI the CPUIDs, each an exit, inside each window, EBP the windows; each
 * window's delta goes to OUT_GPA on. The two RDPMCs exit too, so a window
 * has two exits and EDI more. HLT ends the run.
 *
 * A window retires the first RDPMC, which reads the counter before it
 * retires, the four instructions from its MOV to the JZ, the loop's four
 * instructions, XOR, CPUID, DECL and JNZ, EDI times, the 20 NOPs and the
 * MOV to ECX, and not the second RDPMC, which reads before it retires:
 * 26 + 4 * EDI instructions (`EXACT`). KVM counts the RDPMCs and CPUIDs
 * it emulates as Linux's does.
 */
#define EXACT(exits) (26u + 4u * (exits))
extern const unsigned char guest_start[], guest_end[];
__asm__(".pushsection .rodata\n"
        ".code32\n"
        "guest_start:\n"
        "    mov $0x20000, %esp\n"
        "1:  mov %esi, %ecx\n"
        "    rdpmc\n"
        "    mov %eax, (%esp)\n"
        "    mov %edi, 0xF00\n"
        "    test %edi, %edi\n"
        "    jz 3f\n"
        "2:  xor %eax, %eax\n"
        "    cpuid\n"
        "    decl 0xF00\n"
        "    jnz 2b\n"
        "3:  .rept 20\n"
        "    nop\n"
        "    .endr\n"
        "    mov %esi, %ecx\n"
        "    rdpmc\n"
        "    sub (%esp), %eax\n"
        "    mov %eax, (%esp)\n"
        "    add $4, %esp\n"
        "    dec %ebp\n"
        "    jnz 1b\n"
        "    hlt\n"
        "guest_end:\n"
        ".code64\n"
        ".popsection\n");

/* A case: which counter is read, what else counts beside it, and where
 * its counters run. With pmu_steps' 0x10 KVM places a guest counter of
 * retired instructions on physical fixed counter 0 first, and without it
 * on a general counter first, on Haswell; pmu_placed, read after the run,
 * shows where they ran. */
struct probe_case {
    const char *name;
    uint32_t index;  /* RDPMC's ECX */
    uint64_t global; /* the guest's GLOBAL_CTRL */
    uint64_t fixed;  /* the guest's FIXED_CTRL */
    uint64_t sel[4]; /* the guest's EVENTSEL0 to 3 */
    int on_fixed;    /* 1 on physical fixed counter 0, with 0x10; 0 on a
                        general counter, without it */
    uint64_t others; /* the general counters the others take, or 0 */
};

#define FIXED0_BIT (1ull << 32) /* fixed counter 0 in pmu_placed */
#define INSNS      (EV_ENABLE | EV_OS | EV_USR | 0xC0)
#define BRANCHES   (EV_ENABLE | EV_OS | EV_USR | 0xC4)

static const struct probe_case cases[] = {
    /* The guest's fixed counter 0 alone, on each kind of counter. */
    {"fixed0 on fixed", 0x40000000u, 1ull << 32, 0x3, {0, 0, 0, 0}, 1, 0},
    {"fixed0 on general", 0x40000000u, 1ull << 32, 0x3, {0, 0, 0, 0}, 0, 0},
    /* The guest's general counter 0 counting instructions retired, 0xC0,
     * alone, on each kind. */
    {"gp0 on fixed", 0, 1, 0, {INSNS, 0, 0, 0}, 1, 0},
    {"gp0 on general", 0, 1, 0, {INSNS, 0, 0, 0}, 0, 0},
    /* The guest's fixed counter 0 with the four general counters counting
     * branches, 0xC4, which take the four general counters. */
    {"mixed on fixed",
     0x40000000u,
     (1ull << 32) | 0xF,
     0x3,
     {BRANCHES, BRANCHES, BRANCHES, BRANCHES},
     1,
     0xF},
};

static int kvm = -1;
static int steps_written;

/** Write `value` to pmu_steps; 0 or -1 with errno. */
static int write_steps(unsigned value)
{
    char text[16];
    int fd, n, r;

    fd = open(STEPS_FILE, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    n = snprintf(text, sizeof(text), "0x%x\n", value);
    r = write(fd, text, (size_t)n) == n ? 0 : -1;
    close(fd);
    if (r == 0) {
        steps_written = 1;
    }
    return r;
}

/** The physical counters pmu_placed gives, or ~0 when it cannot be read. */
static uint64_t read_placed(void)
{
    char text[32];
    FILE *f = fopen(PLACED_FILE, "r");
    uint64_t placed = ~0ull;

    if (f) {
        if (fgets(text, sizeof(text), f)) {
            placed = strtoull(text, NULL, 16);
        }
        fclose(f);
    }
    return placed;
}

/** Whether `placed` is where case `pc` asks its counters to run: fixed
 * counter 0 beside the others' general counters, or one general counter
 * alone. */
static int placed_as_asked(const struct probe_case *pc, uint64_t placed)
{
    uint64_t general = placed & 0xFFFFFFFFull;

    if (pc->on_fixed) {
        return placed == (FIXED0_BIT | pc->others);
    }
    return placed == general && general && !(general & (general - 1u));
}

/** Put pmu_steps back to 0 on every exit, so that no step outlives us. */
static void restore_steps(void)
{
    if (steps_written) {
        (void)write_steps(0);
    }
}

static void on_signal(int sig)
{
    restore_steps();
    signal(sig, SIG_DFL);
    raise(sig);
}

static void die(const char *what)
{
    fprintf(stderr, "pmcwin: %s: %s\n", what, strerror(errno));
    exit(1);
}

/** Set or read `n` MSRs of `fd`; the call's return. */
static int msrs(int fd, unsigned long req, const uint32_t *index, uint64_t *value, unsigned n)
{
    struct {
        struct kvm_msrs head;
        struct kvm_msr_entry e[16];
    } m;
    unsigned i;
    int r;

    memset(&m, 0, sizeof(m));
    m.head.nmsrs = n;
    for (i = 0; i < n; i++) {
        m.e[i].index = index[i];
        m.e[i].data = value[i];
    }
    r = ioctl(fd, req, &m);
    for (i = 0; i < n; i++) {
        value[i] = m.e[i].data;
    }
    return r;
}

/** Give the vCPU the CPUID KVM supports, its PMU included. */
static void set_cpuid(int vcpu)
{
    struct kvm_cpuid2 *c;
    unsigned n = 256;

    c = calloc(1, sizeof(*c) + n * sizeof(c->entries[0]));
    if (!c) {
        die("calloc");
    }
    c->nent = n;
    if (ioctl(kvm, KVM_GET_SUPPORTED_CPUID, c) < 0) {
        die("KVM_GET_SUPPORTED_CPUID");
    }
    if (ioctl(vcpu, KVM_SET_CPUID2, c) < 0) {
        die("KVM_SET_CPUID2");
    }
    free(c);
}

/** A flat 4 GiB segment: code when `code`, else read/write data. */
static void flat(struct kvm_segment *s, int code, uint16_t selector)
{
    memset(s, 0, sizeof(*s));
    s->base = 0;
    s->limit = 0xFFFFFFFFu;
    s->selector = selector;
    s->type = code ? 0xB : 0x3;
    s->present = 1;
    s->s = 1;
    s->db = 1;
    s->g = 1;
}

/**
 * Run one case of `windows` windows with `exits` CPUIDs in each, under the
 * knob value `steps`, which the caller has written with the case's 0x10,
 * and print how many deltas differ from the exact count, EXACT(exits), and
 * the most common delta apart. 0, or -1 when the run did not end with the
 * guest's HLT or did not run where the case asks.
 */
static int run_case(const struct probe_case *pc, unsigned steps, unsigned windows, unsigned exits)
{
    static const uint32_t setup[] = {GLOBAL_CTRL, FIXED_CTRL, EVENTSEL0,     EVENTSEL0 + 1,
                                     EVENTSEL0 + 2, EVENTSEL0 + 3, PMC0,     PMC0 + 1,
                                     PMC0 + 2,    PMC0 + 3,    FIXED0};
    uint64_t value[11], back[11];
    struct kvm_userspace_memory_region region;
    struct kvm_sregs sregs;
    struct kvm_regs regs;
    struct kvm_run *run;
    uint32_t *delta, mode = 0, d, exact = EXACT(exits);
    uint64_t placed;
    unsigned i, j, best = 0, bad = 0, over = 0, under = 0, entries = 0, n;
    uint32_t max_over = 0, max_under = 0;
    unsigned char *mem;
    long run_size;
    int vm, vcpu, r = -1;

    vm = ioctl(kvm, KVM_CREATE_VM, 0);
    if (vm < 0) {
        die("KVM_CREATE_VM");
    }
    mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        die("mmap");
    }
    memcpy(mem + CODE_GPA, guest_start, (size_t)(guest_end - guest_start));
    memset(&region, 0, sizeof(region));
    region.memory_size = MEM_SIZE;
    region.userspace_addr = (uintptr_t)mem;
    if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        die("KVM_SET_USER_MEMORY_REGION");
    }
    vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
    if (vcpu < 0) {
        die("KVM_CREATE_VCPU");
    }
    run_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    run = mmap(NULL, (size_t)run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
    if (run == MAP_FAILED) {
        die("mmap vcpu");
    }
    set_cpuid(vcpu);
    if (ioctl(vcpu, KVM_GET_SREGS, &sregs) < 0) {
        die("KVM_GET_SREGS");
    }
    flat(&sregs.cs, 1, 0x8);
    flat(&sregs.ds, 0, 0x10);
    sregs.es = sregs.fs = sregs.gs = sregs.ss = sregs.ds;
    sregs.cr0 = 0x11; /* PE and ET, no paging */
    sregs.cr4 = 0;
    sregs.efer = 0;
    if (ioctl(vcpu, KVM_SET_SREGS, &sregs) < 0) {
        die("KVM_SET_SREGS");
    }
    memset(&regs, 0, sizeof(regs));
    regs.rip = CODE_GPA;
    regs.rflags = 2;
    regs.rsi = pc->index;
    regs.rdi = exits;
    regs.rbp = windows;
    if (ioctl(vcpu, KVM_SET_REGS, &regs) < 0) {
        die("KVM_SET_REGS");
    }
    value[0] = pc->global;
    value[1] = pc->fixed;
    for (i = 0; i < 4; i++) {
        value[2 + i] = pc->sel[i];
    }
    for (i = 6; i < 11; i++) {
        value[i] = 0;
    }
    if (msrs(vcpu, KVM_SET_MSRS, setup, value, 11) != 11) {
        fprintf(stderr, "pmcwin: KVM_SET_MSRS took fewer than 11 MSRs\n");
        exit(1);
    }
    memset(back, 0, sizeof(back));
    if (msrs(vcpu, KVM_GET_MSRS, setup, back, 6) != 6 || memcmp(value, back, 6 * sizeof(back[0]))) {
        fprintf(stderr, "pmcwin: the guest's PMU reads back otherwise\n");
        exit(1);
    }
    for (;;) {
        if (ioctl(vcpu, KVM_RUN, 0) < 0) {
            if (errno == EINTR) {
                continue;
            }
            die("KVM_RUN");
        }
        entries++;
        if (run->exit_reason == KVM_EXIT_HLT) {
            r = 0;
            break;
        }
        fprintf(stderr, "pmcwin: exit %u at entry %u\n", run->exit_reason, entries);
        break;
    }
    placed = read_placed();
    if (r == 0 && !placed_as_asked(pc, placed)) {
        fprintf(stderr, "pmcwin: steps 0x%02x %s exits %u: ran on 0x%llx, not as asked\n", steps,
                pc->name, exits, (unsigned long long)placed);
        r = -1;
    }
    if (r == 0) {
        delta = (uint32_t *)(mem + OUT_GPA);
        /* The most common delta, reported beside the exact count: alike
         * windows show repeatability, not that the count is right. */
        for (i = 0; i < windows && i < 512; i++) {
            for (n = 0, j = 0; j < windows; j++) {
                n += delta[j] == delta[i];
            }
            if (n > best) {
                best = n;
                mode = delta[i];
            }
        }
        for (i = 0; i < windows; i++) {
            d = delta[i];
            if (d == exact) {
                continue;
            }
            bad++;
            if (d > exact) {
                over++;
                max_over = d - exact > max_over ? d - exact : max_over;
            } else {
                under++;
                max_under = exact - d > max_under ? exact - d : max_under;
            }
        }
        printf("pmcwin: steps 0x%02x %s exits %u: %u of %u windows bad, %u over by at most %u, "
               "%u under by at most %u; exact count %u; most common %u in %u; ran on 0x%llx\n",
               steps, pc->name, exits, bad, windows, over, max_over, under, max_under, exact, mode,
               best, (unsigned long long)placed);
        fflush(stdout);
    }
    munmap(run, (size_t)run_size);
    close(vcpu);
    munmap(mem, MEM_SIZE);
    close(vm);
    return r;
}

int main(int argc, char **argv)
{
    static const unsigned exit_counts[] = {0, 10};
    unsigned windows = 30000, steps, case_steps, i, k;
    int a = 1, failed = 0;
    char *end;

    if (a + 1 < argc && !strcmp(argv[a], "-w")) {
        windows = (unsigned)strtoul(argv[a + 1], &end, 0);
        if (*end || !windows || windows > MAX_WIN) {
            fprintf(stderr, "pmcwin: bad window count: %s\n", argv[a + 1]);
            return 2;
        }
        a += 2;
    }
    if (a == argc) {
        fprintf(stderr, "usage: pmcwin [-w windows] steps...\n");
        return 2;
    }
    kvm = open("/dev/kvm", O_RDWR);
    if (kvm < 0) {
        die("/dev/kvm");
    }
    atexit(restore_steps);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    for (; a < argc; a++) {
        steps = (unsigned)strtoul(argv[a], &end, 0);
        if (*end || (steps & STEP_ORDER)) {
            fprintf(stderr, "pmcwin: bad steps: %s: 0x10 is the case's own\n", argv[a]);
            return 2;
        }
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            case_steps = cases[i].on_fixed ? steps | STEP_ORDER : steps;
            if (write_steps(case_steps) < 0) {
                fprintf(stderr, "pmcwin: steps 0x%02x: %s: %s\n", case_steps, STEPS_FILE,
                        strerror(errno));
                failed = 1;
                continue;
            }
            for (k = 0; k < 2; k++) {
                if (run_case(&cases[i], case_steps, windows, exit_counts[k]) < 0) {
                    failed = 1;
                }
            }
        }
    }
    return failed;
}
