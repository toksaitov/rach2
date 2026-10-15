/* tss.c: each processor's kernel TSS and double fault TSS, set up and
 * loaded. */

#include "tss.h"
#include "gdt.h"
#include "../x86/isr.h"
#include "../../lib/kstring.h"

tss_t kernel_tss[NR_CPUS];
tss_t double_fault_tss[NR_CPUS];

/* The stack each processor's double fault task runs on, in words: a kernel
 * stack's size, for the record and the panic its handler writes. */
#define DOUBLE_FAULT_STACK_WORDS (0x4000 / 4)
static uint32_t double_fault_stack[NR_CPUS][DOUBLE_FAULT_STACK_WORDS];

/* The page directory every double fault task loads, once paging is on. */
static uint32_t double_fault_dir;

/* EFLAGS with only bit 1 set, which the processor keeps as 1: interrupts
 * off. */
#define DOUBLE_FAULT_EFLAGS 0x2u

/* Loads the task register with the given GDT selector (ltr). */
extern void _load_tss(uint16_t selector);

/**
 * Initialize the kernel TSS and load it into the task register, and set up
 * the double fault's TSS beside it.
 *
 * On the i386 the TSS supplies the ring 0 stack the CPU switches to on a
 * privilege change: ss0/esp0 are read when a trap or interrupt crosses from
 * ring 3 to ring 0. Each processor has one TSS for every thread, and the
 * kernel switches threads by software, so only ss0/esp0 and the I/O map
 * base of `kernel_tss` matter, and it is where the processor saves what was
 * running when it switches to the double fault's task.
 *
 * That switch is the kernel's one hardware task switch. The gate of vector
 * 8 names the processor's `double_fault_tss`, which starts `isr_double_fault`
 * in the kernel's segments and the processor's own %fs, with interrupts off,
 * on a stack of its own, since a double fault may come from a kernel stack
 * that has run out. Its page directory is the kernel's, which `mmu_start`
 * gives it (`tss_set_double_fault_dir`) before paging is on.
 *
 * iomap_base is set to sizeof(tss_t), past the end of the segment, which the
 * CPU reads as an empty I/O permission bitmap: every port access from ring 3
 * faults.
 *
 * @param kernel_stack Top of the ring 0 stack to install in esp0.
 */
void init_tss_cpu(uint32_t cpu, uint32_t kernel_stack)
{
    tss_t *t = &kernel_tss[cpu];
    tss_t *df = &double_fault_tss[cpu];

    k_memset(t, 0, sizeof(*t));

    t->ss0 = KERNEL_DATA_SELECTOR;
    t->esp0 = kernel_stack;

    t->iomap_base = sizeof(tss_t);

    k_memset(df, 0, sizeof(*df));
    df->eip = (uint32_t)&isr_double_fault;
    df->eflags = DOUBLE_FAULT_EFLAGS;
    df->esp = (uint32_t)&double_fault_stack[cpu][DOUBLE_FAULT_STACK_WORDS];
    df->cs = KERNEL_CODE_SELECTOR;
    df->ss = KERNEL_DATA_SELECTOR;
    df->ds = KERNEL_DATA_SELECTOR;
    df->es = KERNEL_DATA_SELECTOR;
    df->fs = GDT_PERCPU_SELECTOR;
    df->gs = KERNEL_DATA_SELECTOR;
    df->cr3 = double_fault_dir;
    df->iomap_base = sizeof(tss_t);

    _load_tss(TSS_SELECTOR);
}

void init_tss(uint32_t kernel_stack)
{
    init_tss_cpu(0, kernel_stack);
}

void tss_set_kernel_stack(uint32_t stack)
{
    kernel_tss[arch_cpu_id()].esp0 = stack;
}

void tss_set_double_fault_dir(uint32_t dir)
{
    int i;

    double_fault_dir = dir;
    for (i = 0; i < NR_CPUS; i++) {
        double_fault_tss[i].cr3 = dir;
    }
}
