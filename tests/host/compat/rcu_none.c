/* Read-copy update for a host test of one thread (lib/rcu.h): no other
 * processor reads, so every grace period has passed already, and
 * `call_rcu` runs its callback at once. The quiescent states the scheduler
 * reports are not counted. */
#include "../../../rach2/sys/lib/rcu.h"

/** Run `func(head)` now: no reader on another processor can hold it. */
void call_rcu(struct rcu_head *head, void (*func)(struct rcu_head *head))
{
    head->next = 0;
    head->func = func;
    func(head);
}

/** Nothing to wait for. */
void synchronize_rcu(void)
{}

/** Not counted. */
void rcu_qs(void)
{}

/** Not counted. */
void rcu_tick(int user)
{
    (void)user;
}

/** Not counted. */
void rcu_idle_enter(void)
{}

/** Not counted. */
void rcu_idle_exit(void)
{}
