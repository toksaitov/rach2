/* Autogroup: a scheduling group for each session, which shares the
 * processors between sessions before their threads, as Linux 6.12's. */
#ifndef _AUTOGROUP_H_
#define _AUTOGROUP_H_

#include "../lib/kint.h"

struct process;
struct task_group;

/*
 * A session's group, which `setsid` makes and each thread group of the
 * session names in its `struct tgroup`, as Linux's signal->autogroup
 * (kernel/sched/autogroup.h:7-18). A thread group that names none is in the
 * root group, as Linux's autogroup_default. Both counts are under the big
 * lock; the group and its task group are freed when both reach 0, so that
 * no thread's entity is left on a freed queue.
 */
struct autogroup {
    uint32_t refs;         /* the thread groups that name it */
    uint32_t members;      /* the threads whose `sched_task_group` is `tg` */
    struct task_group *tg; /* its queues and entities (fair_group.c) */
    unsigned long id;      /* from 1 in the order of creation, for the path */
    int nice;              /* -20 to 19, which gives `tg`'s shares */
};

/*
 * The calls the rest of the kernel makes. Each runs under the big lock in
 * process context, with sched_lock not held: autogroup takes it to move a
 * thread between queues, and allocates and frees outside it.
 */

/** Give the caller's session, which `setsid` has just made, a new group
 * and move into it every thread of `p`'s thread group that has not begun
 * to end, as Linux's sched_autogroup_create_attach. With no memory for a
 * group, the threads go to the root group, as Linux's autogroup_create
 * falls back to autogroup_default. */
void autogroup_setsid(struct process *p);

/** Put `child`, a thread that fork or clone has just made from `parent`,
 * in its thread group's group: a new thread group shares `parent`'s, as
 * Linux's sched_autogroup_fork, and a thread added to a group joins it.
 * Called once for every new thread, once the fork can no longer fail and
 * before the thread's first `fair_set_task_rq`. */
void autogroup_fork(struct process *child, struct process *parent);

/** Take `p`, which has begun to end and will not run in user mode again,
 * out of its group into the root group, as Linux's
 * sched_autogroup_exit_task. When `p`'s thread group has no live thread
 * left (`live` already 0), the thread group lets its group go, as Linux's
 * sched_autogroup_exit. Called once for each thread that ends, before its
 * record is freed; a second call changes nothing. */
void autogroup_exit(struct process *p);

/** Set the nice value of `p`'s group, which gives its share, as a write of
 * /proc/PID/autogroup (Linux's proc_sched_autogroup_set_nice). 0;
 * -EINVAL for a nice value outside -20 to 19, -EPERM when the caller may
 * not lower a nice value that far (RLIMIT_NICE), and -EAGAIN for a caller
 * without privilege within 100 ms of the last change by anyone. A thread
 * group in the root group changes nothing and gets 0, as on Linux. */
int autogroup_nice(struct process *p, int nice);

/** Whether selection uses the groups: /proc/sys/kernel/sched_autogroup_enabled,
 * 1 at boot. */
int autogroup_enabled(void);

/** Turn the groups' use in selection on or off, moving each thread in a
 * group to its group's queues or to the root queues. Each thread keeps its
 * group while they are off. `on` is 0 or 1; the caller checks the range. */
void autogroup_set_enabled(int on);

/** The id and nice value of `p`'s group for /proc/PID/autogroup, as
 * Linux's proc_sched_autogroup_show_task: 1 with both set, or 0 for a
 * thread group in the root group, which shows nothing. */
int autogroup_show(const struct process *p, unsigned long *id, int *nice);

/** The number of groups that exist, the root group not counted. */
uint32_t autogroup_count(void);

/*
 * The groups' queues and entities, fair_group.c.
 */

/** Give the root group its per-processor arrays, the processors' root
 * queues and no entities, and set `fair_hooks.group_update`. Once, from
 * `fair_init`, before the first enqueue. */
void fair_group_init(void);

/** A new group under the root group with nice 0's share, its queue and
 * entity on each processor that is online, its entities' averages attached
 * to their root queues; or NULL with no memory. Takes sched_lock. Every
 * processor is online before the first thread that may call `setsid`
 * runs, so a group has a queue wherever a thread of it can run. */
struct task_group *tg_create(void);

/** Take `tg`, which no thread is in, off every queue and free it, as
 * Linux's sched_destroy_group: an entity that DELAY_DEQUEUE keeps queued
 * finishes its dequeue, and its queues leave the leaf lists. Takes
 * sched_lock; frees outside it. */
void tg_free(struct task_group *tg);

/** Set `tg`'s share to `shares`, clamped to Linux's MIN_SHARES and
 * MAX_SHARES, and give each of its entities its weight again, as Linux's
 * sched_group_set_shares. Takes sched_lock. */
void tg_set_shares(struct task_group *tg, unsigned long shares);

/** Make `tg`, NULL for the root group, `p`'s group, and move `p`'s entity to
 * the queue `fair_task_group` then names, as Linux's sched_move_task.
 * Takes sched_lock. */
void tg_move_task(struct process *p, struct task_group *tg);

/** What gives processor `cpu` its clocks by `fair_clock` before a group
 * change charges a thread there, called with sched_lock held; set by the
 * scheduler at boot. While it is NULL the hooks charge by the clocks last
 * given. */
extern void (*fair_group_clock)(int cpu);

#endif /* _AUTOGROUP_H_ */
