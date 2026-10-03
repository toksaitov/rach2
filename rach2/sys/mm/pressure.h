#ifndef _PRESSURE_H_
#define _PRESSURE_H_

#include "../lib/kdef.h"
#include "../lib/kint.h"

/*
 * pressure: what memory the caches may hold, what is kept back, and who
 * gives frames up when they run short.
 *
 * The page cache and the buffer cache together have a target of a quarter
 * of managed memory, half each, which is the most either grows to; they
 * take frames as they fill and give them back under pressure: when free
 * frames fall below the low mark, reclaim evicts clean cached pages and
 * buffers and, from a system call, writes dirty ones back, until the free
 * count is above the high mark or nothing is left to give. It runs before
 * the allocation that needed the room, in fork, exec, mmap, brk and a
 * user page fault; a fault enters with interrupts off and outside any
 * call, so reclaim there takes clean frames only. The allocators
 * themselves never sleep, and an allocation that finds nothing after
 * reclaim is refused by name.
 *
 * A reserve, a thirty-second of managed memory, is kept for the kernel's
 * own completion and writeback and for an administrator: a frame
 * taken on behalf of a process whose effective uid is not 0 is refused once
 * the free count is inside the reserve, so a user that has used everything
 * else finds its fork refused with ENOMEM while root can still log in and
 * act. Interrupt handlers and the kernel outside any process take from the
 * reserve. The size is what the pressure row found a login needs: a login
 * on the console is a fork by init and two programs loaded whole, which is
 * over a megabyte on a machine of 128, where a hundred-and-twenty-eighth
 * was a megabyte.
 */

/** Frames the caches together aim to hold at most while memory is short. */
size_t mem_cache_target(void);

/** Frames kept back for the kernel and root. */
size_t mem_reserve_frames(void);

/**
 * Whether a frame may be taken now: 1 unless the caller acts for a process
 * whose effective uid is not 0 and the free count is inside the reserve.
 * `user_page` says the frame is for a user page rather than a kernel
 * object; both are refused alike.
 */
int mem_may_take(int user_page);

/**
 * Give the caches' frames back while the free count is below the high mark;
 * dirty ones are written back, which sleeps, only from a system call with
 * interrupts on. Returns how many frames came back. `mem_balance` is the
 * caller that waits for the low mark before it asks.
 */
uint32_t mem_reclaim(void);

/** `mem_reclaim` if the free count is below the low mark; for the callers
 * about to allocate: fork, exec, mmap, brk and a user page fault. */
void mem_balance(void);

/** One level of interrupt entered or left; `in_interrupt` tells. */
void irq_enter(void);
void irq_leave(void);
int in_interrupt(void);

#endif /* _PRESSURE_H_ */
