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

/** Take the big lock as `bkl_enter` does when it is free or this processor
 * holds it, and return 1; return 0 at once, taking nothing, when another
 * processor holds it or waits for its turn. Interrupts are off. */
int bkl_try_enter(void);

/**
 * Let a processor that has claimed the big lock have it, and take it back at
 * the same depth; nothing when none has. `schedule` calls
 * this for the process it has just resumed, whose kernel state may be seen as
 * at any sleep: a processor that goes from one process inside the kernel to
 * the next never returns to user mode and would keep the lock across a whole
 * run queue. Interrupts are off.
 */
void bkl_yield(void);

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

/** Read the sum and longest big-lock acquisition in microseconds
 * elapsed since boot. The caller holds the big lock. Wait times are zero
 * without a calibrated invariant TSC. */
void smp_wait_stats(uint64_t *total_us, uint64_t *max_us);

/** Name the kernel entry that holds the big lock on this processor, by its
 * vector and, for a system call, its number (-1 otherwise), for the longest
 * hold's record. Only the outermost entry counts; a take again after an idle
 * halt names none. */
void bkl_note_entry(uint32_t vector, int32_t nr);

/** Read the longest hold of the big lock since boot, from its take to its
 * give-back or drop, in microseconds, with its entry's vector (0 for none,
 * a kernel thread's) and system call number (-1 for none). The caller holds
 * the big lock. The time is zero without a calibrated invariant TSC. */
void smp_hold_stats(uint64_t *max_us, uint32_t *vector, int32_t *nr);

/* The instruction a hold's sample keeps, and the return addresses above it. */
#define BKL_SAMPLE_PCS 8

/** Sample a hold of the big lock from an interrupt that found this processor
 * holding it: when the hold is the oldest any interrupt has found, keep `pc`,
 * the instruction the interrupt stopped, and the return addresses of the
 * frames from `fp`, its frame pointer, and keep the first sample past each
 * of the marks into each hold, for the longest hold's record. A hold that keeps
 * interrupts off is sampled only when it turns them on. */
void bkl_hold_sample(uintptr_t pc, uintptr_t fp);

/** Read the oldest hold an interrupt has found, in microseconds since its
 * take, and the instruction it stopped with its callers, 0 past the last.
 * The caller holds the big lock. */
void smp_hold_sample(uint64_t *age_us, uintptr_t pc[BKL_SAMPLE_PCS]);

/* The marks a hold is sampled past: 1, 2, 4, 8 and 16 s. */
#define BKL_SAMPLE_MARKS 5

/** Read the longest hold's first sample past mark `mark`, 2^mark seconds,
 * its age in microseconds and its instruction with callers, all 0 when no
 * interrupt came that late. The caller holds the big lock. */
void smp_hold_early(int mark, uint64_t *age_us, uintptr_t pc[BKL_SAMPLE_PCS]);

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
 * Drop the translations of the user pages [`start`, `end`), or every user
 * translation when `start` is CPU_TLB_ALL, on each other processor that
 * translates through `dir`, with one request to each, and wait until each
 * has; a processor whose current thread has no address space is marked to
 * load `dir` again at its next switch instead. The caller holds the big
 * lock and has changed the pages' entries, and frees a frame they named
 * only after this returns.
 */
void smp_tlb_shootdown(const struct page_dir *dir, uintptr_t start, uintptr_t end);

/** The shootdowns since boot that interrupted at least one processor, and
 * the interrupts they sent, modulo 2^32, which `/proc/stat` gives as
 * `tlb_shootdowns` and `tlb_shootdown_ipis`. `which` 0 is the first, 1 the
 * second. */
uint32_t smp_tlb_count(int which);

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
