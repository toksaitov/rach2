/* Read-copy update: readers without locks, and frees after a grace period. */
#ifndef _RCU_H_
#define _RCU_H_

#include "kdef.h"
#include "kint.h"

/*
 * A minimal RCU after Linux's Tree RCU without CONFIG_PREEMPT_RCU, reduced
 * to what Rach2 needs (rcu.Readme.md). A reader walks a structure that an
 * updater may change under it, inside rcu_read_lock and rcu_read_unlock; the
 * updater publishes the new version, then frees the old one only after a
 * grace period, once every processor has passed a quiescent state, a point
 * at which it can be in no read section: a call of `schedule`, a service
 * point of a long loop, an idle wait, or a tick that finds it in user mode.
 *
 * A thread in the kernel is switched only where it sleeps or reaches a
 * service point, never by an interrupt, as a Linux kernel without
 * CONFIG_PREEMPTION, so a read section is a section in which preemption is
 * off already: rcu_read_lock and rcu_read_unlock stop the compiler from
 * moving the reader's accesses out of it and emit nothing. A reader must not
 * sleep, reach a service point or enter an idle wait inside one.
 */
#define rcu_read_lock()   k_barrier()
#define rcu_read_unlock() k_barrier()

/*
 * The record a structure freed after a grace period embeds, Linux's
 * `struct rcu_head`: `call_rcu` links it on its processor's list, and the
 * grace-period thread calls `func` with it once a grace period that began
 * after the call has ended.
 */
struct rcu_head {
    struct rcu_head *next;
    void (*func)(struct rcu_head *head);
};

/**
 * Call `func(head)` once a grace period has passed after this call, in the
 * grace-period thread: in process context, without the big lock, and with
 * no other lock held; `func` must not sleep. Callbacks queued on one
 * processor run in the order they were queued. May be called with
 * interrupts off, from an interrupt, and under any spinlock that the
 * scheduler's locks are taken under; it allocates nothing.
 */
void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *head));

/**
 * Wait until a grace period has passed: every read section that had begun
 * on another processor when the call began has ended. Sleeps; the caller
 * holds no spinlock and is in no read section. Returns at once while one
 * processor alone is online, where the caller's own quiescent state is the
 * grace period, as Linux's `rcu_blocking_is_gp`.
 */
void synchronize_rcu(void);

/** A quiescent state of the running processor: from `schedule` and from a
 * service point, with interrupts off. */
void rcu_qs(void);

/** The tick of the running processor, `user` when it came in user mode,
 * which is then a quiescent state, as Linux's `rcu_sched_clock_irq`. */
void rcu_tick(int user);

/** The idle task's halt begins and ends on the running processor, with
 * interrupts off: the processor is in an extended quiescent state between
 * the two, through which no grace period waits for it, though its tick may
 * be stopped. */
void rcu_idle_enter(void);
void rcu_idle_exit(void);

/** Set up the processors' lists and start the grace-period thread, once
 * the scheduler runs. A callback queued before waits for it. */
void rcu_init(void);

/*
 * The steps of the grace-period thread's loop, which the host tests drive
 * themselves: `rcu_gp_begin` moves the callbacks queued on every processor
 * to its list of the grace period it begins and records each online
 * processor's quiescent-state count, 0 when nothing was queued and no grace
 * period began; `rcu_gp_ended` says whether every processor that was online
 * at the beginning has since passed a quiescent state, been idle or gone
 * offline; `rcu_gp_finish` ends the grace period and runs its callbacks,
 * each processor's in their order.
 */
int rcu_gp_begin(void);
int rcu_gp_ended(void);
void rcu_gp_finish(void);

/** The grace periods ended since the boot. */
uint32_t rcu_gp_completed(void);

#endif /* _RCU_H_ */
