/**
 * Check the fast i386 entry's copy of the saved EBP before the call's entry,
 * and the return of a call whose copy fails.
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -DRACH2_HOST_X86_64
 */
#include "arch/x86_64/vdso32.c"

extern int printf(const char *format, ...);
struct percpu cpu_table[NR_CPUS];
static process_t proc;
static struct mm space;
static int bad, entered, pushed, left, fault;

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

/* The cycle account, off as by default (sched.c). */
int sched_cycle_account;

/** Account for the frame record a failed copy pushes for the return. */
void bkl_frame_push(int locked)
{
    if (locked) {
        bad++;
    }
    pushed++;
}

/** The copy comes before the call's entry, with no lock taken. */
int copy_from_user(void *dst, const void __user *src, size_t size)
{
    uint32_t ebp = 0x12345678u;

    if (entered != 0 || (uintptr_t)src != 0x40001000u || size != sizeof(ebp)) {
        bad++;
    }
    if (fault) {
        return -EFAULT;
    }
    k_memcpy(dst, &ebp, sizeof(ebp));
    return 0;
}

/** The call's entry receives the sixth argument intact and the landing pad
 * as its return. */
void isr_syscall(isr_params_t *p)
{
    if (p->rbp != 0x12345678u || p->rax != 192) {
        bad++;
    }
    entered++;
    p->rax = 17;
}

/** No clock is read with the cycle account off. */
uint64_t arch_cycles(void)
{
    bad++;
    return 1000;
}

/** Every call returns once, after its entry or its failed copy. */
void isr_leave(isr_params_t *p)
{
    (void)p;
    if (entered + pushed != 1) {
        bad++;
    }
    left++;
}

/** Both fast instructions copy the saved EBP before the entry, and a failed
 * copy answers -EFAULT without the call. */
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
            entered = 0;
            pushed = 0;
            left = 0;
            ia32_fast_syscall(&p, entry);
            if (left != 1 || entered != !fault || pushed != fault || p.rsp != 0x40001000u ||
                (fault && ((int64_t)p.rax != -EFAULT || proc.orig_nr != 192))) {
                bad++;
            }
        }
    }
    printf("vdso32: result: %d faults\n", bad);
    return bad != 0;
}
