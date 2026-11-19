/*
 * autogroup.c: a task group for each session, made at setsid and inherited
 * at fork, Linux 6.12's autogroup.
 */

#include "autogroup.h"

#include "fair.h"
#include "process.h"
#include "tgroup.h"
#include "../fs/krffs.h"
#include "../lib/kerrno.h"
#include "../mm/kheap.h"
#include "../syscall.h"
#include "../syscall_internal.h"
#include "../timer.h"

/* The last group's id, as Linux's autogroup_seq_nr. */
static unsigned long autogroup_ids;

/* The groups that exist. */
static uint32_t autogroups;

/* The tick before which a caller without privilege may not change a nice
 * value again, as the `next` of Linux's proc_sched_autogroup_set_nice. */
static uint64_t autogroup_nice_next;

/** Free `ag` and its task group once no thread group names it and no thread
 * is in it. NULL is the root group, which is never freed. */
static void autogroup_free_if_gone(struct autogroup *ag)
{
    if (ag && ag->refs == 0 && ag->members == 0) {
        tg_free(ag->tg);
        autogroups--;
        kfree(ag);
    }
}

/** Let a thread group's reference to `ag` go. */
static void autogroup_put(struct autogroup *ag)
{
    if (ag) {
        ag->refs--;
        autogroup_free_if_gone(ag);
    }
}

/** A new group with one reference, the caller's, or NULL with no memory
 * for it, which stands for the root group as Linux's fallback to
 * autogroup_default does. */
static struct autogroup *autogroup_create(void)
{
    struct autogroup *ag = kzalloc(sizeof(*ag));

    if (!ag) {
        return NULL;
    }
    ag->tg = tg_create();
    if (!ag->tg) {
        kfree(ag);
        return NULL;
    }
    ag->tg->autogroup = ag;
    ag->refs = 1;
    ag->id = ++autogroup_ids;
    autogroups++;
    return ag;
}

/** The group `p`'s entity is in, or NULL for the root group. */
static struct autogroup *autogroup_of_thread(const struct process *p)
{
    return p->sched_task_group ? p->sched_task_group->autogroup : NULL;
}

/** Make `ag`, NULL for the root group, the group of the thread `p`, moving
 * its entity and its count from the old group to the new one. Returns the
 * old group, which the caller frees if it is gone. */
static struct autogroup *autogroup_set_thread(struct process *p, struct autogroup *ag)
{
    struct autogroup *old = autogroup_of_thread(p);

    if (old != ag) {
        if (ag) {
            ag->members++;
        }
        tg_move_task(p, ag ? ag->tg : NULL);
        if (old) {
            old->members--;
        }
    }
    return old;
}

/** Whether `t` is a thread of `tg` that runs, or will run, in user mode: one
 * that has not ended or begun to, Linux's test of PF_EXITING. A thread that
 * fork is still building is one. */
static int autogroup_member(const struct process *t, const struct tgroup *tg)
{
    return t->tg == tg && t->state != PROC_ZOMBIE && t->futex_exit_state == 0;
}

void autogroup_setsid(struct process *p)
{
    struct autogroup *ag = autogroup_create();
    struct tgroup *tg = p->tg;
    struct autogroup *prev = tg->autogroup;
    process_t *t;

    /* As Linux's autogroup_move_group: the threads move before the old
     * group is let go. A thread that has begun to end keeps its group
     * until its autogroup_exit, which the count of members waits for. */
    tg->autogroup = ag;
    for (t = proc_list; t; t = t->next) {
        if (autogroup_member(t, tg)) {
            autogroup_free_if_gone(autogroup_set_thread(t, ag));
        }
    }
    autogroup_put(prev);
}

void autogroup_fork(struct process *child, struct process *parent)
{
    struct tgroup *tg = child->tg;

    if (tg != parent->tg) {
        tg->autogroup = parent->tg->autogroup;
        if (tg->autogroup) {
            tg->autogroup->refs++;
        }
    }
    autogroup_free_if_gone(autogroup_set_thread(child, tg->autogroup));
}

void autogroup_exit(struct process *p)
{
    struct tgroup *tg = p->tg;
    struct autogroup *ag;

    autogroup_free_if_gone(autogroup_set_thread(p, NULL));
    if (tg && tg->live == 0 && tg->autogroup) {
        ag = tg->autogroup;
        tg->autogroup = NULL;
        autogroup_put(ag);
    }
}

int autogroup_nice(struct process *p, int nice)
{
    struct autogroup *ag;

    if (nice < -20 || nice > 19) {
        return -EINVAL;
    }
    if (nice < 0 && !proc_nice_allowed(current_proc, nice)) {
        return -EPERM;
    }
    if (!cred_privileged() && kernel_ticks < autogroup_nice_next) {
        return -EAGAIN;
    }
    autogroup_nice_next = kernel_ticks + TICK_HZ / 10;
    ag = p->tg ? p->tg->autogroup : NULL;
    if (!ag) {
        return 0;
    }
    tg_set_shares(ag->tg, sched_prio_to_weight[nice + 20]);
    ag->nice = nice;
    return 0;
}

int autogroup_enabled(void)
{
    return fair_groups_enabled;
}

void autogroup_set_enabled(int on)
{
    process_t *t;

    on = on != 0;
    if (on == fair_groups_enabled) {
        return;
    }
    fair_groups_enabled = on;
    for (t = proc_list; t; t = t->next) {
        if (t->sched_task_group) {
            tg_move_task(t, t->sched_task_group);
        }
    }
}

int autogroup_show(const struct process *p, unsigned long *id, int *nice)
{
    const struct autogroup *ag = p->tg ? p->tg->autogroup : NULL;

    if (!ag) {
        return 0;
    }
    *id = ag->id;
    *nice = ag->nice;
    return 1;
}

uint32_t autogroup_count(void)
{
    return autogroups;
}
