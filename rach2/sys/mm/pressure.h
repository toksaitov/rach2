/*
 * pressure: what memory the caches may hold, what is kept back, and who
 * gives frames up when they run short.
 *
 * The page cache and the buffer cache together have a target of a quarter
 * of managed memory, half each, which is the most either grows to; they
 * take frames as they fill and give them back under pressure: when free
 * frames fall below the low mark, reclaim evicts clean cached pages and
 * buffers and, from a system call, writes dirty ones back, then asks the
 * shrinkers drivers registered, until the free count is above the high
 * mark or nothing is left to give. It runs before
 * the allocation that needed the room: in fork, exec, mmap and brk, and
 * before a user page is backed for a fault or a copy. A fault enters with
 * interrupts off and outside any call, so reclaim there takes clean frames
 * only. The allocators themselves never sleep, and an allocation that finds
 * nothing after reclaim is refused by name.
 *
 * A reserve, a thirty-second of managed memory, is kept for the kernel's
 * own completion and writeback and for an administrator: a frame taken on
 * behalf of a process without privilege (`proc_privileged`) is refused once
 * the free count is inside the reserve, so a user that has used everything
 * else finds its fork refused with ENOMEM while root can still log in and
 * act. Interrupt handlers and the kernel outside any process take from the
 * reserve. The size is enough for a login: on the console that is a fork by
 * init and the pages two programs touch, and a machine of 128 MiB keeps
 * back 4 MiB.
 */
#ifndef _PRESSURE_H_
#define _PRESSURE_H_

#include "../lib/kdef.h"
#include "../lib/kint.h"

/** Frames the caches together aim to hold at most while memory is short, a
 * quarter of what the frame allocator manages; the buffer cache and the
 * page cache are each given half. */
size_t mem_cache_target(void);

/** Frames kept back for the kernel and root, a thirty-second of what the
 * frame allocator manages. */
size_t mem_reserve_frames(void);

/**
 * Whether a frame may be taken now: 1 unless the free count is at or below
 * the reserve and the caller acts for a process without privilege that is
 * not reclaiming. An interrupt handler and the kernel outside any process
 * may always take one. `user_page` says the frame is for a user page rather
 * than a kernel object; both are refused alike.
 */
int mem_may_take(int user_page);

/*
 * A driver's memory that reclaim may take back, as Linux's struct shrinker:
 * a GPU driver's purgeable buffers, whose contents the program said it can
 * lose. `shrink` frees up to `want` frames of it and returns how many it
 * freed; `may_sleep` is 1 only from a system call with interrupts on, and
 * with 0 it must not sleep, since reclaim also runs inside a page fault.
 * The record is the driver's, embedded in its own, and is called under the
 * big kernel lock.
 */
struct mem_shrinker {
    uint32_t (*shrink)(struct mem_shrinker *s, uint32_t want, int may_sleep);
    struct mem_shrinker *next; /* the next registered, kept by pressure.c */
};

/** Register `s`, which reclaim then asks after the caches, the later
 * registered first; `s` must not be registered already. */
void mem_shrinker_register(struct mem_shrinker *s);

/** Take `s` off the list; nothing for one that is not on it. */
void mem_shrinker_unregister(struct mem_shrinker *s);

/**
 * Give the caches' frames back while the free count is below the high mark,
 * an eighth of managed memory, or until they give nothing: the buffer
 * cache's first, then the file cache's, then the page cache's, then each
 * registered shrinker's. Dirty ones are written back, which sleeps, only
 * from a system call with interrupts on. Returns how many frames came back.
 * `mem_balance` calls it only below the low mark.
 */
uint32_t mem_reclaim(void);

/** `mem_reclaim` if the free count is below the low mark, a sixteenth of
 * managed memory, and the caller is not an interrupt handler; for the
 * callers about to allocate: fork, exec, mmap, brk and the backing of a
 * user page. */
void mem_balance(void);

/** Whether `mem_balance` would reclaim now: the free count is below the
 * low mark and the caller is not an interrupt handler. A fault asks first,
 * so that it lets its regions' lock go only when the caches will run. */
int mem_short(void);

/** Count one level of interrupt entered, for `in_interrupt`. */
void irq_enter(void);

/** Count one level of interrupt left. */
void irq_leave(void);

/** Whether the processor is in an interrupt handler, by the depth
 * `irq_enter` and `irq_leave` keep: the current process's while one is
 * current, and the kernel's own while none is. */
int in_interrupt(void);

#endif /* _PRESSURE_H_ */
