/*
 * smp.h: the processors past the first, and the one lock for the kernel.
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
 * The interprocessor interrupts are taken without the lock: a TLB request
 * or a page directory to give up, which a holder sends and waits for, the
 * stop, which a panic or a power off sends, and a kick that ends an idle
 * wait. A request is a word in the target's record and an interrupt that
 * has it look; a processor that waits with interrupts off, for the big lock
 * or for a spinlock, looks itself (`smp_service`).
 */
#ifndef _SMP_H_
#define _SMP_H_

#include "percpu.h"

struct page_dir;

/** Take the big lock on an entry to the kernel, or once more when this
 * processor holds it. While it spins, with interrupts off, it answers this
 * processor's requests (`smp_service`), and once it has the lock it drops
 * every translation when a kernel mapping changed since this processor
 * last held it. */
void bkl_enter(void);

/** Give back what `bkl_enter` took, the lock itself on the outermost; a
 * panic when this processor does not hold it. */
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

/** Answer the requests other processors left this one: stop for good, drop
 * a translation or every one, or give up a page directory for the
 * kernel's. Called with interrupts off. */
void smp_service(void);

/** The interprocessor interrupt's handler, which the platform runs without
 * the big lock: it answers the requests in this processor's record
 * (`smp_service`). An interrupt sent only to end an idle halt finds none. */
void smp_ipi(void);

/** Send processor `cpu` an interprocessor interrupt, unless it is this one
 * or not online, so that an idle wait there ends and it selects from its
 * run queue now rather than at its tick. */
void smp_kick(int cpu);

/**
 * Drop the translation of the user page `va`, or every user translation
 * for CPU_TLB_ALL, on each other processor that translates through `dir`,
 * and wait until each has; a processor whose current thread has no address
 * space is marked to load `dir` again at its next switch instead. The
 * caller holds the big lock and has changed the page's entry.
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
 * lists, as many as `maxcpus=` allows, the boot one counted; a panic when
 * the heap has no room for the boot processor's idle task. A processor
 * that does not start or come online is reported and left out. */
void smp_start(void);

#endif /* _SMP_H_ */
