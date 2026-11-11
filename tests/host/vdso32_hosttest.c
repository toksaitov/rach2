/**
 * Check the fast i386 entry's lock across its saved-EBP user copy, and its
 * count of the call for bkl/calls.
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -DRACH2_HOST_X86_64
 */
#include "arch/x86_64/vdso32.c"

extern int printf(const char *format, ...);
struct percpu cpu_table[NR_CPUS];
static process_t proc;
static struct mm space;
static int depth, bad, routed, fault, counted;

/* Image labels stand in for the linked assembler; this test builds no image. */
const char vdso32_text_start[1] = {0};
const char vdso32_text_end[1] = {0};
const char vdso32_vsyscall[1] = {0};
const char vdso32_vsyscall_end[1] = {0};
const char vdso32_vsyscall_entry[1] = {0};
const char vdso32_landing[1] = {0};
const char vdso32_sigreturn[1] = {0};
const char vdso32_sigreturn_end[1] = {0};
const char vdso32_rt_sigreturn[1] = {0};
const char vdso32_rt_sigreturn_end[1] = {0};
const char vdso32_clock_gettime[1] = {0};
const char vdso32_clock_gettime_end[1] = {0};
const char vdso32_clock_gettime64[1] = {0};
const char vdso32_clock_gettime64_end[1] = {0};
const char vdso32_gettimeofday[1] = {0};
const char vdso32_gettimeofday_end[1] = {0};
const char vdso32_time[1] = {0};
const char vdso32_time_end[1] = {0};
const char vdso32_clock_getres[1] = {0};
const char vdso32_clock_getres_end[1] = {0};
const char vdso32_getcpu[1] = {0};
const char vdso32_getcpu_end[1] = {0};

/** No image allocation is needed by the entry. */
void *alloc_kernel_frame(void)
{
    return NULL;
}

/** The modeled machine offers sysenter. */
int cpu_ia32_entry(void)
{
    return IA32_ENTRY_SYSENTER;
}

/** The image helpers are unused by the entry. */
phys_addr_t mmu_virt_to_phys(const volatile void *p)
{
    return (phys_addr_t)(uintptr_t)p;
}

/** No symbol lookup is needed by the entry. */
uint32_t vdso_elf_hash(const char *name)
{
    (void)name;
    return 0;
}

/** Account for the entry's lock acquisition. */
void bkl_enter(void)
{
    depth++;
}

/** Require the entry to hold the lock before its user copy, which can sleep. */
int copy_from_user(void *dst, const void __user *src, size_t size)
{
    uint32_t ebp = 0x12345678u;

    if (depth != 1 || (uintptr_t)src != 0x40001000u || size != sizeof(ebp)) {
        bad++;
    }
    if (fault) {
        return -EFAULT;
    }
    k_memcpy(dst, &ebp, sizeof(ebp));
    return 0;
}

/** The syscall dispatcher receives one lock and the sixth argument intact. */
void isr_router_locked(isr_params_t *p)
{
    if (depth != 1 || p->rbp != 0x12345678u) {
        bad++;
    }
    routed++;
    p->rax = 17;
}

/** The entry reads the clock before it takes the lock, for bkl/calls. */
uint64_t arch_cycles(void)
{
    if (depth != 0) {
        bad++;
    }
    return 1000;
}

/** The entry notes the ioctl request of a routed call under the lock. */
int isr_call_ioctl(const isr_params_t *p)
{
    if (depth != 1 || p->rax != 192 || p->rbp != 0x12345678u) {
        bad++;
    }
    return 0;
}

/** Every call is counted once, under its own number, before the return. */
void smp_call_end(uint32_t nr, int ioctl, uint64_t start)
{
    if (depth != 1 || nr != 192 || ioctl != 0 || start != 1000) {
        bad++;
    }
    counted++;
}

/** Return to user mode releases the entry's one lock. */
void isr_leave(isr_params_t *p)
{
    (void)p;
    if (depth != 1) {
        bad++;
    }
    depth--;
}

/** Take the big lock, as every i386 entry does in the test. */
int isr_entry_lock(const isr_params_t *params)
{
    (void)params;
    bkl_enter();
    return 1;
}

/** Both fast instructions copy under the lock, including a failed copy. */
int main(void)
{
    isr_params_t p;
    int entry;

    current_proc = &proc;
    proc.mm = &space;
    space.compat = 1;
    space.vdso = 0x70000000u;
    for (entry = 0; entry < 2; entry++) {
        for (fault = 0; fault < 2; fault++) {
            k_memset(&p, 0, sizeof(p));
            p.cs = GDT_USER_CODE32;
            p.ss = GDT_USER_DATA;
            p.rax = 192;
            p.rsp = entry ? 0 : 0x40001000u;
            p.rbp = entry ? 0x40001000u : 0;
            routed = 0;
            counted = 0;
            ia32_fast_syscall(&p, entry);
            if (depth || routed != !fault || counted != 1 || p.rsp != 0x40001000u ||
                (fault && ((int64_t)p.rax != -EFAULT || proc.orig_nr != 192))) {
                bad++;
            }
        }
    }
    printf("vdso32: result: %d faults\n", bad);
    return bad != 0;
}
