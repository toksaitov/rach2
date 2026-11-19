/*
 * khugepaged.c: the kernel thread that collapses the small pages of
 * private anonymous memory into transparent huge pages, as Linux 6.12's
 * mm/khugepaged.c runs it, and its start at boot (thp.h).
 */

#include "thp.h"
#include "mm.h"
#include "paging.h"

#include "../klog.h"
#include "../lib/kirq.h"
#include "../proc/process.h"
#include "../proc/sched.h"
#include "../proc/smp.h"
#include "../proc/wait_queue.h"
#include "../timer.h"

static wait_queue_t khugepaged_wait;
/* Moved by each change of the policy, which ends khugepaged's sleep. */
static volatile uint32_t _policy_gen;

void thp_policy_changed(void)
{
    _policy_gen++;
    wq_wake_all(&khugepaged_wait);
}

#if MMU_HUGE
/* The number of khugepaged's pass over the spaces, never 0, which a new
 * space's `thp_pass` holds; under the big lock. */
static uint32_t _pass = 1;

/* khugepaged's sleep: the policy's count when it began and the tick it
 * ends at, 0 for none. */
struct khugepaged_arg {
    uint32_t gen;
    uint64_t due;
};

/** Whether khugepaged's sleep, its `struct khugepaged_arg`, is over. */
static int khugepaged_due(void *arg)
{
    const struct khugepaged_arg *a = (const struct khugepaged_arg *)arg;

    return _policy_gen != a->gen || (a->due != 0 && tick_count() >= a->due);
}

/** Whether khugepaged has work: `enabled` is always or madvise, as Linux's
 * hugepage_pmd_enabled asks. */
static int khugepaged_on(void)
{
    return thp_policy.enabled != THP_NEVER;
}

/** The first address space on the process list that this pass has not
 * finished, with a user's count taken, or NULL when none is left. Under
 * the big lock. */
static struct mm *khugepaged_next(void)
{
    process_t *p;

    for (p = proc_list; p; p = p->next) {
        struct mm *mm = p->mm;

        if (mm && mm->dir && mm->thp_pass != _pass && mm_get_live(mm)) {
            return mm;
        }
    }
    return NULL;
}

/**
 * One wake of khugepaged, Linux's khugepaged_do_scan: scan the address
 * spaces in the order of the process list for `pages_to_scan` pages,
 * going on from where the last wake stopped, and begin a new pass
 * (`full_scans`) when every space is finished. A space whose regions'
 * lock is held is passed over for this pass, as Linux's
 * khugepaged_scan_mm_slot passes over one whose lock it cannot try. 1 when
 * no huge page could be had, or 0.
 */
static int khugepaged_wake(void)
{
    uint32_t budget = thp_policy.pages_to_scan, done;
    int nomem = 0;
    struct mm *mm;

    while (budget > 0 && !nomem) {
        bkl_enter();
        mm = khugepaged_next();
        if (!mm) {
            _pass = _pass + 1 ? _pass + 1 : 1;
            bkl_exit();
            thp_note(THP_FULL_SCANS);
            break;
        }
        bkl_exit();
        if (mmap_write_trylock(mm)) {
            done = thp_scan_mm(mm, budget, _pass, &nomem);
            mmap_unlock(mm);
        } else { /* busy: passed over until the next pass, as Linux's trylock */
            mm->thp_pass = _pass;
            mm->thp_next = 0;
            done = 1;
        }
        bkl_enter();
        mm_put(mm);
        bkl_exit();
        budget = done >= budget ? 0 : budget - done;
    }
    return nomem;
}

/**
 * khugepaged, mm/khugepaged.c:2567: while `enabled` is not never, scan
 * every `scan_sleep_millisecs`, or `alloc_sleep_millisecs` after no huge
 * page could be had, and otherwise sleep until the policy changes, which
 * also ends a sleep at once.
 */
static void khugepaged_main(void)
{
    struct khugepaged_arg arg;
    uint32_t flags, ms;
    int nomem;

    arch_irq_enable();
    for (;;) {
        nomem = khugepaged_on() ? khugepaged_wake() : 0;
        ms = nomem ? thp_policy.alloc_sleep_ms : thp_policy.scan_sleep_ms;
        flags = kirq_save();
        arg.gen = _policy_gen;
        arg.due = khugepaged_on() ? tick_count() + ((uint64_t)ms * TICK_HZ + 999u) / 1000u : 0;
        current_proc->wakeup_tick = arg.due;
        kirq_restore(flags);
        (void)wq_wait_event_idle(&khugepaged_wait, khugepaged_due, &arg);
        current_proc->wakeup_tick = 0;
    }
}

/** A kernel thread named `name` that begins at `entry`, ready, as
 * watchdog.c starts its own; NULL when no record or stack can be had. */
static process_t *khugepaged_start(const char *name, void (*entry)(void))
{
    process_t *p;
    uintptr_t base;
    uint32_t flags = kirq_save();

    base = kstack_alloc();
    p = base ? alloc_process() : NULL;
    if (!p) {
        if (base) {
            kstack_free(base + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        return NULL;
    }
    p->kernel_stack = base + KERNEL_STACK_SIZE;
    p->space_root = 0;
    p->page_dir = NULL;
    p->parent_pid = 0;
    proc_set_comm(p, name);
    setup_process_stack(p, entry);
    sched_make_ready(p);
    kirq_restore(flags);
    return p;
}
#endif

void thp_init(void)
{
    paging_thp_init();
#if MMU_HUGE
    if (!khugepaged_start("khugepaged", khugepaged_main)) {
        klog(KLOG_WARN, "thp", "khugepaged: no thread");
    }
#endif
}
