/*
 * suspend.c: suspend to idle, as suspend.h describes it.
 */

#include "suspend.h"

#include "process.h"
#include "wait_queue.h"
#include "../klog.h"
#include "../lib/kerrno.h"

/* The process that suspended, NULL while none has; and the wake events
 * counted, which a suspend waits for the count to move past. */
static struct process *suspender;
static volatile uint32_t wakes;
static wait_queue_t suspend_wq;
static int suspend_ready;

/** Whether a wake has come since the count `arg` points at was taken. */
static int woken(void *arg)
{
    return wakes != *(const uint32_t *)arg;
}

int suspend_to_idle(void)
{
    uint32_t from;
    int rc;

    if (suspender) {
        return -EBUSY;
    }
    if (!suspend_ready) {
        wait_queue_init(&suspend_wq);
        suspend_ready = 1;
    }
    from = wakes;
    suspender = current_proc;
    klog(KLOG_INFO, "suspend", "freeze: the processes are held until a wake event");
    rc = wq_wait_event(&suspend_wq, woken, &from);
    suspender = NULL;
    klog(KLOG_INFO, "suspend", "freeze: %s", rc < 0 ? "ended by a signal" : "woken");
    return rc < 0 ? -EINTR : 0;
}

int suspend_lets_run(const struct process *p)
{
    return !suspender || p == suspender;
}

void suspend_wake(void)
{
    if (suspender) {
        wakes++;
        wq_wake_all(&suspend_wq);
    }
}
