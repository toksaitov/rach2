/* Timed wakes: interrupt delay, dispatch delay and the return to user mode. */
#include "waketime.h"
#include "process.h"
#include "sched.h"
#include "../rtc.h"
#include "../timer.h"
#include "../vdso.h"
#include "../lib/kstring.h"

/** Read the common microsecond clock with interrupts off, modulo 2^32. */
uint32_t waketime_clock(void)
{
    int64_t sec, ns;

    if (vclock_read(0, 0, &sec, &ns)) {
        return (uint32_t)sec * 1000000u + (uint32_t)ns / 1000u;
    }
    return sched_is_enabled() ? timer_now_us() : 0u;
}

/** Read `cpu`'s completed and current lock waits at `now`. Completed
 * waits change under the big lock. A new waiter publishes its start
 * before its flag; it cannot complete while this reader holds the lock. */
static uint32_t queue_lock(int cpu, uint32_t now)
{
    const struct percpu *c = &cpu_table[cpu];
    uint32_t us = c->wake_bkl_total;

    if (c->wake_bkl_waiting) {
        uint32_t elapsed = now - c->wake_bkl_start;

        if ((int32_t)elapsed > 0) {
            us += elapsed;
        }
    }
    return us;
}

/** Account the current stage through `now`, then begin the next at it. */
static void account(struct wake_record *w, uint32_t now)
{
    uint32_t elapsed = now - w->since;

    if (w->phase == 1) {
        uint32_t locked = queue_lock(w->cpu, now) - w->queue_bkl;

        w->part[WAKE_QUEUE] += elapsed;
        w->part[WAKE_QUEUE_BKL] += locked < elapsed ? locked : elapsed;
    } else if (w->phase == 2) {
        w->part[WAKE_RETURN] += elapsed;
    } else if (w->phase == 3) {
        w->part[WAKE_BLOCKED] += elapsed;
    } else if (w->phase == 4) {
        w->part[WAKE_TIMER] += elapsed;
    }
    w->since = now;
}

/** Start a blocked user thread's first timed wake at `due`, with the big
 * lock held and interrupts off; an unfinished wake keeps its first deadline.
 * Kernel threads have no user return at which to complete a record. */
void waketime_expire(process_t *p, uint64_t due)
{
    struct wake_record *w = &p->wake_time;
    struct percpu *cpu = this_cpu();
    uint64_t ticks;
    uint32_t ns, now, late, before_lock, before_irq;

    if (!p->mm || p->state != PROC_BLOCKED || w->phase) {
        return;
    }
    rtc_tick_phase(&ticks, &ns);
    now = waketime_clock();
    late = (uint32_t)(ticks - due) * TICK_US + ns / 1000u;
    before_lock = now - cpu->wake_irq_locked;
    before_irq = now - cpu->wake_irq_start;
    if (before_lock > late) {
        before_lock = late;
    }
    if (before_irq > late) {
        before_irq = late;
    }
    k_memset(w->part, 0, sizeof(w->part));
    w->part[WAKE_IRQ] = late - before_irq;
    w->part[WAKE_TIMER_BKL] = before_irq - before_lock;
    w->part[WAKE_TIMER] = before_lock;
    w->phase = 4;
    w->since = now;
}

/** Move an active wake to a queued, running or blocked stage under the big
 * lock, with interrupts off; `cpu` is the queue's CPU for phase 1. */
void waketime_state(process_t *p, unsigned int phase, int cpu)
{
    struct wake_record *w = &p->wake_time;
    uint32_t now;

    if (!w->phase) {
        return;
    }
    now = waketime_clock();
    account(w, now);
    w->phase = phase;
    w->cpu = cpu;
    if (phase == 1) {
        w->queue_bkl = queue_lock(cpu, now);
    }
}

/** Charge `waited` microseconds to the current running wake after taking
 * the big lock, with interrupts off. A switch may have begun this stage. */
void waketime_lock(uint32_t waited)
{
    process_t *p = current_proc;

    if (p && p->wake_time.phase == 2) {
        uint32_t elapsed = waketime_clock() - p->wake_time.since;

        p->wake_time.part[WAKE_RETURN_BKL] += waited < elapsed ? waited : elapsed;
    }
}

/** Finish the current thread's active wake before restoring user registers,
 * with the big lock held and interrupts off. */
void waketime_return(void)
{
    struct wake_record *w;
    uint64_t total = 0, worst = 0;
    int i;

    if (!current_proc || !current_proc->wake_time.phase) {
        return;
    }
    w = &current_proc->wake_time;
    account(w, waketime_clock());
    for (i = 0; i < WAKE_FIELDS; i++) {
        w->last[i] = w->part[i];
        w->sum[i] += w->part[i];
        if (i < WAKE_QUEUE_BKL) {
            total += w->part[i];
            worst += w->worst[i];
        }
    }
    if (total >= worst) {
        k_memcpy(w->worst, w->part, sizeof(w->worst));
    }
    w->count++;
    w->phase = 0;
}
