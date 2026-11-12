/* Timed wakes: interrupt delay, dispatch delay and the return to user mode. */
#include "waketime.h"
#include "process.h"
#include "sched.h"
#include "../rtc.h"
#include "../timer.h"
#include "../lib/kstring.h"

/**
 * Read the monotonic clock in microseconds, modulo 2^32, or 0 before the
 * scheduler runs, when no wake is timed. It is the one continuous clock
 * (`rtc_monotonic`): the vDSO's clock while that runs and the tick's
 * after, held to the last reading, so that a stage that starts on one and
 * ends on the other, or across a slew, is measured on one scale. Takes
 * `rtc_mono_lock`, a leaf, with interrupts off.
 */
uint32_t waketime_clock(void)
{
    int64_t sec, ns;

    if (!sched_is_enabled()) {
        return 0u;
    }
    rtc_monotonic(&sec, &ns);
    return (uint32_t)sec * 1000000u + (uint32_t)ns / 1000u;
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

/** Whether `p` starts a timed wake record: a blocked user thread with no
 * wake unfinished. Kernel threads have no user return at which to complete
 * a record. */
static int wake_starts(const process_t *p)
{
    return p->mm && p->state == PROC_BLOCKED && !p->wake_time.phase;
}

/** Start `p`'s timed wake `late` microseconds after its deadline, split by
 * this processor's last timer interrupt into the interrupt's delay, its wait
 * for the big lock and the timer's work since (`_irq_router`). */
static void wake_start(process_t *p, uint32_t late)
{
    struct wake_record *w = &p->wake_time;
    struct percpu *cpu = this_cpu();
    uint32_t now = waketime_clock();
    uint32_t before_lock = now - cpu->wake_irq_locked;
    uint32_t before_irq = now - cpu->wake_irq_start;

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
    proc_work_set(p, WORK_WAKETIME);
}

/** Start a blocked user thread's first timed wake at `due`, under the run
 * queues' lock with interrupts off, so that a wake from elsewhere does not
 * move the stage between the test and the start; an unfinished wake keeps
 * its first deadline. A tick not yet counted, as at a deadline the clock
 * reached first, is no delay. */
void waketime_expire(process_t *p, uint64_t due)
{
    uint64_t ticks;
    uint32_t ns;

    if (!wake_starts(p)) {
        return;
    }
    rtc_tick_phase(&ticks, &ns);
    wake_start(p, ticks >= due ? (uint32_t)(ticks - due) * TICK_US + ns / 1000u : 0u);
}

/**
 * `ns` in whole microseconds, or all ones when that is past 32 bits, by
 * 32-bit divisions in 16-bit steps, since a 64-bit division calls libgcc on
 * i686.
 */
static uint32_t ns_to_us(uint64_t ns)
{
    uint32_t hi = (uint32_t)(ns >> 32), lo = (uint32_t)ns, mid, low;

    if (hi >= 1000u) {
        return 0xFFFFFFFFu;
    }
    mid = (hi << 16) | (lo >> 16);
    low = ((mid % 1000u) << 16) | (lo & 0xFFFFu);
    return ((mid / 1000u) << 16) | (low / 1000u);
}

/** The delay is converted to microseconds whole, and one past 2^32 of them
 * counts as that. */
void waketime_expire_late(process_t *p, int64_t late_ns)
{
    if (!wake_starts(p)) {
        return;
    }
    wake_start(p, late_ns < 0 ? 0u : ns_to_us((uint64_t)late_ns));
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
