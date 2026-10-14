/**
 * Check x86-64's thread pointer and arch_prctl, `arch/x86_64/tls.c`, over a
 * model of the three base registers, compiled with x86-64's `cpu.h`.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -DRACH2_HOST_X86_64
 *
 * A base at or above USER_ADDR_MAX, the end of user space, is refused with
 * EPERM by ARCH_SET_FS, ARCH_SET_GS and clone's CLONE_SETTLS, and changes
 * neither the process record nor a register; the last user address below
 * it, and 0, are taken. ARCH_SET_FS writes the record's `tls_base` and FS's
 * base, ARCH_SET_GS the record's `gs_base` and the kernel GS base register,
 * where the program's base waits during a call, and neither touches the
 * kernel's own GS base. ARCH_GET_FS and ARCH_GET_GS read the registers, which
 * a selector the program loaded may have changed, not the record. An
 * address that cannot be written is EFAULT, another code EINVAL, and a call
 * with no process ESRCH.
 */
#include "arch/x86_64/tls.c"
#include "lib/kstring.h"

extern int printf(const char *format, ...);

struct percpu cpu_table[NR_CPUS];
static process_t proc;
static isr_params_t frame;
static uint64_t fs_base, gs_base, kernel_gs_base;
static uint64_t stored;
static int failures;

/* The address copy_to_user refuses. */
#define BAD_ADDR 0x1000ul

static const unsigned long refused[] = {USER_ADDR_MAX, USER_ADDR_MAX + 1, ~0ul};
static const unsigned long taken[] = {0, 0x401000ul, USER_ADDR_MAX - 1};

/** Record a failed row without ending the others. */
static void check(int ok, const char *row)
{
    if (!ok) {
        printf("tls64-hosttest: fail: %s\n", row);
        failures++;
    }
}

/** The register `msr` of the model; another one fails the test. */
static uint64_t *msr_of(uint32_t msr)
{
    switch (msr) {
    case MSR_FS_BASE:
        return &fs_base;
    case MSR_GS_BASE:
        return &gs_base;
    case MSR_KERNEL_GS_BASE:
        return &kernel_gs_base;
    default:
        check(0, "a register other than the three bases");
        return &stored;
    }
}

uint64_t msr_read(uint32_t msr)
{
    return *msr_of(msr);
}

void msr_write(uint32_t msr, uint64_t value)
{
    *msr_of(msr) = value;
}

/** Copy `n` bytes to `stored`, which stands for any user address but
 * BAD_ADDR. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if ((unsigned long)dst == BAD_ADDR || n != sizeof(stored)) {
        return -1;
    }
    k_memcpy(&stored, src, n);
    return 0;
}

/** Give the running process its bases and the registers the same ones,
 * with the kernel's own GS base apart. */
static void setup(void)
{
    k_memset(&proc, 0, sizeof(proc));
    proc.tls_base = 0x500000ul;
    proc.regs.gs_base = 0x600000ul;
    fs_base = proc.tls_base;
    kernel_gs_base = proc.regs.gs_base;
    gs_base = 0xFFFFFFFF80100000ul;
    stored = 0;
    cpu_table[0].current = &proc;
}

static long prctl(unsigned long code, unsigned long addr)
{
    frame.rdi = code;
    frame.rsi = addr;
    return sys_arch_prctl(&frame);
}

/** Every base unchanged since setup. */
static int unchanged(void)
{
    return proc.tls_base == 0x500000ul && fs_base == 0x500000ul &&
           proc.regs.gs_base == 0x600000ul && kernel_gs_base == 0x600000ul &&
           gs_base == 0xFFFFFFFF80100000ul;
}

static void run_clone(void)
{
    uintptr_t base;
    unsigned int i;

    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        base = 7;
        check(arch_clone_tls(refused[i], &base) == -EPERM && base == 7,
              "clone's base past user space");
    }
    for (i = 0; i < sizeof(taken) / sizeof(taken[0]); i++) {
        base = 7;
        check(arch_clone_tls(taken[i], &base) == 0 && base == taken[i], "clone's user base");
    }
}

static void run_set(void)
{
    unsigned int i;

    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        setup();
        check(prctl(ARCH_SET_FS, refused[i]) == -EPERM && unchanged(),
              "ARCH_SET_FS of a base past user space");
        check(prctl(ARCH_SET_GS, refused[i]) == -EPERM && unchanged(),
              "ARCH_SET_GS of a base past user space");
    }
    for (i = 0; i < sizeof(taken) / sizeof(taken[0]); i++) {
        setup();
        check(prctl(ARCH_SET_FS, taken[i]) == 0 && proc.tls_base == taken[i] &&
                  fs_base == taken[i] && proc.regs.gs_base == 0x600000ul &&
                  kernel_gs_base == 0x600000ul,
              "ARCH_SET_FS of a user base");
        check(prctl(ARCH_SET_GS, taken[i]) == 0 && proc.regs.gs_base == taken[i] &&
                  kernel_gs_base == taken[i] && gs_base == 0xFFFFFFFF80100000ul,
              "ARCH_SET_GS of a user base");
    }
}

static void run_get(void)
{
    setup();
    fs_base = 0;
    kernel_gs_base = 0;
    stored = 1;
    check(prctl(ARCH_GET_FS, 0x7000ul) == 0 && stored == 0, "ARCH_GET_FS reads the register");
    stored = 1;
    check(prctl(ARCH_GET_GS, 0x7000ul) == 0 && stored == 0, "ARCH_GET_GS reads the register");
    setup();
    check(prctl(ARCH_GET_FS, 0x7000ul) == 0 && stored == 0x500000ul, "ARCH_GET_FS of a base");
    check(prctl(ARCH_GET_GS, 0x7000ul) == 0 && stored == 0x600000ul, "ARCH_GET_GS of a base");
    check(prctl(ARCH_GET_FS, BAD_ADDR) == -EFAULT && prctl(ARCH_GET_GS, BAD_ADDR) == -EFAULT,
          "a get to an address that cannot be written");
}

static void run_other(void)
{
    setup();
    check(prctl(0x1005, 0x401000ul) == -EINVAL && unchanged(), "another code");
    cpu_table[0].current = NULL;
    check(prctl(ARCH_SET_FS, 0x401000ul) == -ESRCH && unchanged(), "a call with no process");
    setup();
    arch_tls_set(0x401000ul);
    check(fs_base == 0x401000ul && arch_tls_get() == 0x401000ul, "the thread pointer");
}

int main(void)
{
    run_clone();
    run_set();
    run_get();
    run_other();
    if (failures != 0) {
        return 1;
    }
    printf("tls64-hosttest: ok\n");
    return 0;
}
