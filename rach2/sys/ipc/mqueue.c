/*
 * mqueue.c: POSIX message queues, as Linux gives them through mq_open and
 * its five calls (ipc/Readme.md).
 *
 * A named queue is on one list with the IPC namespace it was made in, and
 * only a process of that namespace finds it by name. It stays on the list
 * until mq_unlink takes its name, and it lives while its name, a descriptor,
 * a waiter or an open of its file has a reference.
 * A queue description retains the queue's inode and its mount. Its
 * access mode is the descriptor's read and write flags and its O_NONBLOCK
 * the description's. Its stat is that of the queue's file and its /proc
 * link the queue's name, as Linux's descriptor is open on that file.
 * Messages are kept highest priority first, in the order they came within a
 * priority. A sender or a receiver that waits holds a reference, so the
 * queue outlives its last close while one sleeps on it.
 *
 * The mqueue file system lists the named queues of one IPC namespace as the
 * files of a directory, as Linux's /dev/mqueue does: a file's attributes are
 * its queue's, reading it gives the queue's status line, and unlinking it is
 * mq_unlink.
 */

#include "ipc32.h"
#include "mqueue.h"

#include "../irq.h"
#include "../proc/process.h"
#include "../proc/smp.h"
#include "../proc/tgroup.h"
#include "../proc/ns.h"
#include "../proc/wait_queue.h"
#include "../mm/mm.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../fs/krffs.h"
#include "../fs/pathbuf.h"
#include "../fs/permission.h"
#include "../fs/vfs.h"
#include "../io/poll.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/ksignal.h"
#include "../lib/kstring.h"
#include "../rtc.h"
#include "../timer.h"
#include "../syscall.h"
#include "../syscall_internal.h"

/* The longest name, the priorities a message may have, and the queues an
 * unprivileged caller may make, counted over every namespace. */
#define MQ_NAME_MAX 255
#define MQ_PRIO_MAX 32768u
#define MQ_QUEUES   256

/* The limits of /proc/sys/fs/mqueue: MQ_MSG_MAX and MQ_MSGSIZE_MAX are an
 * unprivileged caller's largest queue and the queue mq_open makes without
 * attributes, and the hard limits a privileged caller's largest. */
#define MQ_MSG_MAX         10
#define MQ_MSGSIZE_MAX     8192
#define MQ_HARD_MSGMAX     65536
#define MQ_HARD_MSGSIZEMAX (16 * 1024 * 1024)

#define O_ACCMODE  3
#define O_CREAT    0100
#define O_EXCL     0200
#define O_NONBLOCK 04000
#define O_CLOEXEC  02000000

#define SIGEV_SIGNAL 0
#define SIGEV_NONE   1
#define SIGEV_THREAD 2

/* A message: its priority and `len` bytes, which follow the record. */
struct mq_msg {
    struct mq_msg *next;
    uint32_t prio;
    uint32_t len;
};

/* The directory shared by a namespace's private and visible mounts. */
struct mq_root {
    uint32_t mode, uid, gid;
    unsigned int queues; /* includes open unlinked queues */
    int namespace_live;
    int mounted;      /* the root outlives any waiter on an unlinked queue */
    uint32_t entries; /* includes dot and dot-dot */
    int64_t atime_s, atime_ns, mtime_s, mtime_ns, ctime_s, ctime_ns;
};

/* A queue. `refs` counts its name's reference, each description's, each
 * waiter's and each open of its file; `head` is its messages, highest
 * priority first. `gen` rises at every change, which a sleeper compares
 * (`struct mq_wait`). A registered notification is `notify_how`,
 * SIGEV_SIGNAL or SIGEV_NONE, with its signal and value. */
struct mqueue {
    struct mqueue *next;   /* the list of named queues */
    char *name;            /* kept after mq_unlink for the /proc links */
    int unlinked;          /* the name has left the list */
    struct mq_root *root;  /* retained while this queue lives */
    struct vfs_mount *mnt; /* the private mount where its name was created */
    uint32_t size;         /* stat size, independent of the status text */
    uint32_t refs;
    uint32_t ino; /* its file's number in the mqueue file system */
    /* Its file's access, modification and change times: of its making, then
     * of its last message sent or received, or what a setattr stored. */
    int64_t atime_s, atime_ns, mtime_s, mtime_ns, ctime_s, ctime_ns;
    uint32_t uid, gid, mode;
    int32_t maxmsg, msgsize, curmsgs;
    struct mq_msg *head;
    wait_queue_t wq;
    uint32_t gen;
    uint32_t receivers;   /* receivers waiting, to whom a message goes before a notification */
    uint32_t notify_tgid; /* the process registered by mq_notify, 0 for none */
    int notify_how;
    int notify_signo;
    uintptr_t notify_value;
};

/* Linux's struct mq_attr. */
struct mq_attr {
    long flags, maxmsg, msgsize, curmsgs;
    long reserved[4];
};

/* The named queues and the next inode number, 1 being the root's. */
static struct mqueue *queues;
static uint32_t next_ino = 2;

/** Set the access, modification and change times of `q`'s file to now. */
static void mq_touch(struct mqueue *q)
{
    get_current_time(&q->ctime_s, &q->ctime_ns);
    q->atime_s = q->mtime_s = q->ctime_s;
    q->atime_ns = q->mtime_ns = q->ctime_ns;
}

/** Drop a reference to `q`, freeing it and its messages with the last. */
static void mq_put(struct mqueue *q)
{
    if (--q->refs != 0) {
        return;
    }
    while (q->head) {
        struct mq_msg *m = q->head;

        q->head = m->next;
        kfree(m);
    }
    if (--q->root->queues == 0 && !q->root->mounted) {
        kfree(q->root);
    }
    kfree(q->name);
    kfree(q);
}

/** Return whether two queue names are equal. */
static int name_is(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Find a queue by name on the namespace's private mount. */
static struct mqueue *mq_find(const struct vfs_mount *mnt, const char *name)
{
    struct mqueue *q;

    for (q = queues; q; q = q->next) {
        if (q->mnt == mnt && name_is(q->name, name)) {
            return q;
        }
    }
    return NULL;
}

/** Stamp all three directory times after a change of names. */
static void mq_root_stamp(struct mq_root *r)
{
    get_current_time(&r->ctime_s, &r->ctime_ns);
    r->atime_s = r->mtime_s = r->ctime_s;
    r->atime_ns = r->mtime_ns = r->ctime_ns;
}

/** Take `q`'s name away: out of the list, the reference the name held
 * dropped. The string stays for the descriptors' /proc links. */
static void mq_unname(struct mqueue *q)
{
    struct mqueue **link = &queues;
    struct mq_root *r = q->mnt->root;

    while (*link != q) {
        link = &(*link)->next;
    }
    *link = q->next;
    r->entries--;
    mq_root_stamp(r);
    q->unlinked = 1;
    mq_put(q);
}

static int32_t mq_read(fd_entry_t *e, char __user *buf, uint32_t count);
static short mq_poll(fd_entry_t *e, short events, struct wait_queue **wq);

#define FIGETBSZ      2u
#define FIONREAD      0x541bu
#define FIOASYNC      0x5452u
#define FIOQSIZE      0x5460u
#define FS_IOC_FIEMAP 0xc020660bu

/** Answer the VFS ioctl requests that apply to a queue description. */
static int32_t mq_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    const struct mqueue *q = e->ofd->vnode;
    int avail = (int)(q->size - e->ofd->offset);
    uint64_t allocated = 0;
    int value;

    if (request == FIOASYNC) {
        if (copy_from_user(&value, (const void __user *)arg, sizeof(value)))
            return -EFAULT;
        return value ? -ENOTTY : 0;
    }
    if (request == FS_IOC_FIEMAP)
        return -EOPNOTSUPP;
    if (request == FIGETBSZ || request == FIONREAD) {
        value = request == FIGETBSZ ? PAGE_SIZE : avail;
        return copy_to_user((void __user *)arg, &value, sizeof(value)) ? -EFAULT : 0;
    }
    if (request == FIOQSIZE)
        return copy_to_user((void __user *)arg, &allocated, sizeof(allocated)) ? -EFAULT : 0;
    return -ENOTTY;
}

const struct file_ops mq_ops = {mq_read, NULL, mq_poll, mq_ioctl,
                                NULL,    NULL, NULL,    FILE_OPS_POSITIONED};

/** The queue the caller's descriptor `fd` is open on, with the descriptor
 * in `*ep`, or NULL when `fd` is no queue's. */
static struct mqueue *mq_of(int fd, fd_entry_t **ep)
{
    fd_entry_t *e = fd_get(current_proc->fds, fd);

    if (!e || e->ops != &mq_ops) {
        return NULL;
    }
    *ep = e;
    return e->ofd->vnode;
}

/**
 * Write to `out` the text a read of the queue `q` gives, as Linux's mqueue
 * file has it: the bytes of the messages queued, the notification's kind,
 * its signal and the process registered. At most `cap` - 1 characters and a
 * newline are written, then a NUL, so `out` needs `cap` + 1 bytes; returns
 * the length without the NUL.
 */
static int mq_status(const struct mqueue *q, char *out, int cap)
{
    static const char *const keys[] = {"QSIZE:", " NOTIFY:", " SIGNO:", " NOTIFY_PID:"};
    static const int widths[] = {10, 5, 5, 6};
    uint32_t vals[4];
    const struct mq_msg *m;
    int i, n = 0;

    vals[0] = 0;
    for (m = q->head; m; m = m->next) {
        vals[0] += m->len;
    }
    vals[1] = q->notify_tgid ? (uint32_t)q->notify_how : 0u;
    vals[2] = q->notify_tgid && q->notify_how == SIGEV_SIGNAL ? (uint32_t)q->notify_signo : 0u;
    vals[3] = q->notify_tgid;
    for (i = 0; i < 4; i++) {
        char digits[12];
        int d = 0, w;
        uint32_t v = vals[i];

        for (w = 0; keys[i][w] && n < cap - 1; w++) {
            out[n++] = keys[i][w];
        }
        do {
            digits[d++] = (char)('0' + v % 10u);
            v /= 10u;
        } while (v != 0);
        for (w = d; d > 0 && n < cap - 1; d--) {
            out[n++] = digits[d - 1];
        }
        for (; w < widths[i] && n < cap - 1; w++) {
            out[n++] = ' ';
        }
    }
    out[n++] = '\n';
    out[n] = '\0';
    return n;
}

/** The read of a queue's descriptor: its status line (`mq_status`) from the
 * description's offset, which advances; 0 past its end, or -EFAULT. */
static int32_t mq_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    char text[96];
    struct mqueue *q = e->ofd->vnode;
    int n = mq_status(q, text, (int)sizeof(text) - 1);
    uint32_t at = e->ofd->offset > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)e->ofd->offset;

    if (at >= (uint32_t)n) {
        return 0;
    }
    if (count > (uint32_t)n - at) {
        count = (uint32_t)n - at;
    }
    if (copy_to_user(buf, text + at, count) != 0) {
        return -EFAULT;
    }
    if (count) {
        get_current_time(&q->ctime_s, &q->ctime_ns);
        q->atime_s = q->ctime_s;
        q->atime_ns = q->ctime_ns;
    }
    e->ofd->offset += count;
    return (int32_t)count;
}

/** The poll of a queue's descriptor, on the queue's wait queue: readable
 * while a message is queued, writable while there is room for one. */
static short mq_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct mqueue *q = e->ofd->vnode;
    short ready = 0;

    if (wq) {
        *wq = &q->wq;
    }
    if (q->curmsgs > 0) {
        ready |= POLLIN;
    }
    if (q->curmsgs < q->maxmsg) {
        ready |= POLLOUT;
    }
    return (short)(ready & events);
}

void mq_flush(const fd_entry_t *e)
{
    struct mqueue *q = e->ofd->vnode;

    if (current_proc && q->notify_tgid == current_proc->tgid) {
        q->notify_tgid = 0;
    }
}

/* Whether the call is an i386 call on x86-64, whose mq_attr, timespec and
 * sigevent have 32-bit longs and pointers. */
#define mq_compat() (sizeof(long) > 4u && in_compat_syscall())

/** Read the caller's struct mq_attr at `u` into `a`: an i386 call's on
 * x86-64 is Linux's compat_mq_attr, of 32-bit longs, sign-extended. 0 or
 * -EFAULT. */
static int32_t mq_attr_get(const void __user *u, struct mq_attr *a)
{
    struct ipc32_mq_attr a32;

    if (!mq_compat()) {
        return copy_from_user(a, u, sizeof(*a)) != 0 ? -EFAULT : 0;
    }
    if (copy_from_user(&a32, u, sizeof(a32)) != 0) {
        return -EFAULT;
    }
    k_memset(a, 0, sizeof(*a));
    a->flags = a32.flags;
    a->maxmsg = a32.maxmsg;
    a->msgsize = a32.msgsize;
    a->curmsgs = a32.curmsgs;
    return 0;
}

/** Copy `a` out to the caller's struct mq_attr at `u`, in its layout
 * (`mq_attr_get`). 0 or -EFAULT. */
static int32_t mq_attr_put(void __user *u, const struct mq_attr *a)
{
    struct ipc32_mq_attr a32;

    if (!mq_compat()) {
        return copy_to_user(u, a, sizeof(*a)) != 0 ? -EFAULT : 0;
    }
    k_memset(&a32, 0, sizeof(a32));
    a32.flags = (int32_t)a->flags;
    a32.maxmsg = (int32_t)a->maxmsg;
    a32.msgsize = (int32_t)a->msgsize;
    a32.curmsgs = (int32_t)a->curmsgs;
    return copy_to_user(u, &a32, sizeof(a32)) != 0 ? -EFAULT : 0;
}

/** Set a new queue's limits from the supplied attributes or the defaults.
 * Return -EINVAL for a count or size outside the caller's limits. */
static int32_t mq_new_attr(const struct mq_attr *a, int32_t *maxmsg, int32_t *msgsize)
{
    int priv = proc_privileged(current_proc);

    *maxmsg = MQ_MSG_MAX;
    *msgsize = MQ_MSGSIZE_MAX;
    if (!a) {
        return 0;
    }
    if (a->maxmsg <= 0 || a->msgsize <= 0 || a->maxmsg > (priv ? MQ_HARD_MSGMAX : MQ_MSG_MAX) ||
        a->msgsize > (priv ? MQ_HARD_MSGSIZEMAX : MQ_MSGSIZE_MAX)) {
        return -EINVAL;
    }
    *maxmsg = (int32_t)a->maxmsg;
    *msgsize = (int32_t)a->msgsize;
    return 0;
}

/** A new queue `name` in `*out`, the caller's, with the bits of `mode` its
 * umask leaves, entered in the list with the reference its name holds.
 * Returns 0, the refusals of `mq_new_attr`, -ENOSPC past MQ_QUEUES queues
 * for an unprivileged caller, or -ENOMEM. */
static int32_t mq_create(struct vfs_mount *mnt, const char *name, uint32_t mode,
                         const struct mq_attr *attr, struct mqueue **out)
{
    struct mqueue *q;
    struct mq_root *root = mnt->root;
    int32_t maxmsg, msgsize;
    int32_t rc;

    if (!root->namespace_live) {
        return -EACCES;
    }
    if (root->queues >= MQ_QUEUES && !proc_privileged(current_proc)) {
        return -ENOSPC;
    }
    rc = mq_new_attr(attr, &maxmsg, &msgsize);
    if (rc != 0) {
        return rc;
    }
    q = kzalloc(sizeof(*q));
    if (q) {
        q->name = kmalloc(k_strlen(name) + 1);
    }
    if (!q || !q->name) {
        kfree(q);
        return -ENOMEM;
    }
    k_memcpy(q->name, name, k_strlen(name) + 1);
    q->mnt = mnt;
    q->root = root;
    q->size = 80;
    q->refs = 1;
    q->ino = next_ino++;
    mq_touch(q);
    q->uid = proc_fsuid(current_proc);
    q->gid = proc_fsgid(current_proc);
    q->mode = mode & 07777u & ~current_proc->fs->umask;
    q->maxmsg = maxmsg;
    q->msgsize = msgsize;
    wait_queue_init(&q->wq);
    q->next = queues;
    queues = q;
    root->queues++;
    root->entries++;
    mq_root_stamp(root);
    *out = q;
    return 0;
}

/** Copy the user pathname before allocating a descriptor, as getname does. */
static int mq_name(const char __user *uname, char **out)
{
    int n;
    char *name = path_buf_get();

    if (!name) {
        return -ENOMEM;
    }
    n = strncpy_from_user(name, uname, VFS_PATH_MAX);
    if (n <= 0) {
        path_buf_put(name);
        return n == 0 ? -ENOENT : n;
    }
    *out = name;
    return 0;
}

/** Check the component, root search and maximum queue name length. */
static int mq_lookup_allowed(const struct mq_root *r, const char *name)
{
    size_t n = 0;

    if (name_is(name, ".") || name_is(name, "..")) {
        return -EACCES;
    }
    while (name[n]) {
        if (name[n++] == '/') {
            return -EACCES;
        }
    }
    if (!inode_may_access(r->uid, r->gid, 0040000u | r->mode, 1, 0)) {
        return -EACCES;
    }
    return n > MQ_NAME_MAX ? -ENAMETOOLONG : 0;
}

/** A descriptor open on `q` for the access mode of `oflag`, with its
 * O_NONBLOCK and O_CLOEXEC; it takes a reference to `q`. Returns the
 * descriptor, or -ENOMEM or `fd_alloc`'s error. */
static int32_t mq_install(int fd, struct mqueue *q, uint32_t oflag)
{
    int acc = (int)(oflag & O_ACCMODE);
    fd_entry_t *e = fd_slot(current_proc->fds, fd);
    struct vfs_ref file;
    int rc;

    file.mnt = q->mnt;
    file.node = q;
    if (acc == 1 || acc == 2) {
        rc = fd_ofd_access(e->ofd, &file, 1);
        if (rc != 0) {
            return rc;
        }
    }
    rc = vfs_open_node(file.mnt, q, (int)oflag, &e->ofd->fhandle);
    if (rc != 0) {
        return rc;
    }
    q->refs++; /* the vnode held by the open file description */
    vfs_desc_open(file.mnt);
    e->ofd->vmnt = file.mnt;
    e->ofd->vnode = q;
    e->ofd->ino = q->ino;
    mq_bind(e, (unsigned)acc, FD_FLAG_CLOEXEC);
    if (oflag & O_NONBLOCK) {
        e->ofd->status_flags |= OFD_NONBLOCK;
    }
    fd_install(current_proc->fds, fd);
    return fd;
}

/** Reserve a descriptor before lookup and release it if the open fails. */
static int mq_open_named(const char *name, uint32_t oflag, uint32_t mode,
                         const struct mq_attr *attr)
{
    struct vfs_mount *mnt = proc_ipcns(current_proc)->mq;
    const struct mq_root *r = mnt->root;
    struct mqueue *q;
    uint32_t acc = oflag & O_ACCMODE;
    int fd, rc;

    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        return fd;
    }
    vfs_ns_lock(mnt);
    rc = mq_lookup_allowed(r, name);
    if (rc != 0) {
        vfs_ns_unlock(mnt);
        fd_free(current_proc->fds, fd);
        return rc;
    }
    q = mq_find(mnt, name);
    if (!q && !(oflag & O_CREAT)) {
        rc = -ENOENT;
    } else if (!q && vfs_mount_rdonly(mnt)) {
        rc = -EROFS;
    } else if (!q && !inode_may_access(r->uid, r->gid, 0040000u | r->mode, 3, 0)) {
        rc = -EACCES;
    } else if (!q) {
        rc = mq_create(mnt, name, mode, attr, &q);
    } else if ((oflag & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        rc = -EEXIST;
    } else if (acc == 3) {
        rc = -EINVAL;
    } else if (acc != 0 && vfs_sb_rdonly(mnt)) {
        rc = -EROFS;
    } else if (!inode_may_access(q->uid, q->gid, q->mode, acc == 0 ? 4u : acc == 1 ? 2u : 6u, 0)) {
        rc = -EACCES;
    }
    if (rc == 0) {
        rc = mq_install(fd, q, oflag);
    }
    vfs_ns_unlock(mnt);
    if (rc < 0) {
        fd_free(current_proc->fds, fd);
    }
    return rc;
}

/**
 * mq_open(2) takes the name without its leading slash, as the C library
 * passes it, the flags, the mode and the attributes of a new queue, and
 * returns a descriptor open on the queue of that name in the caller's IPC
 * namespace. An existing queue is EEXIST under O_CREAT|O_EXCL and EACCES for
 * an access its bits do not give; a missing one is ENOENT without O_CREAT.
 * EINVAL for the access mode 3, and the refusals of `mq_name`, `mq_create`
 * and `mq_install`.
 */
long sys_mq_open(struct isr_params *params)
{
    const void __user *uattr = (const void __user *)SC_ARG(params, 3);
    struct mq_attr attr;
    char *name;
    int rc;

    if (uattr && mq_attr_get(uattr, &attr) != 0) {
        return -EFAULT;
    }
    rc = mq_name((const char __user *)SC_ARG(params, 0), &name);
    if (rc == 0) {
        rc = mq_open_named(name, (uint32_t)SC_ARG(params, 1), (uint32_t)SC_ARG(params, 2),
                           uattr ? &attr : NULL);
        path_buf_put(name);
    }
    return rc;
}

static int mqfs_unlink(struct vfs_mount *mnt, void *dir, const char *name);

/** Remove a name through the caller's private mount, under its root lock. */
long sys_mq_unlink(struct isr_params *params)
{
    char *name;
    struct vfs_mount *mnt = proc_ipcns(current_proc)->mq;
    int rc = mq_name((const char __user *)SC_ARG(params, 0), &name);

    if (rc == 0) {
        vfs_ns_lock(mnt);
        rc = vfs_mount_rdonly(mnt) ? -EROFS : mqfs_unlink(mnt, mnt->root, name);
        vfs_ns_unlock(mnt);
        path_buf_put(name);
    }
    return rc;
}

/* What a sender or a receiver sleeps for: a change of the queue, or the
 * deadline. `from` is the queue's `gen` when the sleep began; with `timed`,
 * the deadline is the time `sec` and `nsec` of real time, `target` its
 * tick and `ns` the time of the monotonic clock real time reaches it at, in
 * nanoseconds since boot, 0 for one past what those hold, all counted at
 * the clock's step count `steps` (`mq_count`). */
struct mq_wait {
    const struct mqueue *q;
    uint32_t from;
    int timed;
    uint64_t target;
    int64_t ns;
    uint32_t steps;
    int64_t sec;
    int64_t nsec;
};

/** Count the tick of `w`'s deadline from the real-time clock now, which
 * goes on as the ticks do, and its time of the monotonic clock, real time
 * less the offset, as Linux's hrtimer subtracts its base's offset
 * (kernel/time/hrtimer.c:922), after the clock's step count, which is read
 * first so that a setting between the readings leaves it behind; a time
 * already past is the tick now, and a time before boot 1 ns. */
static void mq_count(struct mq_wait *w)
{
    int64_t now, now_ns, dsec, dns, mono_sec;
    int32_t mono_nsec;

    w->steps = rtc_step_count();
    get_current_time(&now, &now_ns);
    dsec = w->sec - now;
    dns = w->nsec - now_ns;
    if (dns < 0) {
        dns += 1000000000;
        dsec--;
    }
    w->target = tick_count();
    if (dsec >= 0) {
        w->target += tick_span(dsec, (uint32_t)dns);
    }
    rtc_mono_of_real(w->sec, (int32_t)w->nsec, &mono_sec, &mono_nsec);
    if (mono_sec < 0) {
        w->ns = 1;
    } else if (mono_sec >= 9223372036ll) {
        w->ns = 0;
    } else {
        w->ns = mono_sec * 1000000000ll + mono_nsec > 0 ? mono_sec * 1000000000ll + mono_nsec : 1;
    }
}

/**
 * Whether `w`'s deadline has expired: the timer's pass found the monotonic
 * clock at its time `ns` when it woke the sleep, which the expiry record
 * `fired` (`wakeup_fired`) then names, or the clock has reached that time
 * with no step of real time since it was counted, the step count read after
 * the clock, which is then recorded the same way. The record holds across a
 * later step, which counts `ns` again, as Linux's hrtimer_wakeup clears the
 * sleeper's task and schedule_hrtimeout_range_clock then returns 0 to
 * wq_sleep (kernel/time/hrtimer.c, ipc/mqueue.c). Never for a deadline
 * without a time; called by the waiting thread.
 */
static int mq_fired(const struct mq_wait *w, int64_t fired)
{
    if (!w->timed || w->ns == 0) {
        return 0;
    }
    if (fired == w->ns) {
        return 1;
    }
    if (rtc_monotonic_ns() < w->ns || w->steps != rtc_step_count()) {
        return 0;
    }
    current_proc->wakeup_fired = w->ns;
    return 1;
}

/** The wait condition of the `struct mq_wait` at `arg`: a change, the
 * deadline's tick or time (`mq_fired`), or a setting of the clock since it
 * was counted. */
static int mq_wait_done(void *arg)
{
    const struct mq_wait *w = arg;

    return w->q->gen != w->from || mq_fired(w, current_proc->wakeup_fired) ||
           (w->timed && (tick_count() >= w->target || w->steps != rtc_step_count()));
}

/** Whether a signal the process acts on is pending, judged under the big
 * lock, which `signal_pending` needs and which is taken here when the
 * caller runs without it; at once 0 while no post has marked the thread
 * (`sig_woken`). An ignored signal is discarded there. */
static int mq_signal_pending(void)
{
    int big, pending;

    if (!*(volatile uint32_t *)&current_proc->sig_woken) {
        return 0;
    }
    big = !bkl_held();
    if (big) {
        bkl_enter();
    }
    pending = signal_pending();
    if (big) {
        bkl_exit();
    }
    return pending;
}

/**
 * Sleep on `q` for what `w` waits for: 0 for a change, -ETIMEDOUT at the
 * deadline, -EINTR for a signal, also one pending at the deadline, as
 * Linux's wq_sleep tests signal_pending before the timeout (ipc/mqueue.c).
 * A step of the real-time clock, a setting or a leap second, wakes the
 * sleep at the next tick (`rt_steps`), which counts the deadline's tick
 * again from the clock as it then is and sleeps on while the time is still
 * to come, as Linux's wq_sleep waits on an hrtimer of CLOCK_REALTIME that
 * clock_was_set moves (ipc/mqueue.c:728-729, kernel/time/hrtimer.c:
 * 951-987); so does the deadline's tick when the clock, read within the
 * tick, has not reached the time by it, since Linux's hrtimer never expires
 * before its clock reaches its time. The sleep ends with -ETIMEDOUT when
 * the monotonic clock reaches the time `ns`, by the interrupt between
 * ticks, as that hrtimer ends it, and not at the tick; once it has
 * (`mq_fired`), a later step does not count the time again. The expiry is
 * judged by the record the end of the timed wait reads under the run
 * queues' lock (`proc_wait_set`), since a pass on another processor may
 * hold that lock between its test of the time and its record of the expiry
 * while a step of the clock lets the thread leave its wait, as Linux's
 * schedule_hrtimeout_range_clock cancels its hrtimer, waiting out a running
 * callback, before it reads whether the timer expired.
 */
static int32_t mq_sleep(struct mqueue *q, struct mq_wait *w)
{
    int rc, fired, fresh = 1;

    w->from = q->gen;
    for (;;) {
        if (w->timed && w->steps != rtc_step_count()) {
            mq_count(w);
        }
        proc_wait_set(w->timed ? (w->target != 0 ? w->target : 1u) : 0, w->timed ? w->ns : 0,
                      w->timed ? w->steps : 0, fresh);
        fresh = 0;
        rc = wq_wait_event(&q->wq, mq_wait_done, w);
        fired = mq_fired(w, proc_wait_set(0, 0, 0, 0));
        if (q->gen != w->from) {
            return 0;
        }
        if (rc != 0 || mq_signal_pending()) {
            return -EINTR;
        }
        if (fired) {
            return -ETIMEDOUT;
        }
        if (w->steps == rtc_step_count()) {
            mq_count(w);
            if (tick_count() >= w->target) {
                return -ETIMEDOUT;
            }
        }
    }
}

/**
 * Set `w`'s deadline, a tick count, from the absolute real-time timeout at
 * `uts`: a timespec of two 64-bit fields when `wide`, else of two longs, 32
 * bits on i686 and 64 on x86-64; an i386 call on x86-64 has 32-bit longs,
 * and the high word of its 64-bit nanoseconds is ignored, as Linux's
 * get_timespec64 ignores it. No deadline when `uts` is NULL. A time
 * already past is a deadline of now. EINVAL for nanoseconds outside 0 to
 * 10^9-1 or negative seconds, as Linux checks it before it looks at the
 * queue; EFAULT for an address it cannot read.
 */
static int32_t mq_deadline(const void __user *uts, int wide, struct mq_wait *w)
{
    int64_t sec, nsec;
    long tsl[2];
    int64_t ts64[2];

    w->timed = uts != NULL;
    if (!uts) {
        return 0;
    }
    if (wide) {
        if (copy_from_user(ts64, uts, sizeof(ts64)) != 0) {
            return -EFAULT;
        }
        sec = ts64[0];
        nsec = ts64[1];
        if (mq_compat()) {
            nsec = (int64_t)(uint32_t)nsec; /* Linux ignores the high word */
        }
    } else if (mq_compat()) {
        int32_t ts32[2];

        if (copy_from_user(ts32, uts, sizeof(ts32)) != 0) {
            return -EFAULT;
        }
        sec = ts32[0];
        nsec = ts32[1];
    } else {
        if (copy_from_user(tsl, uts, sizeof(tsl)) != 0) {
            return -EFAULT;
        }
        sec = tsl[0];
        nsec = tsl[1];
    }
    if (sec < 0 || nsec < 0 || nsec >= 1000000000) {
        return -EINVAL;
    }
    w->sec = sec;
    w->nsec = nsec;
    mq_count(w);
    return 0;
}

/** Post the queue's notification, when a message has come to it empty and
 * no receiver waits for it: the signal its process asked for, with
 * SI_MESGQ, its value and the sender; the registration goes either way. */
static void mq_notify_now(struct mqueue *q)
{
    process_t *p;
    struct k_siginfo info;

    if (!q->notify_tgid || q->curmsgs != 1 || q->receivers != 0) {
        return;
    }
    p = get_process(q->notify_tgid);
    if (p && q->notify_how == SIGEV_SIGNAL) {
        k_memset(&info, 0, sizeof(info));
        info.code = SI_MESGQ;
        info.pid = pid_seen_by(p, current_proc->tgid);
        info.uid = ns_uid_from_kernel(proc_userns(p), current_proc->ruid);
        info.value = q->notify_value;
        info.status = (int32_t)q->notify_value;
        signal_post_info(p, q->notify_signo, &info);
    }
    q->notify_tgid = 0;
}

/** Put `m` in `q` after every message of its priority or a higher one,
 * and set the queue file's times to now, as Linux's mq_timedsend does. */
static void mq_enqueue(struct mqueue *q, struct mq_msg *m)
{
    struct mq_msg **link = &q->head;

    while (*link && (*link)->prio >= m->prio) {
        link = &(*link)->next;
    }
    m->next = *link;
    *link = m;
    q->curmsgs++;
    mq_touch(q);
}

/**
 * mq_timedsend: put `len` bytes at `umsg` of priority `prio` in the queue of
 * descriptor `fd`, waiting while it is full until the deadline at `uts`,
 * ETIMEDOUT after, or EAGAIN at once for a descriptor with O_NONBLOCK.
 * EBADF for a descriptor not open on a queue for writing, EMSGSIZE past the
 * queue's message size, EINVAL for a priority past MQ_PRIO_MAX, EINTR for a
 * signal, the refusals of `mq_deadline`, and ENOMEM and EFAULT. A wait
 * holds a reference to the queue.
 */
static int32_t mq_send(int fd, const void __user *umsg, uint32_t len, uint32_t prio,
                       const void __user *uts, int wide)
{
    struct mq_wait w;
    fd_entry_t *e;
    struct mqueue *q = mq_of(fd, &e);
    struct mq_msg *m;
    int32_t rc;

    if (!q || !(e->flags & FD_FLAG_WRITE)) {
        return -EBADF;
    }
    if (len > (uint32_t)q->msgsize) {
        return -EMSGSIZE;
    }
    if (prio >= MQ_PRIO_MAX) {
        return -EINVAL;
    }
    rc = mq_deadline(uts, wide, &w);
    if (rc != 0) {
        return rc;
    }
    m = kmalloc(sizeof(*m) + len);
    if (!m) {
        return -ENOMEM;
    }
    if (copy_from_user(m + 1, umsg, len) != 0) {
        kfree(m);
        return -EFAULT;
    }
    m->prio = prio;
    m->len = len;
    w.q = q;
    q->refs++;
    while (rc == 0 && q->curmsgs >= q->maxmsg) {
        rc = (e->ofd->status_flags & OFD_NONBLOCK) ? -EAGAIN : mq_sleep(q, &w);
    }
    if (rc == 0) {
        mq_enqueue(q, m);
        mq_notify_now(q);
        q->gen++;
        wq_wake_all(&q->wq);
    } else {
        kfree(m);
    }
    mq_put(q);
    return rc;
}

/**
 * mq_timedreceive: take the first message of the queue of descriptor `fd`,
 * the highest priority's oldest, into `umsg`, and its priority into `uprio`
 * unless NULL, waiting while the queue is empty as `mq_send` waits while it
 * is full. EBADF for a descriptor not open for reading, EMSGSIZE for a
 * buffer smaller than the queue's message size, EFAULT with the message
 * left queued. Returns the message's length.
 */
static int32_t mq_receive(int fd, void __user *umsg, uint32_t len, void __user *uprio,
                          const void __user *uts, int wide)
{
    struct mq_wait w;
    fd_entry_t *e;
    struct mqueue *q = mq_of(fd, &e);
    struct mq_msg *m;
    int32_t rc;

    if (!q || !(e->flags & FD_FLAG_READ)) {
        return -EBADF;
    }
    if (len < (uint32_t)q->msgsize) {
        return -EMSGSIZE;
    }
    rc = mq_deadline(uts, wide, &w);
    if (rc != 0) {
        return rc;
    }
    w.q = q;
    q->refs++;
    while (rc == 0 && !q->head) {
        if (e->ofd->status_flags & OFD_NONBLOCK) {
            rc = -EAGAIN;
            break;
        }
        q->receivers++;
        rc = mq_sleep(q, &w);
        q->receivers--;
    }
    m = rc == 0 ? q->head : NULL;
    if (m && (copy_to_user(umsg, m + 1, m->len) != 0 ||
              (uprio && copy_to_user(uprio, &m->prio, sizeof(m->prio)) != 0))) {
        rc = -EFAULT;
    } else if (m) {
        q->head = m->next;
        q->curmsgs--;
        mq_touch(q);
        rc = (int32_t)m->len;
        kfree(m);
        q->gen++;
        wq_wake_all(&q->wq);
    }
    mq_put(q);
    return rc;
}

/** mq_timedsend(2) takes the descriptor, the message, its length, its
 * priority and the timeout, a timespec of longs, of 32-bit ones from an
 * i386 call on x86-64 (`mq_send`). */
long sys_mq_timedsend(struct isr_params *params)
{
    return mq_send((int)SC_ARG(params, 0), (const void __user *)SC_ARG(params, 1),
                   USER_LEN32(SC_ARG(params, 2)), (uint32_t)SC_ARG(params, 3),
                   (const void __user *)SC_ARG(params, 4), 0);
}

/** mq_timedreceive(2) takes the descriptor, the buffer, its length, where
 * to store the priority and the timeout, a timespec of longs, of 32-bit
 * ones from an i386 call on x86-64 (`mq_receive`). */
long sys_mq_timedreceive(struct isr_params *params)
{
    return mq_receive((int)SC_ARG(params, 0), (void __user *)SC_ARG(params, 1),
                      USER_LEN32(SC_ARG(params, 2)), (void __user *)SC_ARG(params, 3),
                      (const void __user *)SC_ARG(params, 4), 0);
}

/** mq_timedsend_time64, i686's and an i386 program's on x86-64:
 * mq_timedsend with a timespec of two 64-bit fields. */
long sys_mq_timedsend_time64(struct isr_params *params)
{
    return mq_send((int)SC_ARG(params, 0), (const void __user *)SC_ARG(params, 1),
                   USER_LEN32(SC_ARG(params, 2)), (uint32_t)SC_ARG(params, 3),
                   (const void __user *)SC_ARG(params, 4), 1);
}

/** mq_timedreceive_time64, i686's and an i386 program's on x86-64:
 * mq_timedreceive with a timespec of two 64-bit fields. */
long sys_mq_timedreceive_time64(struct isr_params *params)
{
    return mq_receive((int)SC_ARG(params, 0), (void __user *)SC_ARG(params, 1),
                      USER_LEN32(SC_ARG(params, 2)), (void __user *)SC_ARG(params, 3),
                      (const void __user *)SC_ARG(params, 4), 1);
}

/**
 * mq_notify(2) takes the descriptor and a `struct sigevent`. With a null
 * one it takes the caller's registration away; else it registers the
 * caller for the queue's next message as the sigevent says, a signal or
 * nothing. EBUSY when a process is registered already, EINVAL for
 * SIGEV_THREAD, whose notice Linux sends through a netlink socket this
 * kernel does not have, for another kind or for a signal outside 1 to 64;
 * EBADF for a descriptor not open on a queue, EFAULT. An i386 call on
 * x86-64 passes Linux's compat_sigevent, whose value is 32 bits.
 */
long sys_mq_notify(struct isr_params *params)
{
    struct {
        uintptr_t value;
        int32_t signo;
        int32_t notify;
    } ev; /* the head of Linux's struct sigevent */
    struct {
        uint32_t value;
        int32_t signo;
        int32_t notify;
    } ev32; /* an i386 call's, compat_sigevent's */
    fd_entry_t *e;
    struct mqueue *q = mq_of((int)SC_ARG(params, 0), &e);

    if (!q) {
        return -EBADF;
    }
    if (!SC_ARG(params, 1)) {
        if (q->notify_tgid == current_proc->tgid) {
            q->notify_tgid = 0;
        }
        return 0;
    }
    if (mq_compat()) {
        if (copy_from_user(&ev32, (const void __user *)SC_ARG(params, 1), sizeof(ev32)) != 0) {
            return -EFAULT;
        }
        ev.value = ev32.value;
        ev.signo = ev32.signo;
        ev.notify = ev32.notify;
    } else if (copy_from_user(&ev, (const void __user *)SC_ARG(params, 1), sizeof(ev)) != 0) {
        return -EFAULT;
    }
    if ((ev.notify != SIGEV_SIGNAL && ev.notify != SIGEV_NONE) ||
        (ev.notify == SIGEV_SIGNAL && (ev.signo < 1 || ev.signo > KSIG_MAX))) {
        return -EINVAL;
    }
    if (q->notify_tgid) {
        return -EBUSY;
    }
    q->notify_tgid = current_proc->tgid;
    q->notify_how = ev.notify;
    q->notify_signo = ev.signo;
    q->notify_value = ev.value;
    return 0;
}

/**
 * mq_getsetattr: the queue's attributes into the `struct mq_attr` the third
 * argument points at unless it is null, with the description's O_NONBLOCK
 * as the flags; then the flags from the one the second points at unless it
 * is null, of which only O_NONBLOCK may be set, else EINVAL. EBADF for a
 * descriptor not open on a queue, EFAULT. Both records are in the
 * caller's layout (`mq_attr_get`).
 */
long sys_mq_getsetattr(struct isr_params *params)
{
    struct mq_attr a;
    fd_entry_t *e;
    struct mqueue *q = mq_of((int)SC_ARG(params, 0), &e);

    if (!q) {
        return -EBADF;
    }
    if (SC_ARG(params, 1)) {
        if (mq_attr_get((const void __user *)SC_ARG(params, 1), &a) != 0) {
            return -EFAULT;
        }
        if (a.flags & ~O_NONBLOCK) {
            return -EINVAL;
        }
    }
    if (SC_ARG(params, 2)) {
        struct mq_attr old;

        k_memset(&old, 0, sizeof(old));
        old.flags = (e->ofd->status_flags & OFD_NONBLOCK) ? O_NONBLOCK : 0;
        old.maxmsg = q->maxmsg;
        old.msgsize = q->msgsize;
        old.curmsgs = q->curmsgs;
        if (mq_attr_put((void __user *)SC_ARG(params, 2), &old) != 0) {
            return -EFAULT;
        }
    }
    if (SC_ARG(params, 1)) {
        fd_ofd_status_set(e->ofd, OFD_NONBLOCK, (a.flags & O_NONBLOCK) ? OFD_NONBLOCK : 0u);
    }
    return 0;
}

void mq_ns_gone(const struct ipcns *ns)
{
    struct mq_root *r;

    if (!ns->mq) {
        return;
    }
    r = ns->mq->root;
    r->namespace_live = 0;
    if (vfs_mount_put(ns->mq) == 0) {
        vfs_reap_detached();
    }
}

#define MQFS_MAGIC   0x19800202u
#define MQFS_S_IFDIR 0040000u
#define MQFS_S_IFREG 0100000u

int mq_root_made(struct ipcns *ns)
{
    struct mq_root *r = kzalloc(sizeof(*r));

    if (!r) {
        return -ENOMEM;
    }
    r->mode = 01777u;
    r->uid = current_proc ? proc_fsuid(current_proc) : 0;
    r->gid = current_proc ? proc_fsgid(current_proc) : 0;
    r->namespace_live = r->mounted = 1;
    r->entries = 2;
    mq_root_stamp(r);
    ns->mq = vfs_kernel_mount(&vfs_mqueue_type, r, VFS_MS_NOEXEC | VFS_MS_NODEV);
    if (!ns->mq) {
        kfree(r);
        return -ENOMEM;
    }
    ns->mq->sb_noexec = 1;
    return 0;
}

/** Look `name` up in the root: `.` and `..` are the root, another name the
 * queue of that name, held for the caller; -ENOENT for no such queue,
 * -ENOTDIR under a queue. */
static int mqfs_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    struct mqueue *q;

    if (dir && dir != mnt->root) {
        return -ENOTDIR;
    }
    if (name_is(name, ".") || name_is(name, "..")) {
        *node = mnt->root;
        return 0;
    }
    q = mq_find(mnt, name);
    if (!q) {
        return -ENOENT;
    }
    q->refs++;
    *node = q;
    return 0;
}

/** Take a reference to a queue's node; the root needs none. */
static void mqfs_hold(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    if (node != mnt->root) {
        ((struct mqueue *)node)->refs++;
    }
}

/** Drop a reference to a queue's node, which frees the queue with its
 * last (`mq_put`). */
static void mqfs_put(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    if (node != mnt->root) {
        mq_put((struct mqueue *)node);
    }
}

/** Report the queue or directory inode. The queue's inode size is
 * independent of its messages and of the status line returned by read. */
static int mqfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    const struct mqueue *q = (const struct mqueue *)node;

    k_memset(st, 0, sizeof(*st));
    if (node == mnt->root) {
        const struct mq_root *r = mnt->root;

        st->mode = MQFS_S_IFDIR | r->mode;
        st->nlink = 2;
        st->ino = 1;
        st->size = (uint64_t)r->entries * 20;
        st->uid = r->uid;
        st->gid = r->gid;
        st->atime_sec = r->atime_s;
        st->atime_nsec = r->atime_ns;
        st->mtime_sec = r->mtime_s;
        st->mtime_nsec = r->mtime_ns;
        st->ctime_sec = r->ctime_s;
        st->ctime_nsec = r->ctime_ns;
        return 0;
    }
    st->mode = MQFS_S_IFREG | q->mode;
    st->nlink = q->unlinked ? 0u : 1u;
    st->uid = q->uid;
    st->gid = q->gid;
    st->ino = q->ino;
    st->size = q->size;
    st->atime_sec = q->atime_s;
    st->atime_nsec = q->atime_ns;
    st->mtime_sec = q->mtime_s;
    st->mtime_nsec = q->mtime_ns;
    st->ctime_sec = q->ctime_s;
    st->ctime_nsec = q->ctime_ns;
    return 0;
}

/**
 * The `setattr` of the mqueue file system, as Linux's simple_setattr, which
 * mqueue's inodes take: the permission bits of `st->mode`, the owner, the
 * group and the access and modification times of a queue's file or the
 * root, as `what` selects, and the change time set to now for every call, a
 * VFS_SET_CTIME call with nothing else among them. The caller has judged
 * the permission. A size or inode flags are -EINVAL and -EOPNOTSUPP, and
 * change nothing; 0 otherwise.
 */
static int mqfs_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st)
{
    uint32_t *mode, *uid, *gid;
    int64_t *at_s, *at_ns, *mt_s, *mt_ns, *ct_s, *ct_ns;

    if (what & VFS_SET_FLAGS) {
        return -EOPNOTSUPP;
    }
    if (what & VFS_SET_SIZE) {
        return -EINVAL;
    }
    if (node == mnt->root) {
        struct mq_root *r = mnt->root;

        mode = &r->mode;
        uid = &r->uid;
        gid = &r->gid;
        at_s = &r->atime_s;
        at_ns = &r->atime_ns;
        mt_s = &r->mtime_s;
        mt_ns = &r->mtime_ns;
        ct_s = &r->ctime_s;
        ct_ns = &r->ctime_ns;
    } else {
        struct mqueue *q = (struct mqueue *)node;

        mode = &q->mode;
        uid = &q->uid;
        gid = &q->gid;
        at_s = &q->atime_s;
        at_ns = &q->atime_ns;
        mt_s = &q->mtime_s;
        mt_ns = &q->mtime_ns;
        ct_s = &q->ctime_s;
        ct_ns = &q->ctime_ns;
    }
    if (what & VFS_SET_MODE) {
        *mode = st->mode & 07777u;
    }
    if (what & VFS_SET_UID) {
        *uid = st->uid;
    }
    if (what & VFS_SET_GID) {
        *gid = st->gid;
    }
    if (what & VFS_SET_ATIME) {
        *at_s = st->atime_sec;
        *at_ns = st->atime_nsec;
    }
    if (what & VFS_SET_MTIME) {
        *mt_s = st->mtime_sec;
        *mt_ns = st->mtime_nsec;
    }
    get_current_time(ct_s, ct_ns);
    return 0;
}

/** Create a named queue through its directory and return a held inode. */
static int mqfs_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode,
                       void **node)
{
    struct mqueue *q;
    int rc;

    if (dir && dir != mnt->root)
        return -ENOTDIR;
    if (mq_find(mnt, name))
        return -EEXIST;
    rc = mq_create(mnt, name, mode, NULL, &q);
    if (rc == 0) {
        q->refs++;
        *node = q;
    }
    return rc;
}

/** An open of a node: the handle is the node, held while it is open. */
static int mqfs_open(struct vfs_mount *mnt, void *node, int flags, void **handle)
{
    (void)flags;
    mqfs_hold(mnt, node);
    *handle = node;
    return 0;
}

/** Set the inode size of a queue. Its messages and status line stay intact. */
static int mqfs_truncate(struct vfs_mount *mnt, void *handle, uint64_t size, int stamp_times)
{
    struct mqueue *q = handle;

    if (handle == mnt->root) {
        return -EISDIR;
    }
    if (size > 0x7FFFFFFFu) {
        return -EFBIG;
    }
    q->size = (uint32_t)size;
    if (stamp_times) {
        get_current_time(&q->ctime_s, &q->ctime_ns);
        q->mtime_s = q->ctime_s;
        q->mtime_ns = q->ctime_ns;
    }
    return 0;
}

/** The end of an open, which drops its hold. */
static void mqfs_release(struct vfs_mount *mnt, void *handle)
{
    mqfs_put(mnt, handle);
}

/** A queue's status line, Linux's `QSIZE:... NOTIFY:... SIGNO:...
 * NOTIFY_PID:...`, from `offset`; 0 past its end, -EISDIR for the root. */
static int32_t mqfs_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count,
                         uint64_t offset)
{
    char text[96];
    int n;

    (void)mnt;
    if (handle == mnt->root) {
        return -EISDIR;
    }
    n = mq_status((const struct mqueue *)handle, text, (int)sizeof(text));
    if (offset >= (uint64_t)n) {
        return 0;
    }
    if (count > (uint32_t)n - (uint32_t)offset) {
        count = (uint32_t)n - (uint32_t)offset;
    }
    k_memcpy(buf, text + offset, count);
    return (int32_t)count;
}

/** `.` and `..`, then the namespace's queues, one entry a call in `ent`;
 * the cursor counts entries. 1 for an entry, 0 at the end, -ENOTDIR for a
 * queue. */
static int mqfs_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor,
                        struct vfs_dirent *ent)
{
    struct mqueue *q;
    uint32_t i = 2;

    if (handle != mnt->root) {
        return -ENOTDIR;
    }
    k_memset(ent, 0, sizeof(*ent));
    if (*cursor < 2) {
        ent->name[0] = '.';
        ent->name[1] = *cursor == 1 ? '.' : '\0';
        ent->ino = 1;
        ent->is_dir = 1;
        ent->type = VFS_DT_DIR;
        (*cursor)++;
        return 1;
    }
    for (q = queues; q; q = q->next) {
        if (q->mnt != mnt) {
            continue;
        }
        if (i++ == *cursor) {
            k_strncpy(ent->name, q->name, VFS_NAME_MAX);
            ent->ino = q->ino;
            ent->type = VFS_DT_REG;
            (*cursor)++;
            return 1;
        }
    }
    return 0;
}

/** Remove a name after the root's search, write and sticky checks. */
static int mqfs_unlink(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct mqueue *q;
    const struct mq_root *r = mnt->root;
    uint32_t uid = proc_fsuid(current_proc);
    int rc;

    if (dir && dir != mnt->root) {
        return -ENOTDIR;
    }
    rc = mq_lookup_allowed(r, name);
    if (rc != 0) {
        return rc;
    }
    q = mq_find(mnt, name);
    if (!q) {
        return -ENOENT;
    }
    if (!inode_may_access(r->uid, r->gid, 0040000u | r->mode, 3, 0)) {
        return -EACCES;
    }
    if ((r->mode & 01000u) && uid != r->uid && uid != q->uid &&
        !inode_capable(q->uid, q->gid, CAP_FOWNER)) {
        return -EPERM;
    }
    mq_unname(q);
    return 0;
}

/** The statfs of a mount: Linux's magic, a block of 4096 bytes, and the
 * longest name. */
static int mqfs_statfs(struct vfs_mount *mnt, struct vfs_statfs *st)
{
    (void)mnt;
    k_memset(st, 0, sizeof(*st));
    st->magic = MQFS_MAGIC;
    st->bsize = 4096;
    st->namelen = MQ_NAME_MAX;
    return 0;
}

int mq_fd_statfs(const struct fd_entry *e, struct vfs_statfs *st)
{
    if (!e || e->ops != &mq_ops) {
        return 0;
    }
    return mqfs_statfs(NULL, st) == 0;
}

int mq_fd_ref(const struct fd_entry *e, struct vfs_ref *r)
{
    if (!e || e->ops != &mq_ops) {
        return 0;
    }
    if (r) {
        r->mnt = e->ofd->vmnt;
        r->node = e->ofd->vnode;
    }
    return 1;
}

const char *mq_fd_name(const struct fd_entry *e, int *unlinked)
{
    const struct mqueue *q;

    if (!e || e->ops != &mq_ops) {
        return NULL;
    }
    q = e->ofd->vnode;
    *unlinked = q->unlinked;
    return q->name;
}

/** A mount lists the queues of its mounter's IPC namespace, the first one's
 * for the permanent mount the kernel makes at boot. */
static int mqfs_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                      const char *options)
{
    struct ipcns *ns = current_proc ? proc_ipcns(current_proc) : &init_ipcns;
    int rc;

    (void)dev;
    (void)flags;
    (void)options;
    if (!ns->mq) {
        rc = mq_root_made(ns);
        if (rc != 0) {
            return rc;
        }
    }
    mnt->bind_of = ns->mq;
    mnt->root = ns->mq->root;
    k_strncpy(mnt->bind_root, "/", sizeof(mnt->bind_root));
    vfs_mount_get(ns->mq);
    return 0;
}

/** Free the names and directory of a private mount after its last hold. */
static void mqfs_unmount(struct vfs_mount *mnt)
{
    struct mqueue *q = queues;
    struct mq_root *r = mnt->root;

    while (q) {
        struct mqueue *next = q->next;

        if (q->mnt == mnt) {
            mq_unname(q);
        }
        q = next;
    }
    r->mounted = 0;
    if (r->queues == 0) {
        kfree(r);
    }
}

/** Accept changes to generic superblock flags. Mqueue has no mount options. */
static int mqfs_remount(struct vfs_mount *mnt, uint32_t flags, const char *options)
{
    (void)mnt;
    (void)flags;
    return options && *options ? -EINVAL : 0;
}

static const struct vfs_ops mqfs_ops = {
    mqfs_lookup,
    0, /* lookup_rest */
    mqfs_hold,
    mqfs_put,
    mqfs_getattr,
    mqfs_setattr,
    mqfs_open,
    mqfs_release,
    mqfs_read,
    0, /* write */
    mqfs_truncate,
    0, /* fsync */
    mqfs_readdir,
    mqfs_create,
    mqfs_unlink,
    0, /* mkdir */
    0, /* rmdir */
    0, /* rename */
    0, /* link */
    0, /* readlink */
    mqfs_statfs,
    0, /* sync */
    0, /* prepare_unmount */
    0, /* abort_unmount */
    mqfs_unmount,
    0, /* path: the one directory does not move */
    0, /* symlink */
    0, /* mknod */
    0, /* pin: a hold keeps a node */
    0, /* unpin */
    mqfs_remount,
    0, /* fallocate */
    0, /* fdatasync */
    0, /* lock_node */
    0, /* unlock_node */
    0, /* shutdown */
    0, /* xattr */
    0, /* cachestat */
    0, /* put_unless_last */
    0, /* readlink_cached */
    0, /* read_frames */
    0, /* handle_node */
    0, /* writeback */
    0, /* lock_write */
    0, /* unlock_write */
    0, /* rename_flags */
    0, /* tmpfile */
    0, /* uncache */
    0, /* phase */
    0, /* data_at */
    0, /* check_acl */
};

const struct vfs_type vfs_mqueue_type = {"mqueue", mqfs_mount, &mqfs_ops,      0, 1, 0, 0, 0,
                                         0,        0,          VFS_DIRECT_NONE};
