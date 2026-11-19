/* orphan.c: SIGHUP and SIGCONT to a process group that a process's end
 * leaves orphaned with a member stopped, as POSIX and Linux send them. */
#include "process.h"
#include "tgroup.h"
#include "../lib/ksignal.h"
#include "../tty/tty.h"

/* The pid of the system's init, owned by kernel.c. */
extern volatile uint32_t init_pid;

/** Whether the process `p` is in the process group `pgid` of the lifetime
 * `serial`: a leader whose record is in use, as Linux's PIDTYPE_PGID lists
 * hold the group's leaders. */
static int pgrp_member(const process_t *p, uint32_t pgid, uint64_t serial)
{
    return p->state != PROC_UNUSED && p->pid == p->tgid && p->pgid == pgid &&
           p->pgrp_serial == serial;
}

/**
 * Whether the process group `pgid` of the lifetime `serial` is orphaned when
 * `ignored` (NULL for none) is left out, as Linux 6.12.115's
 * will_become_orphaned_pgrp judges it (kernel/exit.c:332-349): no member but
 * `ignored`, one whose threads have all ended or one whose parent is the
 * system's init has its parent in another group of the same session.
 */
static int pgrp_orphaned(uint32_t pgid, uint64_t serial, const process_t *ignored)
{
    process_t *p;
    process_t *parent;

    for (p = proc_list; p; p = p->next) {
        if (!pgrp_member(p, pgid, serial) || p == ignored ||
            (p->state == PROC_ZOMBIE && p->tg->live == 0) || p->parent_pid == init_pid) {
            continue;
        }
        parent = get_process(p->parent_pid);
        if (parent && !pgrp_member(parent, pgid, serial) && parent->sid == p->sid) {
            return 0;
        }
    }
    return 1;
}

/** Whether a member of the process group `pgid` of the lifetime `serial` has
 * a stop in effect, as Linux 6.12.115's has_stopped_jobs asks
 * (kernel/exit.c:362-372). */
static int pgrp_stopped(uint32_t pgid, uint64_t serial)
{
    process_t *p;

    for (p = proc_list; p; p = p->next) {
        if (pgrp_member(p, pgid, serial) && p->state != PROC_ZOMBIE && p->tg->stop != 0) {
            return 1;
        }
    }
    return 0;
}

void pgrp_orphan_signal(const process_t *member, const process_t *parent, const process_t *ignored)
{
    uint32_t pgid = member->pgid;
    uint64_t serial = member->pgrp_serial;

    if (!parent || pgrp_member(parent, pgid, serial) || parent->sid != member->sid ||
        !pgrp_orphaned(pgid, serial, ignored) || !pgrp_stopped(pgid, serial)) {
        return;
    }
    tty_signal_group((int)pgid, serial, SIGHUP);
    tty_signal_group((int)pgid, serial, SIGCONT);
}
