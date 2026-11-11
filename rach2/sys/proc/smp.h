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
 * or a page directory to give up, which one sender at a time sends and
 * waits for, the stop, which a panic or a power off sends, and a kick that
 * ends an idle wait. A request is a word in the target's record and an
 * interrupt that has it look; a processor that waits with interrupts off,
 * for the big lock or for a spinlock, looks itself (`smp_service`).
 */
#ifndef _SMP_H_
#define _SMP_H_

#include "percpu.h"
#include "../lib/kuser.h"

struct page_dir;

/** Take the big lock on an entry to the kernel, or once more when this
 * processor holds it. While it spins, with interrupts off, it answers this
 * processor's requests (`smp_service`). Never under a spinlock. */
void bkl_enter(void);

/** Nonzero when the running thread holds the big lock: this processor's
 * depth, which goes with the thread across a switch. Any context. */
int bkl_held(void);

/** Take the big lock as `bkl_enter` does when it is free or this processor
 * holds it, and return 1; return 0 at once, taking nothing, when another
 * processor holds it or waits for its turn. Interrupts are off. */
int bkl_try_enter(void);

/**
 * Let a processor that has claimed the big lock have it, and take it back at
 * the same depth; nothing when none has. `sched_service_point` calls it in
 * a long call on a processor with no switch to make, and the mouse's
 * command runs between their waits, since a thread that never sleeps and
 * never returns to user mode would keep the lock across them. Interrupts are
 * off.
 */
void bkl_yield(void);

/** Give back what `bkl_enter` took, the lock itself on the outermost; a
 * panic when this processor does not hold it. */
void bkl_exit(void);

/**
 * Record that the router of the entry under way took the big lock, for
 * `locked` 1, or ran its handlers without it, for 0, which `isr_leave` reads
 * back with `bkl_frame_pop`. The record is the current thread's, a bit for
 * each frame on its kernel stack, since an entry that sleeps may return on
 * another processor and other threads' entries come and go here meanwhile;
 * a processor with no current thread keeps its own.
 */
void bkl_frame_push(int locked);

/** Take back the record of the newest frame, 1 when its router took the
 * big lock. */
int bkl_frame_pop(void);

/** Let go of the big lock whatever the depth, before an idle wait or the
 * entry to user mode, and return the depth for `bkl_take`. Interrupts are
 * off. */
int bkl_drop(void);

/** Take the big lock again at `depth`, as `bkl_drop` returned it.
 * Interrupts are off. */
void bkl_take(int depth);

/** The processors started and taking work, the boot processor counted. */
int smp_cpu_count(void);

/**
 * Whether this processor may write what the big lock guards from where it
 * runs: it has begun a panic, which stopped the others; or it is not in an
 * NMI and holds the big lock or is the only processor started. klog keeps
 * a record made elsewhere for a processor that may (klog.c).
 */
int smp_serialized(void);

/** Read the sum and longest big-lock acquisition in microseconds
 * elapsed since boot, and the outer holds completed during that longest
 * wait, modulo 2^32. The caller holds the big lock. Wait times are zero
 * without a calibrated invariant TSC. */
void smp_wait_stats(uint64_t *total_us, uint64_t *max_us, uint32_t *holds);

/* The last outer holds completed during the longest big-lock acquisition. */
#define BKL_WAIT_HOLDS 32u

/** Read retained hold `index`, below the smaller of BKL_WAIT_HOLDS and
 * smp_wait_stats's holds, oldest first. Return its full time in microseconds
 * and entry, as smp_hold_stats does. A hold may begin before the wait.
 * The caller holds the big lock. */
void smp_wait_hold(unsigned int index, uint64_t *held_us, uint32_t *vector, int32_t *nr);

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

/* The keys of the holds by entry (`smp_entry_hold`): a system call's number
 * below `BKL_ENTRY_SYSCALLS`, then each interrupt vector's, the last for the
 * system calls numbered past the first range. */
#define BKL_ENTRY_SYSCALLS 512
#define BKL_ENTRY_KEYS     (BKL_ENTRY_SYSCALLS + 256 + 1)

/** Read the holds of the big lock that entry `key` took since boot, the
 * boot processor's start-up left out: their sum in microseconds and their
 * number. A hold counts under the entry that took the lock, which a system
 * call keeps while it sleeps and the processor runs others, up to its next
 * return to user mode or idle halt. The caller holds the big lock. */
void smp_entry_hold(int key, uint64_t *held_us, uint32_t *holds);

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

/* The ioctl requests the call counts (`smp_calls_show`) keep apart, the
 * slots of the profile of kernel time under the big lock, and the frames a
 * profile's sample keeps: the instruction and three callers. */
#define BKL_IOCTL_SLOTS    256
#define BKL_PROFILE_SLOTS  16384
#define BKL_PROFILE_FRAMES 4

/* The profile's key for an ioctl, which keeps its request beside it; the
 * keys below it are those of `smp_entry_hold`. */
#define BKL_PROFILE_IOCTL BKL_ENTRY_KEYS

/* A process's entry as `bkl_entry_push` replaced it. */
struct bkl_entry_saved {
    uint32_t entry;
    uint32_t cmd;
};

/** Mark the current process, if any, as in the entry by `vector`, the system
 * call `nr` for 0x80 and -1 otherwise, which `bkl_profile_sample` keys its
 * samples by, and keep the entry it was in at `saved`. `ioctl` set marks the
 * call as ioctl request `cmd`, of either ABI. A router calls it before its
 * handlers and `bkl_entry_pop` after them, on the same process, which keeps
 * the mark while it sleeps and gets it back on any processor. */
void bkl_entry_push(struct bkl_entry_saved *saved, uint32_t vector, int32_t nr, int ioctl,
                    uint32_t cmd);

/** Put back the entry `bkl_entry_push` replaced. */
void bkl_entry_pop(const struct bkl_entry_saved *saved);

/** Charge the hold under way on this processor, the outermost entry's, to
 * ioctl request `cmd` as well as to the call, and return the request's slot
 * plus one for `smp_call_end`, 0 when every slot holds another request.
 * The caller holds the big lock. */
int bkl_note_ioctl(uint32_t cmd);

/** Count the system call `nr` that ends now: its time from `start`, the
 * cycle count before its entry took the big lock, to now, which includes
 * its wait for the lock and its sleeps, and, for `ioctl` above 0, the same
 * under that request's slot. The caller holds the big lock. */
void smp_call_end(uint32_t nr, int ioctl, uint64_t start);

/** Count a sample of the kernel's time under the big lock: the tick found
 * this processor holding it at `pc`, with the frame pointer `fp`, which
 * gives its callers, in the current process's entry (`bkl_entry_push`).
 * Nothing when this processor took the lock for the tick itself, from user
 * mode or an idle wait. */
void bkl_profile_sample(uintptr_t pc, uintptr_t fp);

/** Write the call counts as debugfs's `bkl/calls` shows them, as far as `cap`
 * bytes at `buf` go, and return the whole text's length: for each system call,
 * entry vector and ioctl request with any count, the calls, their time from
 * entry to return, the holds of the big lock charged to it, and their time
 * and their waits for the lock, in microseconds. The caller holds the big
 * lock. */
int32_t smp_calls_show(void *data, char *buf, uint32_t cap);

/** Zero the call counts, keeping the requests in their slots; `len`. */
int32_t smp_calls_store(void *data, const char __user *buf, uint32_t len);

/** Write the profile as debugfs's `bkl/profile` shows it: the samples, those
 * a full table lost, and for each entry, instruction and three callers
 * sampled, its count. The caller holds the big lock, which keeps the other
 * processors' ticks from sampling; this processor's tick takes no sample
 * while the text is written, so that the slots' counts and `lost` sum to
 * `samples`. */
int32_t smp_profile_show(void *data, char *buf, uint32_t cap);

/** Zero the profile; `len`. The caller holds the big lock, which keeps the
 * other processors' ticks from sampling; this processor's tick is held off
 * while the table and its totals are zeroed, so that no sample lands in a
 * cleared slot before its total is cleared. */
int32_t smp_profile_store(void *data, const char __user *buf, uint32_t len);

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
 * load `dir` again at its next switch instead. One sender at a time sends
 * requests (smp.c), with interrupts off while it waits. The caller holds
 * the big lock and has changed the pages' entries, and frees a frame they
 * named only after this returns.
 */
void smp_tlb_shootdown(const struct page_dir *dir, uintptr_t start, uintptr_t end);

/**
 * Drop the translations of the kernel pages [`start`, `end`), or every
 * translation when `start` is CPU_TLB_ALL, on each other processor online,
 * and wait until each has, as Linux's `flush_tlb_kernel_range`
 * (arch/x86/mm/tlb.c:1099-1109). The caller has changed the entries and
 * dropped its own translations, and frees a frame they named only after
 * this returns. A processor reads kernel memory without the big lock, at
 * its switches and idle waits among others, so the flush is made at once,
 * not at its next take of the lock. Any context but an NMI; interrupts are
 * off while it waits, and the caller may hold spinlocks, since a processor
 * that spins for one answers requests.
 */
void smp_kernel_flush(uintptr_t start, uintptr_t end);

/** The shootdowns since boot that interrupted at least one processor, and
 * the interrupts they sent, modulo 2^32, which `/proc/stat` gives as
 * `tlb_shootdowns` and `tlb_shootdown_ipis`. `which` 0 is the first, 1 the
 * second. */
uint32_t smp_tlb_count(int which);

/* A barrier request's kinds: a full memory barrier, or one and a
 * serializing instruction. */
#define SMP_BARRIER      1u
#define SMP_BARRIER_SYNC 2u

/**
 * Have each other processor started in `cpus`, a bit for each, pass a full
 * memory barrier, and a serializing instruction as well with `sync_core`,
 * and wait until each has, for membarrier(2). One in user mode passes them
 * in the interrupt, and one in the kernel, which waits for the big lock,
 * while it waits. The caller holds the big lock.
 */
void smp_barrier(uint64_t cpus, int sync_core);

/**
 * Note that a kernel mapping changed, after the caller dropped its own
 * translation of it: every other processor drops all of its translations
 * before this returns (`smp_kernel_flush`).
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
 * wait, a second at most and 10 ms more for a processor sent an NMI, until
 * each has taken the stop, so that none is still writing the log or the
 * screen when the caller does. A processor sent the NMI takes the stop in
 * the NMI's handler (`watchdog_nmi`, `smp_stop_nmi`).
 */
void smp_stop_others(void);

/**
 * Take a stop `smp_stop_others` asked of this processor, from its NMI
 * handler: it leaves VMX or SVM operation (`k_panic_hook`), marks the
 * processor stopped and halts, and does not return when one was asked. 0
 * when none was, and the NMI is some other one.
 */
int smp_stop_nmi(void);

/** Make the boot processor's idle task and start the others the firmware
 * lists, as many as `maxcpus=` allows, the boot one counted; a panic when
 * the heap has no room for the boot processor's idle task. A processor
 * that does not start or come online is reported and left out. */
void smp_start(void);

#endif /* _SMP_H_ */
