/*
 * pidfd.c: descriptors that name a process, as pidfd.h describes them.
 */

#include "pidfd.h"

#include "ns.h"
#include "process.h"
#include "tgroup.h"
#include "wait_queue.h"
#include "../irq.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../io/anon.h"
#include "../mm/kheap.h"
#include "../mm/mm.h"
#include "../network/socket/socket.h"
#include "../lib/kerrno.h"

#define PIDFD_NONBLOCK 04000 /* O_NONBLOCK */
#define PIDFD_THREAD   0200  /* O_EXCL */

#define POLLIN     0x001
#define POLLHUP    0x010
#define POLLRDNORM 0x040

/* A pidfd: the thread group of its process and the PID namespace the
 * process has its number in, each held by a reference so that both outlast
 * the reap, and the kernel id it names with the `serial` of that id's
 * lifetime, as Linux's pidfd holds a struct pid: the process's, which its
 * leader has, an exec by another thread included, or for a pidfd of one
 * thread (PIDFD_THREAD) that thread's, which an exec by another thread of
 * the process gives to the leader it ends, as Linux's exchange_tids does.
 * The serial goes with the id (`proc_exchange_pids`), so no thread given
 * the id once its lifetime has ended passes for it. */
struct pidfd {
    struct anon_obj anon;
    struct tgroup *tg;
    struct pidns *ns;
    uint32_t pid;
    uint32_t thread; /* a pidfd of the one thread `pid`, not of the process */
    uint64_t serial; /* the `serial` (process.h) of `pid`'s lifetime */
    /* Its pollers, which a change of that lifetime wakes, the thread's end
     * and the release of its id, as Linux wakes the struct pid's
     * wait_pidfd, and no other thread's (`pidfd_notify`). */
    wait_queue_t polled;
    struct pidfd *next; /* the next on `tg->pidfds` */
};

/** The thread of the thread group `tg` whose id is `pid` in the lifetime
 * `serial`, or NULL once it no longer has the id: the record of the id
 * must be a thread of the group, since the id may have been given to
 * another process since, must have the serial, since another thread of the
 * group may have been given it since, and must not be a thread whose id
 * Linux has let go (`proc_tid_gone`). A process pidfd's leader keeps its
 * id until the reap. */
static process_t *group_task(const struct tgroup *tg, uint32_t pid, uint64_t serial)
{
    process_t *p = get_process(pid);

    return p && p->tg == tg && p->serial == serial && !proc_tid_gone(p) ? p : NULL;
}

/** The thread `pf` names, the leader for a pidfd of a process, or NULL once
 * it no longer has its id. */
static process_t *pidfd_task(const struct pidfd *pf)
{
    return group_task(pf->tg, pf->pid, pf->serial);
}

/** Wake the pollers of each pidfd of `p`'s id in its lifetime. The caller
 * holds the big lock. */
static void pidfd_notify(const process_t *p)
{
    struct pidfd *pf;

    for (pf = p->tg->pidfds; pf; pf = pf->next) {
        if (pf->serial == p->serial) {
            wq_wake_all(&pf->polled);
        }
    }
}

void pidfd_exit(process_t *t)
{
    process_t *leader = t->tg->leader;

    pidfd_notify(t);
    if (proc_tid_gone(t)) {
        t->pidfd_told = 1;
    }
    if (t->tg->live == 0 && leader != t) {
        pidfd_notify(leader);
    }
}

void pidfd_id_gone(process_t *p)
{
    if (!p->pidfd_told) {
        p->pidfd_told = 1;
        pidfd_notify(p);
    }
}

/** A read or a write of a pidfd, which has no data: EINVAL, as on Linux. */
static int32_t pidfd_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    (void)count;
    return -EINVAL;
}

static int32_t pidfd_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    (void)count;
    return -EINVAL;
}

/** poll of a pidfd: POLLIN once every thread of the process has ended, or
 * for a pidfd of one thread once that thread has, and POLLHUP too once the
 * process has been reaped or the thread has let its id go, as Linux 6.12's
 * pidfd_poll has it. The pidfd's own queue is woken at the end of its
 * thread, of a process's leader and of its last thread, and when its id
 * is let go (`pidfd_exit`, `pidfd_id_gone`). */
static short pidfd_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct pidfd *pf = (struct pidfd *)e->priv;
    const process_t *t = pidfd_task(pf);
    short mask = 0;

    if (wq) {
        *wq = &pf->polled;
    }
    if (!t) {
        mask = POLLIN | POLLRDNORM | POLLHUP;
    } else if (pf->thread ? t->state == PROC_ZOMBIE : pf->tg->live == 0) {
        mask = POLLIN | POLLRDNORM;
    }
    return (short)((mask & events) | (mask & POLLHUP));
}

/** The last close of a pidfd lets go of its thread group. */
static void pidfd_close(fd_entry_t *e)
{
    pidfd_free((struct pidfd *)e->priv);
    e->priv = NULL;
}

static const struct file_ops pidfd_ops = {pidfd_read,  pidfd_write, pidfd_poll, NULL,
                                          pidfd_close, NULL,        NULL,       0};

struct pidfd *pidfd_new(process_t *p, int thread)
{
    struct pidfd *pf = kzalloc(sizeof(*pf));

    if (pf) {
        pf->anon.name = "[pidfd]";
        pf->tg = p->tg;
        tgroup_get(pf->tg);
        pf->ns = pidns_get(proc_pidns(p));
        pf->pid = thread ? p->pid : p->tgid;
        pf->thread = thread != 0;
        pf->serial = thread ? p->serial : p->tg->leader->serial;
        wait_queue_init(&pf->polled);
        pf->next = pf->tg->pidfds;
        pf->tg->pidfds = pf;
    }
    return pf;
}

void pidfd_install(struct pidfd *pf, int fd, int nonblock)
{
    fd_entry_t *e = fd_slot(current_proc->fds, fd);

    e->type = FD_TYPE_ANON;
    e->ops = &pidfd_ops;
    e->priv = pf;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE | FD_FLAG_CLOEXEC;
    if (nonblock) {
        e->ofd->status_flags |= OFD_NONBLOCK;
    }
    if (pf->thread) {
        e->ofd->status_flags |= OFD_EXCL;
    }
    fd_install(current_proc->fds, fd);
}

void pidfd_free(struct pidfd *pf)
{
    struct pidfd **link;

    if (pf) {
        link = &pf->tg->pidfds;
        while (*link != pf) {
            link = &(*link)->next;
        }
        *link = pf->next;
        wq_wake_free(&pf->polled);
        tgroup_put(pf->tg);
        ns_put(&pf->ns->c);
        kfree(pf);
    }
}

int32_t pidfd_lookup(int fd, process_t **out, int *nonblock)
{
    fd_entry_t *e = current_proc ? fd_get(current_proc->fds, fd) : NULL;

    if (!e || e->ops != &pidfd_ops) {
        return -EBADF;
    }
    *out = pidfd_task((const struct pidfd *)e->priv);
    if (nonblock) {
        *nonblock = e->ofd && (e->ofd->status_flags & OFD_NONBLOCK) != 0;
    }
    return 0;
}

int32_t pidfd_ref_get(int fd, struct pidfd_ref *ref)
{
    fd_entry_t *e = current_proc ? fd_get(current_proc->fds, fd) : NULL;
    const struct pidfd *pf;

    if (!e || e->ops != &pidfd_ops) {
        return -EBADF;
    }
    pf = (const struct pidfd *)e->priv;
    ref->tg = pf->tg;
    tgroup_get(ref->tg);
    ref->ns = pidns_get(pf->ns);
    ref->pid = pf->pid;
    ref->serial = pf->serial;
    ref->thread = pf->thread;
    return 0;
}

process_t *pidfd_ref_task(const struct pidfd_ref *ref)
{
    return group_task(ref->tg, ref->pid, ref->serial);
}

int pidfd_ref_seen(const struct pidfd_ref *ref)
{
    return pidns_seen(ref->ns);
}

void pidfd_ref_put(struct pidfd_ref *ref)
{
    tgroup_put(ref->tg);
    ns_put(&ref->ns->c);
    ref->tg = NULL;
    ref->ns = NULL;
}

process_t *pidfd_process(process_t *t)
{
    return t && t->pid == t->tgid ? t : NULL;
}

long sys_pidfd_open(struct isr_params *params)
{
    int32_t pid = (int32_t)SC_ARG(params, 0);
    uint32_t flags = (uint32_t)SC_ARG(params, 1);
    int thread = (flags & PIDFD_THREAD) != 0;
    struct pidfd *pf;
    process_t *p;
    int fd;

    if ((flags & ~(uint32_t)(PIDFD_NONBLOCK | PIDFD_THREAD)) != 0 || pid <= 0) {
        return -EINVAL;
    }
    p = proc_named((uint32_t)pid);
    if (proc_tid_gone(p)) {
        return -ESRCH;
    }
    if (!thread && p->pid != p->tgid) {
        return -EINVAL;
    }
    pf = pidfd_new(p, thread);
    if (!pf) {
        return -ENOMEM;
    }
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        pidfd_free(pf);
        return fd;
    }
    pidfd_install(pf, fd, (flags & PIDFD_NONBLOCK) != 0);
    return fd;
}

/** Whether the caller may take descriptors of the thread `t`: one of its
 * own process, or one it could attach to with ptrace, as Linux's
 * PTRACE_MODE_ATTACH_REALCREDS check has it: the caller privileged, or its
 * real user and group IDs each of `t`'s three, and `t` dumpable and not
 * privileged. */
static int may_take_fds(const process_t *t)
{
    if (t->tg == current_proc->tg || proc_privileged(current_proc)) {
        return 1;
    }
    return current_proc->ruid == t->ruid && current_proc->ruid == t->euid &&
           current_proc->ruid == t->suid && current_proc->rgid == t->rgid &&
           current_proc->rgid == t->egid && current_proc->rgid == t->sgid &&
           (!t->mm || t->mm->dumpable) && !proc_privileged(t);
}

long sys_pidfd_getfd(struct isr_params *params)
{
    int targetfd = (int)SC_ARG(params, 1);
    fd_entry_t src;
    fd_entry_t *dst;
    process_t *t;
    int32_t rc;
    int fd;

    if (SC_ARG(params, 2) != 0) {
        return -EINVAL;
    }
    rc = pidfd_lookup((int)SC_ARG(params, 0), &t, NULL);
    if (rc != 0) {
        return rc;
    }
    if (!t) {
        return -ESRCH;
    }
    if (!may_take_fds(t)) {
        return -EPERM;
    }
    if (!t->fds) {
        return -ESRCH;
    }
    if (fd_peek(t->fds, targetfd, &src, 0) != 0) {
        return -EBADF;
    }
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        return fd;
    }
    /* The descriptor is taken again, with its description, under its
     * table's lock, since its owner may have closed it meanwhile. */
    if (!t->fds || fd_peek(t->fds, targetfd, &src, FD_PEEK_REF) != 0) {
        fd_free(current_proc->fds, fd);
        return t->fds ? -EBADF : -ESRCH;
    }
    dst = fd_slot(current_proc->fds, fd);
    fd_ofd_unref(dst->ofd);
    dst->type = src.type;
    dst->node = src.node;
    dst->sock = src.sock;
    dst->ops = src.ops;
    dst->priv = src.priv;
    dst->dev = src.dev;
    dst->ofd = src.ofd;
    dst->flags = (src.flags & (FD_FLAG_READ | FD_FLAG_WRITE | FD_FLAG_PATH)) | FD_FLAG_CLOEXEC;
    if (src.type == FD_TYPE_SOCKET && src.sock) {
        socket_ref(src.sock);
    }
    fd_install(current_proc->fds, fd);
    return fd;
}
