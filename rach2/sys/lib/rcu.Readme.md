# Read-copy update

`rcu.c` is a minimal read-copy update after Linux 6.12's Tree RCU without `CONFIG_PREEMPT_RCU`, reduced to what Rach2 needs. A reader walks a structure that an updater may replace under it without taking a lock; the updater publishes the new version and frees the old one only after a grace period, once every processor has passed a quiescent state, a point at which it can be in no read section. Its first user is the descriptor table's lookup, whose maps of held descriptors a growth replaces and frees with `call_rcu` ([fs Readme](../fs/Readme.md)).

## Interfaces

* `rcu_read_lock()` and `rcu_read_unlock()` bracket a read section. They emit no instruction and stop the compiler from moving the reader's accesses out of the section. A read section must not sleep, reach a service point (`sched_service_point`) or enter an idle wait.
* `call_rcu(head, func)` calls `func(head)` once a grace period that began after the call has ended. The caller embeds `struct rcu_head` in what it frees. The callback runs in the grace-period thread, `rcu_sched`, in process context, without the big lock and with no other lock held, and must not sleep. Callbacks queued on one processor run in their order. `call_rcu` allocates nothing and may be called with interrupts off, from an interrupt, and under any spinlock that the scheduler's locks are taken under, since it wakes the thread.
* `synchronize_rcu()` sleeps until a grace period has passed, by a callback that wakes it, as Linux's `wait_rcu_gp`. While one processor alone is online it returns at once, as Linux's `rcu_blocking_is_gp`: the caller is in no read section, and no other processor runs one.
* The scheduler, the idle task and the tick report the quiescent states: `rcu_qs` from each call of `schedule` and from each service point, `rcu_tick(user)` from every processor's tick, a quiescent state when the tick came in user mode, as Linux's `rcu_sched_clock_irq`, and `rcu_idle_enter` and `rcu_idle_exit` around the idle task's halt.

## Design

A thread in the kernel is switched only where it sleeps or reaches a service point, never by an interrupt, as a Linux kernel without `CONFIG_PREEMPTION` is. A read section is therefore a section with preemption off already, and a read section that does not sleep ends before its processor's next quiescent state.

Each processor has a count of its quiescent states, alone in 64 bytes so that no two processors' counts share a cache line. The processor adds 2 at each quiescent state and 1 as its idle task's halt begins and again as it ends, so the count is odd while the processor is idle, as Linux's `dynticks` counter marks an extended quiescent state. Only the processor writes its count, with interrupts off, by a locked exchange, a full barrier on x86: the reads of the read sections before the exchange are done before it, and those of the sections after it cannot pass it.

Each processor has two lists of callbacks under a spinlock of its own: `next`, queued since the grace period under way began, and `wait`, the grace period's own, as Linux's `RCU_NEXT_TAIL` and `RCU_WAIT_TAIL` segments. `call_rcu` appends to its processor's `next` and wakes the thread.

The thread, Linux's `rcu_gp_kthread`, sleeps until some processor's `next` is not empty. It then begins a grace period (`rcu_gp_begin`): it moves each processor's `next` to its `wait` under that processor's lock, takes a full barrier, and reads the count of each processor online. Taking the lock that `call_rcu` released orders the updater's stores before its `call_rcu`, the unlinking of what the callback frees among them, before the counts are read. A processor has passed a quiescent state (`rcu_gp_ended`) once its count has moved from that reading, or when the reading was odd, since a halted processor is in no read section; a reader that began after the count moved, or after the halt ended, began after the unlinking was visible. The thread looks at each tick until every processor online at the beginning has passed one or gone offline, then takes a full barrier, counts the grace period and runs each processor's `wait` list in order (`rcu_gp_finish`). Its own sleeps are its processor's quiescent states.

A processor goes offline only when the kernel stops it, at a panic or a restart. The grace period then waits for it no more, as Linux's `rcu_report_dead` takes a processor out of the grace periods, and its callbacks still run, since the thread runs every processor's lists, as Linux moves a dead processor's callbacks to a live one (`rcutree_migrate_callbacks`).

## Limits

* There is no preemptible RCU: no reader may sleep, and there is no `srcu`.
* There is no expedited grace period (`synchronize_rcu_expedited`): a grace period takes one tick or more, 4 ms each at `TICK_HZ` 250, and longer when a processor stays in the kernel without a sleep or a service point.
* An interrupt that comes during the idle task's halt runs in its processor's extended quiescent state, as Linux's would before `ct_irq_enter`: an interrupt handler must not read under RCU.
* There are no callback batches by number (`rcu_segcblist`'s grace-period sequence numbers): one thread runs one grace period at a time, and a callback queued while one is under way waits for the next.

## Tests

`tests/host/rcu_hosttest.c` drives the thread's three steps itself, one host thread playing four processors. Its rows check that a processor inside a read section holds a grace period through the others' quiescent states and its own ticks in the kernel, and that a tick in user mode ends its hold; that callbacks of one processor run in their order and one queued after a grace period began waits for the next; that a processor idle at the beginning, or idle since, is not waited for, and one that left its idle wait is; and that a processor gone offline is waited for no more and its callback runs.
