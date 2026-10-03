/*
 * pressure.c: the targets, the reserve and reclaim, as pressure.h says.
 */

#include "../lib/kirq.h"
#include "pressure.h"

#include "../fs/bcache.h"
#include "../klog.h"
#include "../proc/process.h"
#include "filecache.h"
#include "pagecache.h"
#include "paging.h"

/* The interrupt depth while no process is current, as at boot. While one is,
 * the depth is kept in its record instead: a handler that switches to another
 * process is left only when the process it interrupted runs again, so the
 * count belongs to that process. */
static volatile uint32_t irq_depth;

void irq_enter(void)
{
    if (current_proc) {
        current_proc->irq_depth++;
    } else {
        irq_depth++;
    }
}

void irq_leave(void)
{
    if (current_proc) {
        current_proc->irq_depth--;
    } else {
        irq_depth--;
    }
}

int in_interrupt(void)
{
    return current_proc ? current_proc->irq_depth > 0 : irq_depth > 0;
}

size_t mem_cache_target(void)
{
    return frame_total_count() / 4u;
}

size_t mem_reserve_frames(void)
{
    return frame_total_count() / 32u;
}

/**
 * The free-frame count below which `mem_balance` starts reclaim: a sixteenth
 * of managed memory.
 *
 * Reclaim stops at `high_mark`, an eighth, so it runs in bursts and not on
 * every allocation.
 */
static size_t low_mark(void)
{
    return frame_total_count() / 16u;
}

/**
 * The free-frame count at which `mem_reclaim` stops: an eighth of managed
 * memory.
 */
static size_t high_mark(void)
{
    return frame_total_count() / 8u;
}

/**
 * Whether a frame may be taken now.
 *
 * When the free count is at or below the reserve, a frame is refused to a
 * process whose effective uid is not 0. An interrupt handler, the kernel
 * outside any process, root, and a process that is reclaiming may still take
 * one: the writeback a reclaim starts needs requests, and the reserve is kept
 * for them. `user_page` is not consulted, so a user page and a kernel object
 * are refused alike.
 */
int mem_may_take(int user_page)
{
    (void)user_page;
    if (in_interrupt() || !current_proc || proc_privileged(current_proc) ||
        current_proc->reclaiming) {
        return 1;
    }
    return frame_free_count() > mem_reserve_frames();
}

/**
 * Whether reclaim may write dirty pages back from here: inside a system call
 * with interrupts on, where a writeback may sleep.
 *
 * A page fault enters with interrupts off and outside any call, and
 * `block_request` panics when a device that sleeps its caller is reached
 * there.
 */
static int may_write_back(void)
{
    return kirq_enabled() && proc_in_syscall();
}

/**
 * Evict cached frames until the free count reaches the high mark, or until
 * the caches give nothing back. Returns the number of frames freed.
 *
 * The buffer cache is asked first and the page cache for the rest. Dirty
 * frames are written back only where `may_write_back` allows it. The calling
 * process is marked as reclaiming meanwhile, so the requests a writeback
 * needs come from the reserve even when the caller itself may not take from
 * it: the writeback is what the reserve is kept for.
 */
uint32_t mem_reclaim(void)
{
    size_t before = frame_free_count();
    uint32_t got = 0, written = 0;
    int can_write = may_write_back();

    if (current_proc) {
        current_proc->reclaiming++;
    }
    while (frame_free_count() < high_mark()) {
        size_t short_by = high_mark() - frame_free_count();
        uint32_t want = (uint32_t)short_by == short_by ? (uint32_t)short_by : 0xFFFFFFFFu;
        uint32_t freed = bcache_shrink(want, can_write, &written);

        if (freed < want) {
            freed += filecache_shrink(want - freed);
        }
        if (freed < want) {
            freed += pc_shrink(want - freed, can_write, &written);
        }
        if (freed == 0) {
            break;
        }
        got += freed;
    }
    if (current_proc) {
        current_proc->reclaiming--;
    }
    if (got) {
        klog(KLOG_INFO, "memory",
             "reclaim: %d frames from the caches, %d written back, %ld free before", (int)got,
             (int)written, (long)before);
    }
    return got;
}

void mem_balance(void)
{
    if (in_interrupt() || frame_free_count() >= low_mark()) {
        return;
    }
    mem_reclaim();
}
