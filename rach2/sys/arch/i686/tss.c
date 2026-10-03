#include "tss.h"
#include "../x86/isr.h"
#include "../../lib/kstring.h"

tss_t kernel_tss;
tss_t double_fault_tss;

/* The stack the double fault's task runs on, in words: a kernel stack's
 * size, for the record and the panic its handler writes. */
#define DOUBLE_FAULT_STACK_WORDS (0x4000 / 4)
static uint32_t double_fault_stack[DOUBLE_FAULT_STACK_WORDS];

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
 * ring 3 to ring 0. The kernel uses one TSS for all tasks and switches
 * between them by software, so only ss0/esp0 and the I/O map base of
 * `kernel_tss` matter, and it is where the processor saves what was running
 * when it switches to the double fault's task.
 *
 * That switch is the kernel's one hardware task switch. The gate of vector
 * 8 names `double_fault_tss`, which starts `isr_double_fault` in the
 * kernel's segments, with interrupts off, on a stack of its own, since a
 * double fault may come from a kernel stack that has run out. Its page
 * directory is the kernel's, which `init_paging` gives it
 * (`tss_set_double_fault_dir`) before paging is on; until then the
 * processor loads no directory from it.
 *
 * iomap_base is set to sizeof(tss_t), past the end of the segment, which the
 * CPU reads as an empty I/O permission bitmap: every port access from ring 3
 * faults.
 *
 * @param kernel_stack Top of the ring 0 stack to install in esp0.
 */
void init_tss(uint32_t kernel_stack)
{
    k_memset(&kernel_tss, 0, sizeof(kernel_tss));

    kernel_tss.ss0 = KERNEL_DATA_SELECTOR;
    kernel_tss.esp0 = kernel_stack;

    kernel_tss.iomap_base = sizeof(tss_t);

    k_memset(&double_fault_tss, 0, sizeof(double_fault_tss));
    double_fault_tss.eip = (uint32_t)&isr_double_fault;
    double_fault_tss.eflags = DOUBLE_FAULT_EFLAGS;
    double_fault_tss.esp = (uint32_t)&double_fault_stack[DOUBLE_FAULT_STACK_WORDS];
    double_fault_tss.cs = KERNEL_CODE_SELECTOR;
    double_fault_tss.ss = KERNEL_DATA_SELECTOR;
    double_fault_tss.ds = KERNEL_DATA_SELECTOR;
    double_fault_tss.es = KERNEL_DATA_SELECTOR;
    double_fault_tss.fs = KERNEL_DATA_SELECTOR;
    double_fault_tss.gs = KERNEL_DATA_SELECTOR;
    double_fault_tss.iomap_base = sizeof(tss_t);

    _load_tss(TSS_SELECTOR);
}

void tss_set_kernel_stack(uint32_t stack)
{
    kernel_tss.esp0 = stack;
}

void tss_set_double_fault_dir(uint32_t dir)
{
    double_fault_tss.cr3 = dir;
}
