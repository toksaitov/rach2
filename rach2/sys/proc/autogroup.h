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
 * root group, as Linux's autogroup_default. The group goes when the last
 * thread group lets it go (`autogroup_put`).
 */
struct autogroup {
    /* The thread groups that name it, and a caller that holds it across a
     * sleep; under the big lock. */
    uint32_t refs;
    struct task_group *tg; /* its queues and entities (fair_group.c) */
    unsigned long id;      /* from 1 in the order of creation, for the path */
    int nice;              /* -20 to 19, which gives `tg`'s shares */
};

/*
 * The calls the rest of the kernel makes. Each runs under the big lock in
 * process context, with sched_lock not held: autogroup takes it itself to
 * move threads between queues.
 */

/** Give the caller's session, which `setsid` has just made, a new group
 * and move every live thread of `p`'s thread group into it. With no memory
 * for a group, the threads go to the root group, as Linux's
 * autogroup_create falls back to autogroup_default. */
void autogroup_setsid(struct process *p);

/** Let `child`, the leader of a thread group that a fork has just made,
 * share `parent`'s group, as Linux's sched_autogroup_fork. A thread
 * that clone adds to a group needs no call, since its group is the thread
 * group's. Before `child` is first enqueued, the caller places its entity
 * with `autogroup_task_group(child)`. */
void autogroup_fork(struct process *child, struct process *parent);

/** Take `p`, which has begun to end and will not run in user mode again,
 * out of its group into the root group, as Linux's
 * sched_autogroup_exit_task; when it was the last live thread of its
 * thread group (`live` already 0), the thread group lets its group go,
 * which frees the group once its queues hold nothing. */
void autogroup_exit(struct process *p);

/** Set the nice value of `p`'s group, which gives its share, as a write of
 * /proc/PID/autogroup (Linux's proc_sched_autogroup_set_nice). 0;
 * -EINVAL for a nice value outside -20 to 19, -EPERM when the caller may
 * not lower a nice value that far (RLIMIT_NICE), and -EAGAIN for a caller
 * without privilege within 100 ms of the last change. A thread group in
 * the root group changes nothing and gets 0, as on Linux. */
int autogroup_nice(struct process *p, int nice);

/** Whether selection uses the groups: /proc/sys/kernel/sched_autogroup_enabled,
 * 1 at boot. */
int autogroup_enabled(void);

/** Turn the groups' use in selection on or off, moving each live thread to
 * its session's group or to the root group. Each thread keeps its group
 * while they are off. `on` is 0 or 1; the caller checks the range. */
void autogroup_set_enabled(int on);

/** The id and nice value of `p`'s group for /proc/PID/autogroup, as
 * Linux's proc_sched_autogroup_show_task: 1 with both set, or 0 for a
 * thread group in the root group, which shows nothing. */
int autogroup_show(const struct process *p, unsigned long *id, int *nice);

/** The group whose queues `p`'s entity belongs on: its session's group's
 * while the groups are on and `p` has not begun to end, else the root
 * group, as Linux's autogroup_task_group with task_wants_autogroup. Read
 * under sched_lock or the big lock. */
struct task_group *autogroup_task_group(const struct process *p);

/** The number of groups that exist, the root group not counted. */
uint32_t autogroup_count(void);

#endif /* _AUTOGROUP_H_ */
