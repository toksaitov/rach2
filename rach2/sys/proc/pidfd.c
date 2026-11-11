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

#define POLLIN     0x001
#define POLLHUP    0x010
#define POLLRDNORM 0x040

/* A pidfd: the thread group of its process, held by a reference, and the
 * process's kernel id, which its leader has, an exec by another thread
 * included. */
struct pidfd {
    struct anon_obj anon;
    struct tgroup *tg;
    uint32_t pid;
};

/** The leader of `pf`'s process, or NULL once the process has been reaped:
 * the record of its id must be a thread of its group, since the id may have
 * been given to another process since. */
static process_t *pidfd_task(const struct pidfd *pf)
{
    process_t *p = get_process(pf->pid);

    return p && p->tg == pf->tg && p->state != PROC_UNUSED ? p : NULL;
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

/** poll of a pidfd: POLLIN once every thread of the process has ended, and
 * POLLHUP too once it has been reaped, with the queue woken at the
 * last thread's end and the leader's reap. */
static short pidfd_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    const struct pidfd *pf = (const struct pidfd *)e->priv;
    short mask = 0;

    if (wq) {
        *wq = &pf->tg->exited;
    }
    if (pf->tg->live == 0) {
        mask |= POLLIN | POLLRDNORM;
    }
    if (!pidfd_task(pf)) {
        mask |= POLLIN | POLLRDNORM | POLLHUP;
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

struct pidfd *pidfd_new(process_t *p)
{
    struct pidfd *pf = kzalloc(sizeof(*pf));

    if (pf) {
        pf->anon.name = "[pidfd]";
        pf->tg = p->tg;
        pf->tg->refs++;
        pf->pid = p->tgid;
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
    fd_install(current_proc->fds, fd);
}

void pidfd_free(struct pidfd *pf)
{
    if (pf) {
        tgroup_put(pf->tg);
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

long sys_pidfd_open(struct isr_params *params)
{
    int32_t pid = (int32_t)SC_ARG(params, 0);
    uint32_t flags = (uint32_t)SC_ARG(params, 1);
    struct pidfd *pf;
    process_t *p;
    int fd;

    if (pid <= 0 || (flags & ~(uint32_t)PIDFD_NONBLOCK) != 0) {
        return -EINVAL;
    }
    p = proc_named((uint32_t)pid);
    if (!p || p->state == PROC_UNUSED) {
        return -ESRCH;
    }
    if (p->pid != p->tgid) {
        return -EINVAL;
    }
    pf = pidfd_new(p);
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

/** Whether the caller may take descriptors of the process `t` leads: its
 * own, or one it could attach to with ptrace, as Linux's
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
    if (!t->fds || fd_peek(t->fds, targetfd, &src, 0) != 0) {
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
        return -EBADF;
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
