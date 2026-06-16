#include "tss.h"
#include "../lib/kstring.h"

tss_t kernel_tss;

/* Loads the task register with the given GDT selector (ltr). */
extern void _load_tss(uint16_t selector);

/**
 * Initialize the single kernel TSS and load it into the task register.
 *
 * On the i386 the TSS supplies the ring 0 stack the CPU switches to on a
 * privilege change: ss0/esp0 are read when a trap or interrupt crosses from
 * ring 3 to ring 0. The kernel uses one TSS for all tasks and never task-
 * switches through it, so only ss0/esp0 and the I/O map base matter.
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

    _load_tss(TSS_SELECTOR);
}

void tss_set_kernel_stack(uint32_t stack)
{
    kernel_tss.esp0 = stack;
}
