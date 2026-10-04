#ifndef _PAT_H_
#define _PAT_H_

/*
 * pat: the page attribute table, whose entry 1 the kernel makes
 * write-combining so that the framebuffer's mapping can be (`PTE_WC`,
 * fbcon_window in boot.c). It is a file of its own because it logs, and
 * kcpu.c is also compiled into test programs, which have no log.
 */

/**
 * Make entry 1 of the running processor's page attribute table
 * write-combining, the entry a page table entry with PWT alone selects
 * (`PTE_WC`), when `cpuid` reports the table (leaf 1, EDX bit 16). The
 * other seven entries keep their reset types, so the kernel's other entries,
 * which set PWT and PCD together or neither, keep theirs.
 *
 * The boot processor runs it before the kernel's tables map the
 * framebuffer, and its answer is `pat_write_combining`'s; a started
 * processor runs it first, before it can draw on the console, and writes
 * the same table when the boot processor did.
 */
void pat_setup(void);

/** Whether the boot processor's `pat_setup` made `PTE_WC`
 * write-combining. */
int pat_write_combining(void);

#endif /* _PAT_H_ */
