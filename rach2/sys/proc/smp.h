#ifndef _SMP_H_
#define _SMP_H_

#include "percpu.h"

struct page_dir;

/*
 * The processors past the first, and the one lock for the kernel.
 *
 * Every entry to the kernel, a system call, a fault or an interrupt, takes
 * the big lock (`bkl_enter`), and every return from one gives it back
 * (`bkl_exit`). A processor that holds it may take it again: an interrupt
 * in the middle of a system call nests inside it. The lock is free when the
 * processor returns to user mode or waits idle (`bkl_drop`, `bkl_take`), so
 * user code runs on every processor at once, and kernel code on one at a
 * time, as Linux 2.0 and FreeBSD's Giant began. The context switch keeps the
 * lock held and hands the depth from one process to the next (sched.c).
 *
 * The interprocessor interrupts are taken without the lock: a TLB request,
 * which a holder sends and waits for, and the stop, which a panic sends.
 * Each is a word in the target's record and an interrupt that has it look;
 * a processor that waits with interrupts off, for the big lock or for a
 * spinlock, looks itself (`smp_service`).
 */

/** Take the big lock on an entry to the kernel, or once more when this
 * processor holds it. */
void bkl_enter(void);

/** Give back what `bkl_enter` took, the lock itself on the outermost. */
void bkl_exit(void);

/** Let go of the big lock whatever the depth, before an idle wait or the
 * entry to user mode, and return the depth for `bkl_take`. Interrupts are
 * off. */
int bkl_drop(void);

/** Take the big lock again at `depth`, as `bkl_drop` returned it.
 * Interrupts are off. */
void bkl_take(int depth);

/** The processors started and taking work, the boot processor counted. */
int smp_cpu_count(void);

/** Answer the requests other processors left this one: drop a translation,
 * or stop. Called with interrupts off. */
void smp_service(void);

/** The interprocessor interrupt's handler, which the platform runs without
 * the big lock: what it asks is in the record (`smp_service`), and the halt
 * it ends was its other purpose. */
void smp_ipi(void);

/** Have `cpu`, idle, look at its run queue now rather than at its tick. */
void smp_kick(int cpu);

/**
 * Drop the translation of the user page `va`, or every user translation
 * for CPU_TLB_ALL, on each other processor that translates through `dir`,
 * and wait until each has. The caller holds the big lock and has changed
 * the page's entry.
 */
void smp_tlb_shootdown(const struct page_dir *dir, uintptr_t va);

/**
 * Note that a kernel mapping changed, after the caller dropped its own
 * translation of it: every other processor drops all of its translations
 * before it next takes the big lock, the one place it reads kernel memory
 * that was not mapped when it last held it.
 */
void smp_kernel_mapping_changed(void);

/**
 * Have every other processor that still translates through `dir`, which no
 * thread uses any more, load the kernel's directory, and wait until each
 * has: `dir` is about to be freed. Only an idle task or a kernel thread can
 * be running there, since a switch to a thread with no address space keeps
 * the directory loaded, as Linux's lazy TLB mode does, which spares a
 * process that blocks and wakes two loads of CR3 and the flushes they cause.
 */
void smp_space_release(const struct page_dir *dir);

/**
 * Stop every other processor for good, for a panic and a power off, and
 * wait, a second at most for each, until each has taken the stop, so that
 * none is still writing the log or the screen when the caller does.
 */
void smp_stop_others(void);

/** Make the boot processor's idle task and start the others the firmware
 * lists, as many as `maxcpus=` allows. */
void smp_start(void);

#endif /* _SMP_H_ */
