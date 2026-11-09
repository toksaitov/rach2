/*
 * kcmp.c: kcmp(2), whether two processes share a kernel object, as Linux
 * 6.12's kernel/kcmp.c answers it: an open file, the address space, the
 * descriptor table, the directories, the signal dispositions, the I/O
 * context, the semaphore adjustments or the target of an epoll
 * registration. Mesa compares descriptors of DRM nodes with it.
 */

#include "kcmp.h"

#include "ns.h"
#include "percpu.h"
#include "process.h"
#include "tgroup.h"
#include "../arch/arch.h"
#include "../fs/fd.h"
#include "../io/epoll.h"
#include "../ipc/sysv.h"
#include "../lib/kerrno.h"
#include "../lib/krandom.h"
#include "../mm/mm.h"
#include "../mm/usercopy.h"

/* The kinds of object, Linux's enum kcmp_type. */
#define KCMP_FILE      0
#define KCMP_VM        1
#define KCMP_FILES     2
#define KCMP_FS        3
#define KCMP_SIGHAND   4
#define KCMP_IO        5
#define KCMP_SYSVSEM   6
#define KCMP_EPOLL_TFD 7
#define KCMP_TYPES     8

/* KCMP_EPOLL_TFD's argument: the epoll set of the second process, the
 * descriptor number registered in it and which of its registrations. */
struct kcmp_epoll_slot {
    uint32_t efd;
    uint32_t tfd;
    uint32_t toff;
};

/* The keys that hide the objects' addresses while keeping an order, one
 * pair for each kind, drawn at the first call, as Linux's cookies. */
static uintptr_t cookies[KCMP_TYPES][2];
static int cookies_drawn;

/** Draw the keys; the multiplier is odd with its top bit set, as Linux's. */
static void draw_cookies(void)
{
    int i;

    krandom_bytes(cookies, sizeof(cookies));
    for (i = 0; i < KCMP_TYPES; i++) {
        cookies[i][1] |= ~(~(uintptr_t)0 >> 1) | 1u;
    }
    cookies_drawn = 1;
}

/** The order of two objects of kind `type`: 0 for the same, 1 when the
 * first's key is below the second's, 2 when above. */
static long kcmp_ptr(const void *v1, const void *v2, int type)
{
    uintptr_t t1 = ((uintptr_t)v1 ^ cookies[type][0]) * cookies[type][1];
    uintptr_t t2 = ((uintptr_t)v2 ^ cookies[type][0]) * cookies[type][1];

    return t1 == t2 ? 0 : t1 < t2 ? 1 : 2;
}

/** Whether the caller may inspect `t`, as Linux's ptrace_may_access with
 * PTRACE_MODE_READ_REALCREDS: its own thread group, a privileged caller,
 * or a caller whose real user and group IDs are each of `t`'s three, with
 * `t` dumpable and not privileged. */
static int may_inspect(const process_t *t)
{
    if (t->tg == current_proc->tg || proc_privileged(current_proc)) {
        return 1;
    }
    return current_proc->ruid == t->ruid && current_proc->ruid == t->euid &&
           current_proc->ruid == t->suid && current_proc->rgid == t->rgid &&
           current_proc->rgid == t->egid && current_proc->rgid == t->sgid &&
           (!t->mm || t->mm->dumpable) && !proc_privileged(t);
}

/** The open file of descriptor `idx` of `t`, or NULL for none. */
static const struct open_file *file_of(const process_t *t, uintptr_t idx)
{
    fd_entry_t *e;

    if (!t->fds || idx > 0x7FFFFFFFu) {
        return NULL;
    }
    e = fd_get(t->fds, (int)idx);
    return e ? e->ofd : NULL;
}

/** KCMP_EPOLL_TFD: the file of `t1`'s descriptor `idx1` against the target
 * of the registration `uslot` names in an epoll set of `t2`. */
static long kcmp_epoll_target(const process_t *t1, const process_t *t2, uintptr_t idx1,
                              const void __user *uslot)
{
    struct kcmp_epoll_slot slot;
    const struct open_file *f1, *target;
    fd_entry_t *ep;
    int32_t rc;

    if (copy_from_user(&slot, uslot, sizeof(slot)) != 0) {
        return -EFAULT;
    }
    f1 = file_of(t1, idx1);
    if (!f1) {
        return -EBADF;
    }
    ep = t2->fds && slot.efd <= 0x7FFFFFFFu ? fd_get(t2->fds, (int)slot.efd) : NULL;
    if (!ep) {
        return -EBADF;
    }
    rc = epoll_target(ep, (int)slot.tfd, slot.toff, &target);
    if (rc != 0) {
        return rc;
    }
    return kcmp_ptr(f1, target, KCMP_FILE);
}

long sys_kcmp(isr_params_t *params)
{
    process_t *t1 = proc_named((uint32_t)(int32_t)SC_ARG(params, 0));
    process_t *t2 = proc_named((uint32_t)(int32_t)SC_ARG(params, 1));
    int type = (int)SC_ARG(params, 2);
    uintptr_t idx1 = (uintptr_t)SC_ARG(params, 3);
    uintptr_t idx2 = (uintptr_t)SC_ARG(params, 4);
    const struct open_file *f1, *f2;

    if (!t1 || !t2) {
        return -ESRCH;
    }
    if (!may_inspect(t1) || !may_inspect(t2)) {
        return -EPERM;
    }
    if (!cookies_drawn) {
        draw_cookies();
    }
    switch (type) {
    case KCMP_FILE:
        f1 = file_of(t1, idx1);
        f2 = file_of(t2, idx2);
        if (!f1 || !f2) {
            return -EBADF;
        }
        return kcmp_ptr(f1, f2, KCMP_FILE);
    case KCMP_VM:
        return kcmp_ptr(t1->mm, t2->mm, KCMP_VM);
    case KCMP_FILES:
        return kcmp_ptr(t1->fds, t2->fds, KCMP_FILES);
    case KCMP_FS:
        return kcmp_ptr(t1->fs, t2->fs, KCMP_FS);
    case KCMP_SIGHAND:
        return kcmp_ptr(t1->sigacts, t2->sigacts, KCMP_SIGHAND);
    case KCMP_IO:
        /* Linux gives a task an I/O context when its priority is set, and
         * a thread one of its own, since nothing here passes CLONE_IO. */
        return kcmp_ptr(t1->ioprio ? t1 : NULL, t2->ioprio ? t2 : NULL, KCMP_IO);
    case KCMP_SYSVSEM:
        return kcmp_ptr(sysv_sem_undo_list(t1->tg), sysv_sem_undo_list(t2->tg), KCMP_SYSVSEM);
    case KCMP_EPOLL_TFD:
        return kcmp_epoll_target(t1, t2, idx1, (const void __user *)idx2);
    default:
        return -EINVAL;
    }
}
