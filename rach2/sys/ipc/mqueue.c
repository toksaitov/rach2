/*
 * mqueue.c: POSIX message queues, as Linux gives them through mq_open and
 * its five calls (ipc/Readme.md).
 *
 * A queue has a name in one list, which every process shares, until
 * mq_unlink, and lives while a descriptor or the name has it. A descriptor
 * is an anonymous one whose object refers to the queue; its access mode is
 * the descriptor's read and write flags and its O_NONBLOCK the description's.
 * Messages are kept highest priority first, in the order they came within a
 * priority. A sender or a receiver that waits holds a reference, so the
 * queue outlives its last close while one sleeps on it.
 *
 * The mqueue file system lists the named queues of one IPC namespace as the
 * files of a directory, as Linux's /dev/mqueue does: a file's attributes are
 * its queue's, reading it gives the queue's status line, and unlinking it is
 * mq_unlink.
 */

#include "mqueue.h"

#include "../irq.h"
#include "../proc/process.h"
#include "../proc/tgroup.h"
#include "../proc/ns.h"
#include "../proc/wait_queue.h"
#include "../mm/mm.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../fs/krffs.h"
#include "../fs/vfs.h"
#include "../io/anon.h"
#include "../io/poll.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/ksignal.h"
#include "../lib/kstring.h"
#include "../syscall_internal.h"

#define MQ_NAME_MAX 255
#define MQ_PRIO_MAX 32768u
#define MQ_QUEUES   256

/* The limits of /proc/sys/fs/mqueue: an unprivileged caller's largest
 * queue, and the queue mq_open makes without attributes. */
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
#define SI_MESGQ     (-3)

struct mq_msg {
    struct mq_msg *next;
    uint32_t prio;
    uint32_t len;
};

struct mqueue {
    struct mqueue *next;    /* the list of named queues */
    char *name;             /* NULL once unlinked */
    const struct ipcns *ns; /* the IPC namespace whose name it is */
    uint32_t refs;
    uint32_t ino;  /* its file's number in the mqueue file system */
    int64_t ctime; /* when it was made, the file's times */
    uint32_t uid, gid, mode;
    int32_t maxmsg, msgsize, curmsgs;
    struct mq_msg *head;
    wait_queue_t wq;
    uint32_t gen;
    uint32_t receivers;   /* receivers waiting, to whom a message goes before a notification */
    uint32_t notify_tgid; /* the process registered by mq_notify, 0 for none */
    int notify_how;
    int notify_signo;
    int32_t notify_value;
};

/* A descriptor's object: the queue it was opened on. */
struct mq_file {
    struct anon_obj anon;
    struct mqueue *q;
};

/* i386's struct mq_attr. */
struct mq_attr {
    int32_t flags, maxmsg, msgsize, curmsgs;
    int32_t reserved[4];
};

static struct mqueue *queues;
static int nqueues;
static uint32_t next_ino = 2;

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
    kfree(q->name);
    kfree(q);
    nqueues--;
}

/** The queue named `name` in the caller's IPC namespace, or NULL. */
static struct mqueue *mq_find(const char *name)
{
    struct mqueue *q;
    int i;

    for (q = queues; q; q = q->next) {
        for (i = 0; q->name[i] && q->name[i] == name[i]; i++) {
        }
        if (q->name[i] == name[i] && q->ns == proc_ipcns(current_proc)) {
            return q;
        }
    }
    return NULL;
}

/** Take `q`'s name away: out of the list, the reference the name held
 * dropped. */
static void mq_unname(struct mqueue *q)
{
    struct mqueue **link = &queues;

    while (*link != q) {
        link = &(*link)->next;
    }
    *link = q->next;
    kfree(q->name);
    q->name = NULL;
    mq_put(q);
}

/** Whether the caller may open `q` for the access mode `acc`: the owner's,
 * the group's or the others' bits, or a privileged caller. */
static int mq_may(const struct mqueue *q, int acc)
{
    uint32_t want = acc == 0 ? 4u : acc == 1 ? 2u : 6u;
    uint32_t bits = q->mode;
    uint32_t i;

    if (current_proc->euid == q->uid) {
        bits >>= 6;
    } else {
        int in = q->gid == current_proc->egid;

        for (i = 0; !in && i < current_proc->ngroups; i++) {
            in = current_proc->groups[i] == q->gid;
        }
        if (in) {
            bits >>= 3;
        }
    }
    return (want & ~bits & 7u) == 0 || proc_privileged(current_proc);
}

static int32_t mq_read(fd_entry_t *e, char __user *buf, uint32_t count);
static short mq_poll(fd_entry_t *e, short events, struct wait_queue **wq);
static void mq_close(fd_entry_t *e);

static const struct file_ops mq_ops = {mq_read, NULL, mq_poll, NULL, mq_close};

/** The queue the caller's descriptor `fd` is open on, or NULL. */
static struct mqueue *mq_of(int fd, fd_entry_t **ep)
{
    fd_entry_t *e = fd_get(current_proc->fds, fd);

    if (!e || e->ops != &mq_ops) {
        return NULL;
    }
    *ep = e;
    return ((struct mq_file *)e->priv)->q;
}

/**
 * The text a read of the descriptor gives, as Linux's mqueue file has it:
 * the bytes of the messages queued, the notification's kind, its signal and
 * the process registered.
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

static int32_t mq_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    char text[96];
    int n = mq_status(((struct mq_file *)e->priv)->q, text, (int)sizeof(text) - 1);
    uint32_t at = (uint32_t)e->ofd->offset;

    if (at >= (uint32_t)n) {
        return 0;
    }
    if (count > (uint32_t)n - at) {
        count = (uint32_t)n - at;
    }
    if (copy_to_user(buf, text + at, count) != 0) {
        return -EFAULT;
    }
    e->ofd->offset += count;
    return (int32_t)count;
}

static short mq_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct mqueue *q = ((struct mq_file *)e->priv)->q;
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

/** The last close of a description: the notification goes if the closing
 * process registered it, as Linux removes it, and the queue is put. */
static void mq_close(fd_entry_t *e)
{
    struct mq_file *f = e->priv;

    if (current_proc && f->q->notify_tgid == current_proc->tgid) {
        f->q->notify_tgid = 0;
    }
    mq_put(f->q);
    kfree(f);
    e->priv = NULL;
}

/** The attributes a new queue takes from `uattr`, or the defaults without
 * one: EINVAL for a count or a size below 1 or past the caller's limit. */
static int32_t mq_new_attr(const void __user *uattr, int32_t *maxmsg, int32_t *msgsize)
{
    struct mq_attr a;
    int priv = proc_privileged(current_proc);

    *maxmsg = MQ_MSG_MAX;
    *msgsize = MQ_MSGSIZE_MAX;
    if (!uattr) {
        return 0;
    }
    if (copy_from_user(&a, uattr, sizeof(a)) != 0) {
        return -EFAULT;
    }
    if (a.maxmsg <= 0 || a.msgsize <= 0 || a.maxmsg > (priv ? MQ_HARD_MSGMAX : MQ_MSG_MAX) ||
        a.msgsize > (priv ? MQ_HARD_MSGSIZEMAX : MQ_MSGSIZE_MAX)) {
        return -EINVAL;
    }
    *maxmsg = a.maxmsg;
    *msgsize = a.msgsize;
    return 0;
}

/** A new queue `name`, the caller's, with the bits of `mode` its umask
 * leaves, entered in the list with the reference its name holds. */
static int32_t mq_create(const char *name, uint32_t mode, const void __user *uattr,
                         struct mqueue **out)
{
    struct mqueue *q;
    int32_t maxmsg, msgsize;
    int32_t rc = mq_new_attr(uattr, &maxmsg, &msgsize);

    if (rc != 0) {
        return rc;
    }
    if (nqueues >= MQ_QUEUES && !proc_privileged(current_proc)) {
        return -ENFILE;
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
    q->ns = proc_ipcns(current_proc);
    q->refs = 1;
    q->ino = next_ino++;
    {
        int64_t ns;

        get_current_time(&q->ctime, &ns);
    }
    q->uid = current_proc->euid;
    q->gid = current_proc->egid;
    q->mode = mode & 0777u & ~current_proc->fs->umask;
    q->maxmsg = maxmsg;
    q->msgsize = msgsize;
    wait_queue_init(&q->wq);
    q->next = queues;
    queues = q;
    nqueues++;
    *out = q;
    return 0;
}

/** Copy the queue name at `uname` into `name`: ENAMETOOLONG past
 * MQ_NAME_MAX, ENOENT for an empty one, EACCES for one with a slash. */
static int32_t mq_name(const char __user *uname, char *name)
{
    int32_t n = strncpy_from_user(name, uname, MQ_NAME_MAX + 1);

    if (n < 0) {
        return n == -ENAMETOOLONG ? n : -EFAULT;
    }
    if (n > MQ_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    if (n == 0) {
        return -ENOENT;
    }
    while (n-- > 0) {
        if (name[n] == '/') {
            return -EACCES;
        }
    }
    return 0;
}

/** A descriptor open on `q` for the access mode of `oflag`, with its
 * O_NONBLOCK and O_CLOEXEC; it takes a reference to `q`. */
static int32_t mq_install(struct mqueue *q, uint32_t oflag)
{
    struct mq_file *f = kzalloc(sizeof(*f));
    int acc = (int)(oflag & O_ACCMODE);
    fd_entry_t *e;
    int fd;

    if (!f) {
        return -ENOMEM;
    }
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(f);
        return fd;
    }
    f->anon.name = "mqueue";
    f->q = q;
    q->refs++;
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_ANON;
    e->ops = &mq_ops;
    e->priv = f;
    e->flags = (acc != 1 ? FD_FLAG_READ : 0u) | (acc != 0 ? FD_FLAG_WRITE : 0u) |
               ((oflag & O_CLOEXEC) ? FD_FLAG_CLOEXEC : 0u);
    if (oflag & O_NONBLOCK) {
        e->ofd->status_flags |= OFD_NONBLOCK;
    }
    return fd;
}

/**
 * mq_open, number 277: EBX the name without its leading slash, as the C
 * library passes it, ECX the flags, EDX the mode and ESI the attributes of
 * a new queue. An existing queue is EEXIST under O_CREAT|O_EXCL and EACCES
 * for an access its bits do not give; a missing one is ENOENT without
 * O_CREAT. EINVAL for the access mode 3, ENFILE past MQ_QUEUES queues for an
 * unprivileged caller, and the refusals of `mq_name` and `mq_new_attr`.
 */
long sys_mq_open(struct isr_params *params)
{
    char name[MQ_NAME_MAX + 1];
    uint32_t oflag = (uint32_t)SC_ARG(params, 1);
    struct mqueue *q;
    int created = 0;
    int32_t rc;

    if ((oflag & O_ACCMODE) == 3) {
        return -EINVAL;
    }
    rc = mq_name((const char __user *)SC_ARG(params, 0), name);
    if (rc != 0) {
        return rc;
    }
    q = mq_find(name);
    if (q && (oflag & O_CREAT) && (oflag & O_EXCL)) {
        return -EEXIST;
    }
    if (!q && !(oflag & O_CREAT)) {
        return -ENOENT;
    }
    if (q && !mq_may(q, (int)(oflag & O_ACCMODE))) {
        return -EACCES;
    }
    if (!q) {
        rc = mq_create(name, (uint32_t)SC_ARG(params, 2), (const void __user *)SC_ARG(params, 3), &q);
        if (rc != 0) {
            return rc;
        }
        created = 1;
    }
    rc = mq_install(q, oflag);
    if (rc < 0 && created) {
        mq_unname(q);
    }
    return rc;
}

/** mq_unlink, number 278: take the queue's name away; it lives while a
 * descriptor has it. ENOENT for no such queue, EACCES for a caller that
 * neither owns it nor is privileged, as the sticky directory of Linux's
 * mqueue mount has it. */
long sys_mq_unlink(struct isr_params *params)
{
    char name[MQ_NAME_MAX + 1];
    struct mqueue *q;
    int32_t rc = mq_name((const char __user *)SC_ARG(params, 0), name);

    if (rc != 0) {
        return rc;
    }
    q = mq_find(name);
    if (!q) {
        return -ENOENT;
    }
    if (q->uid != current_proc->euid && !proc_privileged(current_proc)) {
        return -EACCES;
    }
    mq_unname(q);
    return 0;
}

/* What a sender or a receiver sleeps for: a change of the queue, or the
 * deadline. */
struct mq_wait {
    const struct mqueue *q;
    uint32_t from;
    int timed;
    uint32_t target;
};

static int mq_wait_done(void *arg)
{
    const struct mq_wait *w = arg;

    return w->q->gen != w->from || (w->timed && (int32_t)(kernel_ticks - w->target) >= 0);
}

/** Sleep on `q` for what `w` waits for: 0 for a change, -ETIMEDOUT at the
 * deadline, -EINTR for a signal. */
static int32_t mq_sleep(struct mqueue *q, struct mq_wait *w)
{
    int rc;

    w->from = q->gen;
    current_proc->wakeup_tick = w->timed ? (w->target != 0 ? w->target : 1u) : 0;
    rc = wq_wait_event(&q->wq, mq_wait_done, w);
    current_proc->wakeup_tick = 0;
    if (q->gen != w->from) {
        return 0;
    }
    return rc != 0 ? -EINTR : -ETIMEDOUT;
}

/**
 * The deadline of the absolute real-time timeout at `uts`, a timespec of
 * 64-bit seconds when `wide` and of 32-bit ones else, as ticks from now in
 * `w`; no deadline without one. EINVAL for nanoseconds outside 0 to 10^9-1
 * or negative seconds, as Linux checks it before it looks at the queue.
 */
static int32_t mq_deadline(const void __user *uts, int wide, struct mq_wait *w)
{
    int64_t sec, nsec, now, now_ns, dsec, dns;
    int32_t ts32[2];
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
    } else {
        if (copy_from_user(ts32, uts, sizeof(ts32)) != 0) {
            return -EFAULT;
        }
        sec = ts32[0];
        nsec = ts32[1];
    }
    if (sec < 0 || nsec < 0 || nsec >= 1000000000) {
        return -EINVAL;
    }
    get_current_time(&now, &now_ns);
    dsec = sec - now;
    dns = nsec - now_ns;
    if (dns < 0) {
        dns += 1000000000;
        dsec--;
    }
    w->target = kernel_ticks;
    if (dsec >= 0) {
        w->target += (uint32_t)(dsec > 21474836 ? 21474836 : dsec) * 100u +
                     ((uint32_t)dns + 9999999u) / 10000000u;
    }
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
        info.status = q->notify_value;
        signal_post_info(p, q->notify_signo, &info);
    }
    q->notify_tgid = 0;
}

/** Put `m` in `q` after every message of its priority or a higher one. */
static void mq_enqueue(struct mqueue *q, struct mq_msg *m)
{
    struct mq_msg **link = &q->head;

    while (*link && (*link)->prio >= m->prio) {
        link = &(*link)->next;
    }
    m->next = *link;
    *link = m;
    q->curmsgs++;
}

/**
 * mq_timedsend: put `len` bytes at `umsg` of priority `prio` in the queue of
 * descriptor `fd`, waiting while it is full until the deadline at `uts`,
 * ETIMEDOUT after, or EAGAIN at once for a descriptor with O_NONBLOCK.
 * EBADF for a descriptor not open on a queue for writing, EMSGSIZE past the
 * queue's message size, EINVAL for a priority past MQ_PRIO_MAX.
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
 * buffer smaller than the queue's message size. Returns the message's
 * length.
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
        rc = (int32_t)m->len;
        kfree(m);
        q->gen++;
        wq_wake_all(&q->wq);
    }
    mq_put(q);
    return rc;
}

long sys_mq_timedsend(struct isr_params *params)
{
    return mq_send((int)SC_ARG(params, 0), (const void __user *)SC_ARG(params, 1), USER_LEN32(SC_ARG(params, 2)), (uint32_t)SC_ARG(params, 3),
                   (const void __user *)SC_ARG(params, 4), 0);
}

long sys_mq_timedreceive(struct isr_params *params)
{
    return mq_receive((int)SC_ARG(params, 0), (void __user *)SC_ARG(params, 1), USER_LEN32(SC_ARG(params, 2)),
                      (void __user *)SC_ARG(params, 3), (const void __user *)SC_ARG(params, 4), 0);
}

long sys_mq_timedsend_time64(struct isr_params *params)
{
    return mq_send((int)SC_ARG(params, 0), (const void __user *)SC_ARG(params, 1), USER_LEN32(SC_ARG(params, 2)), (uint32_t)SC_ARG(params, 3),
                   (const void __user *)SC_ARG(params, 4), 1);
}

long sys_mq_timedreceive_time64(struct isr_params *params)
{
    return mq_receive((int)SC_ARG(params, 0), (void __user *)SC_ARG(params, 1), USER_LEN32(SC_ARG(params, 2)),
                      (void __user *)SC_ARG(params, 3), (const void __user *)SC_ARG(params, 4), 1);
}

/**
 * mq_notify, number 281: with ECX NULL, take the caller's registration
 * away; else register the caller for the queue's next message as the
 * `struct sigevent` at ECX says, a signal or nothing. EBUSY when another
 * process is registered, EINVAL for SIGEV_THREAD, whose notice Linux sends
 * through a netlink socket this kernel does not have, for another kind or
 * for a signal outside 1 to 64; EBADF for a descriptor not open on a queue.
 */
long sys_mq_notify(struct isr_params *params)
{
    int32_t ev[3];
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
    if (copy_from_user(ev, (const void __user *)SC_ARG(params, 1), sizeof(ev)) != 0) {
        return -EFAULT;
    }
    if ((ev[2] != SIGEV_SIGNAL && ev[2] != SIGEV_NONE) ||
        (ev[2] == SIGEV_SIGNAL && (ev[1] < 1 || ev[1] > KSIG_MAX))) {
        return -EINVAL;
    }
    if (q->notify_tgid) {
        return -EBUSY;
    }
    q->notify_tgid = current_proc->tgid;
    q->notify_how = ev[2];
    q->notify_signo = ev[1];
    q->notify_value = ev[0];
    return 0;
}

/**
 * mq_getsetattr, number 282: the queue's attributes into the `struct
 * mq_attr` at EDX unless NULL, with the description's O_NONBLOCK as the
 * flags; then the flags from the one at ECX unless NULL, of which only
 * O_NONBLOCK may be set, else EINVAL.
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
        if (copy_from_user(&a, (const void __user *)SC_ARG(params, 1), sizeof(a)) != 0) {
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
        if (copy_to_user((void __user *)SC_ARG(params, 2), &old, sizeof(old)) != 0) {
            return -EFAULT;
        }
    }
    if (SC_ARG(params, 1)) {
        if (a.flags & O_NONBLOCK) {
            e->ofd->status_flags |= OFD_NONBLOCK;
        } else {
            e->ofd->status_flags &= ~(uint32_t)OFD_NONBLOCK;
        }
    }
    return 0;
}

void mq_ns_gone(const struct ipcns *ns)
{
    struct mqueue *q = queues;

    while (q) {
        struct mqueue *next = q->next;

        if (q->ns == ns) {
            mq_unname(q);
        }
        q = next;
    }
}

/* The mqueue file system. */

#define MQFS_MAGIC   0x19800202u
#define MQFS_S_IFDIR 0040000u
#define MQFS_S_IFREG 0100000u

/* The mount's root directory, the one node that is not a queue. */
static char mqfs_root;

/** Whether the strings `a` and `b` are the same. */
static int name_is(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** The IPC namespace whose queues the mount lists. */
static const struct ipcns *mqfs_ns(const struct vfs_mount *mnt)
{
    return (const struct ipcns *)mnt->priv;
}

/** The queue named `name` in the mount's namespace, or NULL. */
static struct mqueue *mqfs_find(const struct vfs_mount *mnt, const char *name)
{
    struct mqueue *q;

    for (q = queues; q; q = q->next) {
        if (q->ns == mqfs_ns(mnt) && name_is(q->name, name)) {
            return q;
        }
    }
    return NULL;
}

static int mqfs_lookup(struct vfs_mount *mnt, void *dir, const char *name, void **node)
{
    struct mqueue *q;

    if (dir && dir != &mqfs_root) {
        return -ENOTDIR;
    }
    if (name_is(name, ".") || name_is(name, "..")) {
        *node = &mqfs_root;
        return 0;
    }
    q = mqfs_find(mnt, name);
    if (!q) {
        return -ENOENT;
    }
    q->refs++;
    *node = q;
    return 0;
}

static void mqfs_hold(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    if (node != &mqfs_root) {
        ((struct mqueue *)node)->refs++;
    }
}

static void mqfs_put(struct vfs_mount *mnt, void *node)
{
    (void)mnt;
    if (node != &mqfs_root) {
        mq_put((struct mqueue *)node);
    }
}

/** A queue's file: its owner and mode, and as its size the bytes of its
 * messages; the root: a directory every user may add to, sticky, 1777. */
static int mqfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st)
{
    const struct mqueue *q = (const struct mqueue *)node;
    const struct mq_msg *m;

    (void)mnt;
    k_memset(st, 0, sizeof(*st));
    st->nlink = 1;
    if (node == &mqfs_root) {
        st->mode = MQFS_S_IFDIR | 01777u;
        st->nlink = 2;
        st->ino = 1;
        return 0;
    }
    st->mode = MQFS_S_IFREG | q->mode;
    st->uid = q->uid;
    st->gid = q->gid;
    st->ino = q->ino;
    for (m = q->head; m; m = m->next) {
        st->size += m->len;
    }
    st->atime_sec = st->mtime_sec = st->ctime_sec = q->ctime;
    return 0;
}

static int mqfs_open(struct vfs_mount *mnt, void *node, int flags, void **handle)
{
    (void)flags;
    mqfs_hold(mnt, node);
    *handle = node;
    return 0;
}

static void mqfs_release(struct vfs_mount *mnt, void *handle)
{
    mqfs_put(mnt, handle);
}

/** A queue's status line, Linux's `QSIZE:... NOTIFY:... SIGNO:...
 * NOTIFY_PID:...`, from `offset`. */
static int32_t mqfs_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count,
                         uint64_t offset)
{
    char text[96];
    int n;

    (void)mnt;
    if (handle == &mqfs_root) {
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

/** `.` and `..`, then the namespace's queues; the cursor counts entries. */
static int mqfs_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor,
                        struct vfs_dirent *ent)
{
    struct mqueue *q;
    uint32_t i = 2;

    if (handle != &mqfs_root) {
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
        if (q->ns != mqfs_ns(mnt)) {
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

/** mq_unlink by the file's name: the queue's owner, or a privileged caller,
 * since the directory is sticky. */
static int mqfs_unlink(struct vfs_mount *mnt, void *dir, const char *name)
{
    struct mqueue *q;

    if (dir && dir != &mqfs_root) {
        return -ENOTDIR;
    }
    q = mqfs_find(mnt, name);
    if (!q) {
        return -ENOENT;
    }
    if (current_proc->euid != q->uid && !proc_privileged(current_proc)) {
        return -EPERM;
    }
    mq_unname(q);
    return 0;
}

static int mqfs_statfs(struct vfs_mount *mnt, struct vfs_statfs *st)
{
    (void)mnt;
    k_memset(st, 0, sizeof(*st));
    st->magic = MQFS_MAGIC;
    st->bsize = 4096;
    st->namelen = MQ_NAME_MAX;
    return 0;
}

/** A mount lists the queues of its mounter's IPC namespace, the first one's
 * for the permanent mount the kernel makes at boot. */
static int mqfs_mount(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                      const char *options)
{
    (void)dev;
    (void)flags;
    (void)options;
    mnt->priv = (void *)(current_proc ? proc_ipcns(current_proc) : &init_ipcns);
    mnt->root = &mqfs_root;
    return 0;
}

static void mqfs_unmount(struct vfs_mount *mnt)
{
    mnt->priv = 0;
}

static const struct vfs_ops mqfs_ops = {
    mqfs_lookup,
    0, /* lookup_rest */
    mqfs_hold,
    mqfs_put,
    mqfs_getattr,
    0, /* setattr */
    mqfs_open,
    mqfs_release,
    mqfs_read,
    0, /* write */
    0, /* truncate */
    0, /* fsync */
    mqfs_readdir,
    0, /* create */
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
    0, /* remount */
    0, /* fallocate */
};

const struct vfs_type vfs_mqueue_type = {"mqueue", mqfs_mount, &mqfs_ops, 0, 1};
