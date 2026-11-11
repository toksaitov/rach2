/*
 * suspend.c: suspend to idle, as suspend.h describes it.
 */

#include "suspend.h"

#include "process.h"
#include "sched.h"
#include "wait_queue.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../network/driver/ath9k/ath9k.h"

/* The process that suspended, NULL while none has; the wake events
 * counted, which a suspend waits for the count to move past; and the queue
 * the suspender sleeps on, made at the first suspend. */
static struct process *suspender;
static volatile uint32_t wakes;
static wait_queue_t suspend_wq;
static int suspend_ready;
static int suspend_preparing;

/** Whether a wake has come since the count `arg` points at was taken. */
static int woken(void *arg)
{
    return wakes != *(const uint32_t *)arg;
}

/** Stop the card, freeze processes until a wake or signal, and restart it.
 * A preparation or freeze already in progress returns -EBUSY. A signal
 * returns -EINTR after resume; a radio resume error is logged separately. */
int suspend_to_idle(void)
{
    uint32_t from;
    int rc, resume_rc;

    if (suspender || suspend_preparing) {
        return -EBUSY;
    }
    if (!suspend_ready) {
        wait_queue_init(&suspend_wq);
        suspend_ready = 1;
    }
    from = wakes;
    suspend_preparing = 1;
    ath9k_suspend_all();
    suspender = current_proc;
    sched_hold_waits(1);
    suspend_preparing = 0;
    klog(KLOG_INFO, "suspend", "freeze: the processes are held until a wake event");
    rc = wq_wait_event(&suspend_wq, woken, &from);
    suspend_preparing = 1;
    suspender = NULL;
    sched_hold_waits(0);
    resume_rc = ath9k_resume_all();
    suspend_preparing = 0;
    if (resume_rc)
        klog(KLOG_ERR, "ath9k", "resume: failed: %d", resume_rc);
    klog(KLOG_INFO, "suspend", "freeze: %s", rc < 0 ? "ended by a signal" : "woken");
    return rc < 0 ? -EINTR : 0;
}

int suspend_lets_run(const struct process *p)
{
    return !suspender || p == suspender;
}

void suspend_wake(void)
{
    if (suspender || suspend_preparing) {
        wakes++;
        wq_wake_all(&suspend_wq);
    }
}
