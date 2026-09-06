/*
 * pressure.c: the targets, the reserve and reclaim, as pressure.h says.
 */

#include "pressure.h"

#include "../fs/bcache.h"
#include "../klog.h"
#include "../proc/process.h"
#include "pagecache.h"
#include "paging.h"

/* The depth outside any process, at boot; inside one it is the record's,
 * since a handler that switches to another process is left by the one it
 * interrupted, when that one runs again. */
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

uint32_t mem_cache_target(void)
{
    return frame_total_count() / 4u;
}

uint32_t mem_reserve_frames(void)
{
    return frame_total_count() / 32u;
}

/* The low mark starts reclaim and the high mark ends it: a sixteenth and an
 * eighth of managed memory, so that reclaim runs in bursts and not on every
 * allocation. */
static uint32_t low_mark(void)
{
    return frame_total_count() / 16u;
}

static uint32_t high_mark(void)
{
    return frame_total_count() / 8u;
}

int mem_may_take(int user_page)
{
    (void)user_page;
    if (in_interrupt() || !current_proc || current_proc->euid == 0 ||
        current_proc->reclaiming) {
        return 1;
    }
    return frame_free_count() > mem_reserve_frames();
}

/* Whether this context may sleep in a write: inside a system call with
 * interrupts on. A page fault enters with them off and outside any call,
 * and a writeback there would stop on the request layer's check. */
static int may_write_back(void)
{
    uint32_t eflags;

    __asm__ __volatile__("pushfl; popl %0" : "=r"(eflags));
    return (eflags & 0x200u) != 0 && proc_in_syscall();
}

uint32_t mem_reclaim(void)
{
    uint32_t before = frame_free_count();
    uint32_t got = 0, written = 0;
    int can_write = may_write_back();

    /* The requests a writeback needs come from the reserve when the caller
     * itself may not take from it: the writeback is the reserve's purpose. */
    if (current_proc) {
        current_proc->reclaiming++;
    }
    while (frame_free_count() < high_mark()) {
        uint32_t want = high_mark() - frame_free_count();
        uint32_t freed = bcache_shrink(want, can_write, &written);

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
        klog(KLOG_INFO, "memory", "reclaim: %d frames from the caches, %d written back, %d free before",
             (int)got, (int)written, (int)before);
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
