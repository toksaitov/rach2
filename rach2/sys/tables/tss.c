#include "tss.h"
#include "../lib/kstring.h"

/*
 * Global TSS instance
 */
tss_t kernel_tss;

/*
 * Load TSS register with selector
 */
extern void _load_tss(uint16_t selector);

void init_tss(uint32_t kernel_stack)
{
    /* Clear the TSS */
    k_memset(&kernel_tss, 0, sizeof(kernel_tss));

    /* Set up ring 0 stack - used when transitioning from user to kernel mode */
    kernel_tss.ss0 = KERNEL_DATA_SELECTOR;
    kernel_tss.esp0 = kernel_stack;

    /* Set I/O map base to size of TSS (no I/O bitmap) */
    kernel_tss.iomap_base = sizeof(tss_t);

    /* Load the TSS into the task register */
    _load_tss(TSS_SELECTOR);
}

void tss_set_kernel_stack(uint32_t stack)
{
    kernel_tss.esp0 = stack;
}
