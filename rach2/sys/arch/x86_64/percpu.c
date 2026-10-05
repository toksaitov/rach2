/*
 * percpu.c: the x86-64's per-processor records, their GDTs and TSSs, and
 * the MSRs of `syscall`, as percpu.h describes them.
 */

#include "percpu.h"

#include "../x86/kcpu.h"
#include "../x86/smpboot.h"

#include "../../mm/paging.h"

#include "../../lib/kstring.h"

struct cpu cpus[NR_CPUS];

/* The entry code and `arch_cpu_id` read these four fields at fixed offsets.
 * TinyCC cannot fold an offset into an array size, so a kernel it builds checks
 * them at boot instead (`arch_context_check`). */
#if !defined(__TINYC__)
typedef char cpu_offsets_fit[(__builtin_offsetof(struct cpu, self) == 0 &&
                              __builtin_offsetof(struct cpu, kernel_rsp) == 8 &&
                              __builtin_offsetof(struct cpu, user_rsp) == 16 &&
                              __builtin_offsetof(struct cpu, id) == 24)
                                 ? 1
                                 : -1];
#endif

/*
 * The interrupt stacks of each processor, for the double fault, the NMI, the
 * machine check and the debug exception. A double fault comes from a kernel
 * stack that ran into its guard page and has no room for the frame, and the
 * record and the panic its handler writes take the most; the NMI and the
 * machine check print and panic, and the debug entry copies a user frame to
 * the thread's stack or defers a kernel exception.
 */
#define DOUBLE_FAULT_STACK_SIZE 0x2000
#define IST_STACK_SIZE          0x1000
static uint8_t double_fault_stacks[NR_CPUS][DOUBLE_FAULT_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t nmi_stacks[NR_CPUS][IST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t machine_check_stacks[NR_CPUS][IST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t debug_stacks[NR_CPUS][IST_STACK_SIZE] __attribute__((aligned(16)));

/* The flags `syscall` clears on entry: TF, IF, DF, IOPL, NT and AC, so the
 * kernel starts with interrupts off and the direction flag clear. */
#define SYSCALL_FMASK 0x47700u

/* The `syscall` entry (isr_handler.s). */
extern void syscall_entry(void);

/* The operand of `lgdt`, and the routine that loads it and reloads the
 * segment registers (switch.s). */
struct desc_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));
extern void x86_64_load_gdt(const struct desc_ptr *ptr);

uint64_t msr_read(uint32_t msr)
{
    uint32_t lo, hi;

    kcpu_rdmsr(msr, &lo, &hi);
    return ((uint64_t)hi << 32) | lo;
}

void msr_write(uint32_t msr, uint64_t value)
{
    kcpu_wrmsr(msr, (uint32_t)value, (uint32_t)(value >> 32));
}

/*
 * A code or data descriptor (Intel SDM Vol. 3A, 3.4.5). In long mode the
 * processor reads only the access byte and, for code, the L bit; base and
 * limit are the flat ones every segment has.
 */
#define DESC_KERNEL_CODE 0x00AF9A000000FFFFUL /* present, DPL 0, code, L */
#define DESC_KERNEL_DATA 0x00CF92000000FFFFUL /* present, DPL 0, writable data */
#define DESC_USER_DATA   0x00CFF2000000FFFFUL /* present, DPL 3, writable data */
#define DESC_USER_CODE   0x00AFFA000000FFFFUL /* present, DPL 3, code, L */

/*
 * Fill processor `id`'s record: the GDT with the TSS's descriptor, which is
 * 16 bytes in long mode, its base split over both words, and the TSS with
 * the interrupt stacks and the closed I/O map.
 */
static void cpu_fill(uint32_t id)
{
    struct cpu *c = &cpus[id];
    uint64_t base = (uint64_t)(uintptr_t)&c->tss;
    uint64_t limit = sizeof(struct tss64) - 1;

    k_memset(c, 0, sizeof(*c));
    c->self = c;
    c->id = id;

    c->gdt[GDT_KERNEL_CODE >> 3] = DESC_KERNEL_CODE;
    c->gdt[GDT_KERNEL_DATA >> 3] = DESC_KERNEL_DATA;
    c->gdt[GDT_USER_DATA >> 3] = DESC_USER_DATA;
    c->gdt[GDT_USER_CODE >> 3] = DESC_USER_CODE;
    /* Present, DPL 0, type 9: an available 64-bit TSS. */
    c->gdt[GDT_TSS >> 3] = (limit & 0xFFFFu) | ((base & 0xFFFFFFu) << 16) | (0x89UL << 40) |
                           (((limit >> 16) & 0xFu) << 48) | (((base >> 24) & 0xFFu) << 56);
    c->gdt[(GDT_TSS >> 3) + 1] = base >> 32;

    c->tss.ist[IST_DOUBLE_FAULT - 1] =
        (uint64_t)(uintptr_t)&double_fault_stacks[id][DOUBLE_FAULT_STACK_SIZE];
    c->tss.ist[IST_NMI - 1] = (uint64_t)(uintptr_t)&nmi_stacks[id][IST_STACK_SIZE];
    c->tss.ist[IST_MACHINE_CHECK - 1] =
        (uint64_t)(uintptr_t)&machine_check_stacks[id][IST_STACK_SIZE];
    c->tss.ist[IST_DEBUG - 1] = (uint64_t)(uintptr_t)&debug_stacks[id][IST_STACK_SIZE];
    c->tss.iomap_base = sizeof(struct tss64);
}

/**
 * Build the boot processor's record and load its GDT, then make the record
 * the kernel's GS base. The base the program will have, in the other slot,
 * starts at 0.
 */
void init_gdt(void)
{
    struct desc_ptr ptr;

    cpu_fill(0);
    ptr.limit = sizeof(cpus[0].gdt) - 1;
    ptr.base = (uint64_t)(uintptr_t)cpus[0].gdt;
    x86_64_load_gdt(&ptr);
    msr_write(MSR_GS_BASE, (uint64_t)(uintptr_t)&cpus[0]);
    msr_write(MSR_KERNEL_GS_BASE, 0);
}

/**
 * Give the boot processor its kernel stack, load the task register, and
 * turn `syscall` on: IA32_STAR has the kernel's code selector for the
 * entry and the base of the user's for the return (percpu.h), IA32_LSTAR
 * the entry, and IA32_FMASK the flags the entry clears.
 */
void init_tss(uintptr_t kernel_stack)
{
    cpu_set_kernel_stack(kernel_stack);
    __asm__ __volatile__("ltr %w0" : : "r"(GDT_TSS));

    msr_write(MSR_STAR, ((uint64_t)(GDT_USER_DATA - 8u) << 48) | ((uint64_t)GDT_KERNEL_CODE << 32));
    msr_write(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    msr_write(MSR_FMASK, SYSCALL_FMASK);
    msr_write(MSR_EFER, msr_read(MSR_EFER) | EFER_SCE);
}

/**
 * The x86-64's part of a started processor's start: the kernel's page
 * tables in place of the trampoline's, its record, GDT and TSS, GS on the
 * record before anything reads `this_cpu()`, `syscall`, and the IDT every
 * processor shares.
 */
void arch_ap_tables(int cpu, uintptr_t stack_top)
{
    struct desc_ptr ptr;

    arch_space_load(get_page_dir_phys(_kernel_dir));
    cpu_fill((uint32_t)cpu);
    ptr.limit = sizeof(cpus[cpu].gdt) - 1;
    ptr.base = (uint64_t)(uintptr_t)cpus[cpu].gdt;
    x86_64_load_gdt(&ptr);
    msr_write(MSR_GS_BASE, (uint64_t)(uintptr_t)&cpus[cpu]);
    msr_write(MSR_KERNEL_GS_BASE, 0);
    init_tss(stack_top);
    idt_load();
}

void cpu_set_kernel_stack(uintptr_t top)
{
    struct cpu *c = cpu_self();

    c->tss.rsp0 = top;
    c->kernel_rsp = top;
}
