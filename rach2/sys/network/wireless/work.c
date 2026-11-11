/* The wireless worker: one kernel thread that runs the 802.11 work in order
 * of its due ticks, so that none of it runs in an interrupt handler. */
#include "work.h"
#include "../../lib/kerrno.h"
#include "../../lib/spinlock.h"
#include "../../timer.h"
#ifndef RACH2_HOSTTEST
#include "../../klog.h"
#include "../../arch/arch.h"
#include "../../lib/kirq.h"
#include "../../proc/percpu.h"
#include "../../proc/process.h"
#include "../../proc/sched.h"
#include "../../proc/wait_queue.h"
#endif

/* Pending work in order of due tick, equal ticks in the order queued.
 * Immediate work is due at the tick it was queued, so that it goes behind a
 * timer already overdue instead of ahead of every timer. */
static struct wlan_work *queue;
/* The work whose callback runs now, compared and never dereferenced, since
 * its owner may reuse it once the callback returns. */
static struct wlan_work *running;
static struct spinlock queue_lock = SPINLOCK_INIT_NAMED("wlan_queue_lock");

#ifndef RACH2_HOSTTEST
static process_t *thread;
/* Static storage starts them empty, so work queued before the thread exists
 * can wake them. */
static wait_queue_t work_wq; /* the thread sleeps here */
static wait_queue_t idle_wq; /* a cancel waits here for a callback's end */
/* Whether the first program has its pid, after which the thread may take
 * one; and whether a radio asked for the thread before then. */
static int threads_ready;
static int thread_wanted;
#endif

/** Wake the thread to look at the queue again; IRQ-safe. */
static void work_kick(void)
{
#ifndef RACH2_HOSTTEST
    wq_wake_all(&work_wq);
#endif
}

int wlan_queue_work(struct wlan_work *work, uint64_t due)
{
    struct wlan_work **link;
    uint32_t flags;

    if (!work || !work->run) {
        return -EINVAL;
    }
    flags = spin_lock_irqsave(&queue_lock);
    if (work->pending || work == running) {
        spin_unlock_irqrestore(&queue_lock, flags);
        return -EBUSY;
    }
    if (!due)
        due = tick_count();
    work->due = due;
    work->pending = 1;
    for (link = &queue; *link && (*link)->due <= due; link = &(*link)->next) {
    }
    work->next = *link;
    *link = work;
    spin_unlock_irqrestore(&queue_lock, flags);
    work_kick();
    return 0;
}

/** Take `work` off the queue if it is there; the queue lock is held. */
static void work_unlink(struct wlan_work *work)
{
    struct wlan_work **link;

    for (link = &queue; *link; link = &(*link)->next) {
        if (*link == work) {
            *link = work->next;
            work->next = 0;
            work->pending = 0;
            return;
        }
    }
}

#ifndef RACH2_HOSTTEST
/** Whether the callback of the work `arg` has ended. */
static int work_not_running(void *arg)
{
    return running != (struct wlan_work *)arg;
}
#endif

void wlan_cancel_work(struct wlan_work *work)
{
    uint32_t flags;

    if (!work) {
        return;
    }
    flags = spin_lock_irqsave(&queue_lock);
    if (work->pending) {
        work_unlink(work);
    }
    spin_unlock_irqrestore(&queue_lock, flags);
#ifndef RACH2_HOSTTEST
    /* The timer switches the thread away inside a callback; once scheduling
       is off it never runs again, and a wait for its end would not end. */
    if (current_proc != thread && sched_is_enabled()) {
        (void)wq_wait_event_nointr(&idle_wq, work_not_running, work);
    }
#endif
}

uint64_t wlan_work_next_due(void)
{
    uint64_t due;
    uint32_t flags;

    flags = spin_lock_irqsave(&queue_lock);
    due = queue ? (queue->due ? queue->due : 1) : 0;
    spin_unlock_irqrestore(&queue_lock, flags);
    return due;
}

unsigned wlan_work_run_due(uint64_t now)
{
    struct wlan_work *work;
    unsigned ran = 0;
    uint32_t flags;
    uint64_t tick;

    for (;;) {
        /* Immediate work queued by a callback is due at the tick then. */
        tick = tick_count();
        if (tick > now)
            now = tick;
        flags = spin_lock_irqsave(&queue_lock);
        work = queue;
        if (!work || work->due > now) {
            spin_unlock_irqrestore(&queue_lock, flags);
            return ran;
        }
        queue = work->next;
        work->next = 0;
        work->pending = 0;
        running = work;
        spin_unlock_irqrestore(&queue_lock, flags);

        work->run(work);
        ran++;

        flags = spin_lock_irqsave(&queue_lock);
        running = 0;
        spin_unlock_irqrestore(&queue_lock, flags);
#ifndef RACH2_HOSTTEST
        wq_wake_all(&idle_wq);
#endif
    }
}

#ifndef RACH2_HOSTTEST
/** Whether the thread must wake: the earliest due tick is no longer the one
 * it armed in `*arg`, or that tick has come. True once the deadline has
 * passed, as the deadline wait requires. */
static int work_ready(void *arg)
{
    uint64_t armed = *(const uint64_t *)arg;
    uint64_t due = wlan_work_next_due();

    return due != armed || (due != 0 && due <= tick_count());
}

/** The thread, kwlan: sleep until the earliest work is due or new work
 * comes, then run what is due, holding the big lock as kernel code does. */
static void work_thread(void)
{
    uint64_t armed;

    arch_irq_enable();
    for (;;) {
        armed = wlan_work_next_due();
        current_proc->wakeup_tick = armed;
        (void)wq_wait_event_idle(&work_wq, work_ready, &armed);
        current_proc->wakeup_tick = 0;
        (void)wlan_work_run_due(tick_count());
    }
}

int wlan_work_start(void)
{
    process_t *p;
    uintptr_t base;
    uint32_t flags;

    if (thread) {
        return 0;
    }
    if (!threads_ready) {
        /* Init must be pid 1, which BusyBox's reboot and poweroff signal. */
        thread_wanted = 1;
        return 0;
    }
    flags = kirq_save();
    base = kstack_alloc();
    p = base ? alloc_process() : 0;
    if (!p) {
        if (base) {
            kstack_free(base + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        klog(KLOG_ERR, "wlan", "worker: refused: no record or stack for it");
        return -ENOMEM;
    }
    p->kernel_stack = base + KERNEL_STACK_SIZE;
    p->space_root = 0;
    p->page_dir = 0;
    p->parent_pid = 0;
    proc_set_comm(p, "kwlan");
    setup_process_stack(p, work_thread);
    thread = p;
    sched_make_ready(p);
    kirq_restore(flags);
    klog(KLOG_DEBUG, "wlan", "worker: kwlan is pid %d", (int)p->pid);
    return 0;
}

void wlan_work_threads_start(void)
{
    threads_ready = 1;
    if (thread_wanted) {
        (void)wlan_work_start();
    }
}
#endif
