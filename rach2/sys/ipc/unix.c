/*
 * unix.c: AF_UNIX sockets, as unix.h describes them.
 */

#include "unix.h"

#include "../irq.h"
#include "../fs/file.h"
#include "../fs/krffs.h"
#include "../fs/vfs.h"
#include "../proc/process.h"
#include "../proc/ns.h"
#include "../proc/wait_queue.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/ksignal.h"
#include "../lib/kstring.h"
#include "../network/socket/socket.h"
#include "../syscall_internal.h"

#define SOCK_CLOEXEC  02000000
#define SOCK_NONBLOCK 04000
#define AF_UNSPEC     0

/* Format: where sun_path starts in a sockaddr_un. */
#define SUN_PATH_OFF 2

/* Policy: the bytes a socket's receive queue takes before a sender waits,
 * Linux's default buffer size, and the datagrams it takes, Linux's
 * net.unix.max_dgram_qlen. A stream send is queued in pieces of at most
 * UNIX_CHUNK bytes, so a reader drains the first while the rest waits. */
#define UNIX_BUF        212992u
#define UNIX_DGRAM_QLEN 10u
#define UNIX_CHUNK      65536u
/* Format: the descriptors one message carries at most, Linux's SCM_MAX_FD. */
#define UNIX_MAX_FDS    253u
/* Policy: the connections a listener queues at most, Linux's somaxconn. */
#define UNIX_SOMAXCONN  4096

#define MSG_OOB          0x1
#define MSG_PEEK         0x2
#define MSG_CTRUNC       0x8
#define MSG_TRUNC        0x20
#define MSG_DONTWAIT     0x40
#define MSG_WAITALL      0x100
#define MSG_NOSIGNAL     0x4000
#define MSG_CMSG_CLOEXEC 0x40000000

#define SOL_SOCKET     1
#define SO_REUSEADDR   2
#define SO_TYPE        3
#define SO_ERROR       4
#define SO_BROADCAST   6
#define SO_SNDBUF      7
#define SO_RCVBUF      8
#define SO_KEEPALIVE   9
#define SO_LINGER      13
#define SO_PASSCRED    16
#define SO_PEERCRED    17
#define SO_RCVLOWAT    18
#define SO_SNDLOWAT    19
#define SO_RCVTIMEO    20
#define SO_SNDTIMEO    21
#define SO_ACCEPTCONN  30
#define SO_SNDBUFFORCE 32
#define SO_RCVBUFFORCE 33
#define SO_PROTOCOL    38
#define SO_DOMAIN      39

#define SCM_RIGHTS      1
#define SCM_CREDENTIALS 2

#define POLLIN     0x001
#define POLLOUT    0x004
#define POLLERR    0x008
#define POLLHUP    0x010
#define POLLRDNORM 0x040
#define POLLWRNORM 0x100
#define POLLRDHUP  0x2000

#define TIOCOUTQ 0x5411
#define FIONREAD 0x541B

/* A socket's shutdown bits: it receives nothing more, it sends nothing
 * more. */
#define SHUT_RCV  1
#define SHUT_SEND 2
#define SHUT_BOTH 3

enum { US_UNCONNECTED, US_LISTENING, US_CONNECTED };

struct unix_cred {
    uint32_t pid;
    uint32_t uid;
    uint32_t gid;
};

/* A name as a sockaddr_un has it: `len` bytes of sun_path, 0 for none. An
 * abstract name begins with a NUL byte; a path is kept without its NUL,
 * with one after it. */
struct unix_name {
    uint32_t len;
    char path[UNIX_PATH_MAX + 1];
};

/* A message on a receive queue. A stream reader takes it `off` bytes at a
 * time; the others take it whole. `fds` are the descriptors it carries, each
 * holding its open file as a descriptor in a table would. */
struct unix_msg {
    struct unix_msg *next;
    uint32_t len;
    uint32_t off;
    struct unix_cred cred;
    struct unix_name from;
    uint32_t nfds;
    fd_entry_t *fds;
    uint8_t *data;
};

/*
 * A socket. `refs` counts the descriptors' side, one reference however many
 * descriptors share it; each socket connected to this one; and the accept
 * queue it waits on before accept. The descriptors' side is let go at the
 * last close, and the record is freed when the count reaches zero.
 */
struct unix_sock {
    struct unix_sock *next;
    /* The network namespace it was made in, whose abstract names it alone
     * finds (proc/ns.h). */
    const struct netns *ns;
    uint32_t refs;
    int type;
    int state;
    int closed;
    uint32_t inode;
    uint32_t uid;
    /* The name bind gave it, or for an accepted socket the listener's name,
     * which it reports; only a `bound` socket is found by its name. */
    struct unix_name name;
    int bound;
    int bound_fs;
    struct vfs_ref fs_ref;
    uint32_t fs_ino;
    struct unix_sock *peer;
    struct unix_msg *rq_head;
    struct unix_msg *rq_tail;
    uint32_t rq_bytes;
    uint32_t rq_count;
    int shut;
    int err;
    int passcred;
    struct unix_cred peercred;
    int has_peercred;
    int backlog;
    struct unix_sock *acc_head;
    struct unix_sock *acc_tail;
    struct unix_sock *acc_next;
    uint32_t acc_len;
    uint32_t sndbuf;
    uint32_t rcvbuf;
    uint32_t rcvtimeo; /* ticks, 0 for none */
    uint32_t sndtimeo;
    wait_queue_t wq;
    /* A bind in progress, which holds the socket across the file system's
     * sleeps (`bind_lock`); a receive, which holds its queue from choosing a
     * message to consuming it (`recv_lock`); and the queue the waiters for
     * either sleep on. */
    int binding;
    int reading;
    wait_queue_t lock_wq;
};

/* What a send carries besides its bytes. */
struct unix_out {
    uint32_t nfds;
    fd_entry_t *fds;
    int has_cred;
    struct unix_cred cred;
};

/* What a receive gives besides its bytes. */
struct unix_in {
    struct unix_name from;
    struct unix_cred cred;
    int has_cred;
    uint32_t nfds;
    fd_entry_t *fds;
    uint32_t flags;          /* MSG_TRUNC, MSG_CTRUNC */
    struct unix_in *transit; /* the next on the transit list */
};

static int32_t unix_read(fd_entry_t *e, char __user *buf, uint32_t count);
static int32_t unix_write(fd_entry_t *e, const char __user *buf, uint32_t count);
static short unix_poll(fd_entry_t *e, short events, struct wait_queue **wq);
static int32_t unix_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg);
static void unix_close(fd_entry_t *e);

static const struct file_ops unix_ops = {unix_read, unix_write, unix_poll, unix_ioctl, unix_close};

static struct unix_sock *socks;

/* The messages whose descriptors are being closed, first to last, and
 * whether a call is closing them now (`msg_free`). */
static struct unix_msg *reap_head;
static struct unix_msg *reap_tail;
static int reaping;

/* The receives in progress, whose descriptors are off their queue and not
 * yet installed or closed (`transit_enter`). */
static struct unix_in *transits;

void unix_init(void)
{
    socks = NULL;
    reap_head = NULL;
    reap_tail = NULL;
    reaping = 0;
    transits = NULL;
}

static fd_table_t *table(void)
{
    return current_proc->fds;
}

static struct unix_sock *sock_of(const fd_entry_t *e)
{
    return (struct unix_sock *)e->priv;
}

static int nonblocking(const fd_entry_t *e, int flags)
{
    return (flags & MSG_DONTWAIT) || (e->ofd && (e->ofd->status_flags & OFD_NONBLOCK));
}

int unix_inode_used(uint32_t ino)
{
    const struct unix_sock *s;

    for (s = socks; s; s = s->next) {
        if (s->inode == ino) {
            return 1;
        }
    }
    return 0;
}

int unix_count(void)
{
    const struct unix_sock *s;
    int n = 0;

    for (s = socks; s; s = s->next) {
        n += !s->closed;
    }
    return n;
}

int unix_snapshot(struct unix_row **rows)
{
    const struct unix_sock *s;
    struct unix_row *out;
    int n = 0, room = unix_count();

    *rows = NULL;
    if (room == 0) {
        return 0;
    }
    out = kmalloc(sizeof(*out) * (size_t)room);
    if (!out) {
        return -1;
    }
    for (s = socks; s && n < room; s = s->next) {
        if (s->closed || s->ns != proc_netns(current_proc)) {
            continue;
        }
        out[n].refs = s->refs;
        out[n].type = s->type;
        out[n].listening = s->state == US_LISTENING;
        out[n].connected = s->state == US_CONNECTED;
        out[n].inode = s->inode;
        out[n].uid = s->uid;
        out[n].peer = s->peer ? s->peer->inode : 0;
        out[n].rqueue = s->state == US_LISTENING ? s->acc_len : s->rq_bytes;
        out[n].wqueue = s->state == US_LISTENING ? (uint32_t)s->backlog
                        : s->peer                ? s->peer->rq_bytes
                                                 : 0;
        out[n].name_len = s->name.len;
        k_memcpy(out[n].path, s->name.path, s->name.len);
        out[n].path[s->name.len] = '\0';
        n++;
    }
    *rows = out;
    return n;
}

uint32_t unix_inode(const fd_entry_t *e)
{
    return sock_of(e)->inode;
}

static struct unix_sock *unix_alloc(int type)
{
    struct unix_sock *s = kzalloc(sizeof(*s));

    if (!s) {
        return NULL;
    }
    s->refs = 1;
    s->type = type;
    s->ns = proc_netns(current_proc);
    s->state = US_UNCONNECTED;
    s->inode = socket_inode_next();
    s->uid = current_proc->euid;
    s->sndbuf = UNIX_BUF;
    s->rcvbuf = UNIX_BUF;
    wait_queue_init(&s->wq);
    wait_queue_init(&s->lock_wq);
    s->next = socks;
    socks = s;
    return s;
}

/* The caller's credentials: the effective ids SO_PEERCRED reports, or the
 * real ids a message carries unless the sender names others. */
static void own_cred(struct unix_cred *c, int effective)
{
    c->pid = current_proc->tgid;
    c->uid = effective ? current_proc->euid : current_proc->ruid;
    c->gid = effective ? current_proc->egid : current_proc->rgid;
}

/* Credentials of kernel ids as the current process sees them in its user
 * and PID namespaces. */
static struct unix_cred cred_seen(struct unix_cred c)
{
    c.pid = pid_seen(c.pid);
    c.uid = uid_seen(c.uid);
    c.gid = gid_seen(c.gid);
    return c;
}

/* Take the references a descriptor in a table holds, for a copy of it that
 * goes in flight; and drop them, as a close does, for one that never
 * arrives. */
static void flight_hold(fd_entry_t *e)
{
    fd_ofd_ref(e->ofd);
    if (e->type == FD_TYPE_SOCKET && e->sock) {
        socket_ref(e->sock);
    }
}

/**
 * Bring a KRFFS file or directory in flight to the node its description
 * names now.
 *
 * A write that grows a KRFFS file can move its node, and `vfs_repoint`
 * then repoints the descriptors in tables and the descriptions, but not the
 * copies of descriptors in flight. The description is what a copy shares
 * with every other descriptor of the open, so its node is the one to use
 * before a copy is released or installed.
 */
static void flight_sync(fd_entry_t *e)
{
    if ((e->type == FD_TYPE_FILE || e->type == FD_TYPE_DIR) && e->node && e->ofd && e->ofd->vnode) {
        e->node = (struct krffs_node *)e->ofd->vnode;
    }
}

/**
 * Close the descriptor in flight at `slot`, as a close of it in a table
 * would, and take it from where `unix_inflight_holds` finds it: the slot is
 * emptied, or, when `count` is not NULL, `slot` is the last of the `*count`
 * a message carries and the count is lowered. It is closed from a copy,
 * taken from its place first, since the close can sleep and the one being
 * closed is no holder of its own file. A descriptor of a file of the KRFFS
 * root leaves its place only once the root's namespace lock is held
 * (`fd_release_lock`): a release of the file's last descriptor elsewhere
 * counts it until then, and the count and any reclaim of the file its own
 * close makes come in the same hold.
 */
static void flight_drop(fd_entry_t *slot, uint32_t *count)
{
    struct vfs_mount *locked = fd_release_lock(slot);
    fd_entry_t e = *slot;

    if (count) {
        (*count)--;
    } else {
        slot->type = FD_TYPE_NONE;
    }
    flight_sync(&e);
    fd_release_backing(&e);
    fd_ofd_unref(e.ofd);
    if (locked) {
        vfs_ns_unlock(locked);
    }
}

/*
 * Close the descriptors in flight `fds[from..n)` and free the array. The ones
 * after the descriptor being closed stay where `unix_inflight_holds` finds
 * them while its close sleeps (`flight_drop`).
 */
static void drop_fds(fd_entry_t *fds, uint32_t from, uint32_t n)
{
    uint32_t i;

    for (i = from; i < n; i++) {
        flight_drop(&fds[i], NULL);
    }
    kfree(fds);
}

/**
 * Put the receive `in` on the transit list, before it takes descriptors
 * off a queue, and take it off with `transit_leave` once they are
 * installed or closed.
 *
 * A descriptor in flight holds its KRFFS file as a descriptor in a table
 * does, and the holder count looks for it on the queues; taken off its
 * queue, it is on neither, while installing it or closing the rest can
 * sleep, and a close of the file's last descriptor elsewhere would free a
 * file it still holds. `unix_inflight_holds` looks here too.
 */
static void transit_enter(struct unix_in *in)
{
    in->transit = transits;
    transits = in;
}

static void transit_leave(struct unix_in *in)
{
    struct unix_in **link = &transits;

    while (*link && *link != in) {
        link = &(*link)->transit;
    }
    if (*link) {
        *link = in->transit;
    }
}

static struct unix_msg *msg_alloc(uint32_t len)
{
    struct unix_msg *m = kmalloc(sizeof(*m) + len);

    if (!m) {
        return NULL;
    }
    k_memset(m, 0, sizeof(*m));
    m->len = len;
    m->data = (uint8_t *)(m + 1);
    return m;
}

/**
 * Free the message `m` and close the descriptors it carries.
 *
 * Closing a descriptor in flight can close an AF_UNIX socket, whose queue
 * can carry the only descriptor of another, and so on down a chain as long
 * as its senders made it. Freeing each queue inside the close of the socket
 * before it would take kernel stack for every socket in the chain, so a
 * message with descriptors goes on the reap list instead, and the outermost
 * call closes them one at a time, from the list, while the closes it makes
 * add to the list and return. A close can sleep; a call that frees a
 * message meanwhile only adds it to the list, for the call at work to
 * close. A message stays on the list, and its descriptors not yet closed
 * stay where `unix_inflight_holds` finds them, until the last is closed.
 */
static void msg_free(struct unix_msg *m)
{
    if (m->nfds == 0) {
        kfree(m->fds);
        kfree(m);
        return;
    }
    m->next = NULL;
    if (reap_tail) {
        reap_tail->next = m;
    } else {
        reap_head = m;
    }
    reap_tail = m;
    if (reaping) {
        return;
    }
    reaping = 1;
    while (reap_head) {
        m = reap_head;
        if (m->nfds > 0) {
            flight_drop(&m->fds[m->nfds - 1], &m->nfds);
            continue;
        }
        reap_head = m->next;
        if (!reap_head) {
            reap_tail = NULL;
        }
        kfree(m->fds);
        kfree(m);
    }
    reaping = 0;
}

/* Whether one of the `n` descriptors in flight at `fds` is the KRFFS file
 * `node`, which its description names (`flight_sync`). */
static int fds_hold(const fd_entry_t *fds, uint32_t n, const void *node)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        const fd_entry_t *e = &fds[i];

        if (e->type == FD_TYPE_FILE && e->node && e->ofd && e->ofd->vnode == node) {
            return 1;
        }
    }
    return 0;
}

/* Whether a message of the chain from `m` carries a descriptor of the KRFFS
 * file `node`. */
static int chain_holds(const struct unix_msg *m, const void *node)
{
    for (; m; m = m->next) {
        if (fds_hold(m->fds, m->nfds, node)) {
            return 1;
        }
    }
    return 0;
}

int unix_inflight_holds(const void *node)
{
    const struct unix_sock *s;
    const struct unix_in *in;

    for (s = socks; s; s = s->next) {
        if (chain_holds(s->rq_head, node)) {
            return 1;
        }
    }
    for (in = transits; in; in = in->transit) {
        if (fds_hold(in->fds, in->nfds, node)) {
            return 1;
        }
    }
    return chain_holds(reap_head, node);
}

static int queue_full(const struct unix_sock *s)
{
    if (s->type == SOCK_STREAM) {
        return s->rq_bytes >= s->rcvbuf;
    }
    return s->rq_count > UNIX_DGRAM_QLEN || s->rq_bytes >= s->rcvbuf;
}

static void enqueue(struct unix_sock *s, struct unix_msg *m)
{
    m->next = NULL;
    if (s->rq_tail) {
        s->rq_tail->next = m;
    } else {
        s->rq_head = m;
    }
    s->rq_tail = m;
    s->rq_bytes += m->len;
    s->rq_count++;
    wq_wake(&s->wq, WQ_DATA);
}

/**
 * Wake whoever waits for room in `s`'s queue: a sender to it waits on its
 * queue, where the room changes none of `s`'s own events, and a socket
 * connected to it polls on its own, where it is room to write.
 */
static void wake_writers(struct unix_sock *s)
{
    struct unix_sock *o;

    wq_wake(&s->wq, 0);
    for (o = socks; o; o = o->next) {
        if (o->peer == s) {
            wq_wake(&o->wq, WQ_ROOM);
        }
    }
}

/* Take the head message off `s`'s queue, with whatever of it was not read;
 * the caller frees it. */
static struct unix_msg *dequeue(struct unix_sock *s)
{
    struct unix_msg *m = s->rq_head;
    int was_full = queue_full(s);

    s->rq_head = m->next;
    if (!s->rq_head) {
        s->rq_tail = NULL;
    }
    s->rq_bytes -= m->len - m->off;
    s->rq_count--;
    if (was_full && !queue_full(s)) {
        wake_writers(s);
    }
    return m;
}

static void purge(struct unix_sock *s)
{
    while (s->rq_head) {
        msg_free(dequeue(s));
    }
}

static void unix_put(struct unix_sock *s)
{
    struct unix_sock **link = &socks;

    if (--s->refs != 0) {
        return;
    }
    purge(s);
    while (*link && *link != s) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = s->next;
    }
    kfree(s);
}

/* Forget the name: an abstract one is free at once, and the socket file of
 * a path stays until it is removed, as in Linux. */
static void unbind(struct unix_sock *s)
{
    if (s->bound_fs) {
        fs_socket_unbind(&s->fs_ref);
        s->bound_fs = 0;
    }
    s->bound = 0;
}

static void unix_release(struct unix_sock *s, int embryo);

/* A listener's queued connections, never accepted, end as if closed. */
static void drop_embryos(struct unix_sock *s)
{
    while (s->acc_head) {
        struct unix_sock *c = s->acc_head;

        s->acc_head = c->acc_next;
        c->acc_next = NULL;
        s->acc_len--;
        unix_release(c, 1);
    }
    s->acc_tail = NULL;
}

/**
 * The end of the descriptors' side of `s`, at its last close, or with
 * `embryo` of a connection never accepted.
 *
 * A connected peer sees the end of input; unread data, or a connection that
 * was never accepted, gives it ECONNRESET as well, as in Linux. The queue is
 * dropped now, so the descriptors in flight on it are closed now. The
 * reference the descriptors' side had is put last, so the record outlives
 * whatever those closes do.
 */
static void unix_release(struct unix_sock *s, int embryo)
{
    struct unix_sock *peer = s->peer;

    s->closed = 1;
    s->shut = SHUT_BOTH;
    unbind(s);
    if (s->state == US_LISTENING) {
        drop_embryos(s);
    }
    if (peer) {
        s->peer = NULL;
        if (s->type != SOCK_DGRAM && peer->peer == s) {
            peer->shut = SHUT_BOTH;
            if (s->rq_head || embryo) {
                peer->err = ECONNRESET;
            }
        }
        wq_wake_all(&peer->wq);
        unix_put(peer);
    }
    purge(s);
    wake_writers(s);
    unix_put(s);
}

/* The wait of `unix_wait`: its condition, or the socket's time limit. */
struct timed_cond {
    wq_cond_fn cond;
    void *arg;
    uint32_t deadline;
    int timed;
};

static int timed_ready(void *arg)
{
    struct timed_cond *t = (struct timed_cond *)arg;

    return t->cond(t->arg) || (t->timed && (int32_t)(t->deadline - kernel_ticks) <= 0);
}

/**
 * Wait on `wq` until `cond(arg)`, a signal, or the end of the socket time
 * limit `timeo` in ticks, 0 for none. Returns 0 when the condition holds,
 * -EINTR for a signal, -EAGAIN when the time ran out, as Linux reports it.
 */
static int unix_wait(wait_queue_t *wq, wq_cond_fn cond, void *arg, uint32_t timeo)
{
    struct timed_cond t;
    int rc;

    t.cond = cond;
    t.arg = arg;
    t.timed = timeo != 0;
    t.deadline = kernel_ticks + timeo + 1u;
    if (t.timed) {
        current_proc->wakeup_tick = t.deadline;
    }
    rc = wq_wait_event(wq, timed_ready, &t);
    if (t.timed) {
        current_proc->wakeup_tick = 0;
    }
    if (rc < 0) {
        return -EINTR;
    }
    return cond(arg) ? 0 : -EAGAIN;
}

/**
 * Reserve the lowest free descriptor for a socket a call is making: 0 or
 * more, or `fd_alloc`'s -EMFILE for a full table or -ENOMEM for a heap with
 * no room.
 *
 * A reserved slot has no type yet, so no other thread sharing the table can
 * look it up, close it or duplicate over it. The call copies the number to
 * the caller, which may sleep in a page fault, and only then gives the slot
 * its socket with `publish`; a copy that fails frees the slot with
 * `fd_free`, and nothing else can have used it.
 */
static int reserve(void)
{
    return fd_alloc(table());
}

/* Give the reserved descriptor `fd` the socket `s`, which takes the
 * reference the caller had, with the creation `flags` SOCK_CLOEXEC and
 * SOCK_NONBLOCK. */
static void publish(int fd, struct unix_sock *s, int flags)
{
    fd_entry_t *e = &table()->entries[fd];

    e->node = NULL;
    e->sock = NULL;
    e->ops = &unix_ops;
    e->priv = s;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    if (flags & SOCK_CLOEXEC) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    if (flags & SOCK_NONBLOCK) {
        e->ofd->status_flags |= OFD_NONBLOCK;
    }
    e->type = FD_TYPE_UNIX;
}

static int32_t check_type(int *type, int protocol)
{
    if (*type == SOCK_RAW) {
        *type = SOCK_DGRAM; /* as Linux takes it */
    }
    if (*type != SOCK_STREAM && *type != SOCK_DGRAM && *type != SOCK_SEQPACKET) {
        return -ESOCKTNOSUPPORT;
    }
    if (protocol != 0 && protocol != AF_UNIX) {
        return -EPROTONOSUPPORT;
    }
    return 0;
}

int32_t unix_socket(int type, int protocol, int flags)
{
    struct unix_sock *s;
    int32_t rc = check_type(&type, protocol);
    int fd;

    if (rc != 0) {
        return rc;
    }
    s = unix_alloc(type);
    if (!s) {
        return -ENOMEM;
    }
    fd = reserve();
    if (fd < 0) {
        unix_put(s);
        return fd;
    }
    publish(fd, s, flags);
    return fd;
}

/* Join `a` and `b` as the two ends of one connection. */
static void join(struct unix_sock *a, struct unix_sock *b)
{
    a->peer = b;
    b->refs++;
    b->peer = a;
    a->refs++;
    a->state = US_CONNECTED;
    b->state = US_CONNECTED;
}

/**
 * socketpair(2). The two descriptors are reserved and their numbers copied
 * to `sv` before either is published, so a failed copy leaves nothing that
 * another thread could have used, and both sockets are released.
 */
int32_t unix_socketpair(int type, int protocol, int flags, uintptr_t sv)
{
    struct unix_sock *a, *b;
    int32_t rc = check_type(&type, protocol);
    int fds[2];

    if (rc != 0) {
        return rc;
    }
    a = unix_alloc(type);
    b = a ? unix_alloc(type) : NULL;
    if (!b) {
        if (a) {
            unix_put(a);
        }
        return -ENOMEM;
    }
    join(a, b);
    own_cred(&a->peercred, 1);
    b->peercred = a->peercred;
    a->has_peercred = 1;
    b->has_peercred = 1;
    fds[0] = reserve();
    fds[1] = fds[0] >= 0 ? reserve() : fds[0];
    rc = fds[1] < 0 ? fds[1] : 0;
    if (rc == 0 && copy_to_user((void __user *)sv, fds, sizeof(fds)) != 0) {
        rc = -EFAULT;
    }
    if (rc != 0) {
        if (fds[1] >= 0) {
            fd_free(table(), fds[1]);
        }
        if (fds[0] >= 0) {
            fd_free(table(), fds[0]);
        }
        unix_release(a, 0);
        unix_release(b, 0);
        return rc;
    }
    publish(fds[0], a, flags);
    publish(fds[1], b, flags);
    return 0;
}

/**
 * Copy a sockaddr_un from the caller into `n`. An address of the family
 * alone gives an empty name; a path ends at its first NUL or at the end of
 * the address. EINVAL for a length or a family that is not AF_UNIX's.
 */
static int32_t get_name(uintptr_t addr, uint32_t addrlen, struct unix_name *n)
{
    uint8_t sa[SUN_PATH_OFF + UNIX_PATH_MAX];
    uint32_t i;

    if (addrlen < SUN_PATH_OFF || addrlen > sizeof(sa)) {
        return -EINVAL;
    }
    if (copy_from_user(sa, (const void __user *)addr, addrlen) != 0) {
        return -EFAULT;
    }
    if ((sa[0] | (sa[1] << 8)) != AF_UNIX) {
        return -EINVAL;
    }
    k_memset(n, 0, sizeof(*n));
    n->len = addrlen - SUN_PATH_OFF;
    k_memcpy(n->path, sa + SUN_PATH_OFF, n->len);
    if (n->len > 0 && n->path[0] != '\0') {
        for (i = 0; i < n->len && n->path[i] != '\0'; i++) {
        }
        n->len = i;
    }
    return 0;
}

static int abstract(const struct unix_name *n)
{
    return n->len > 0 && n->path[0] == '\0';
}

static int same_name(const struct unix_name *a, const struct unix_name *b)
{
    uint32_t i;

    if (a->len != b->len) {
        return 0;
    }
    for (i = 0; i < a->len && a->path[i] == b->path[i]; i++) {
    }
    return i == a->len;
}

/* The length of the sockaddr_un a name is reported in: the family alone
 * for no name, and a path with its NUL. */
static uint32_t name_addrlen(const struct unix_name *n)
{
    if (n->len == 0) {
        return SUN_PATH_OFF;
    }
    return SUN_PATH_OFF + n->len + (abstract(n) ? 0u : 1u);
}

/**
 * Report a name through the caller's address and length: as much of the
 * address as the caller's length has room for, and the true length. A
 * receive reports a sender with no name as a length of 0.
 */
static int32_t put_name(uintptr_t addr, uintptr_t lenp, const struct unix_name *n, int sender)
{
    uint8_t sa[SUN_PATH_OFF + UNIX_PATH_MAX + 1];
    uint32_t full = (sender && n->len == 0) ? 0u : name_addrlen(n);
    int32_t given;

    if (copy_from_user(&given, (const void __user *)lenp, sizeof(given)) != 0) {
        return -EFAULT;
    }
    if (given < 0) {
        return -EINVAL;
    }
    k_memset(sa, 0, sizeof(sa));
    sa[0] = AF_UNIX;
    k_memcpy(sa + SUN_PATH_OFF, n->path, n->len);
    if (addr && given > 0 && full > 0 &&
        copy_to_user((void __user *)addr, sa, (uint32_t)given < full ? (uint32_t)given : full) !=
            0) {
        return -EFAULT;
    }
    if (copy_to_user((void __user *)lenp, &full, sizeof(full)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * The live socket bound to `n`, with a reference the caller puts, or NULL
 * with the reason in `*err`. An abstract name is found among the sockets of
 * `type` of the caller's network namespace, as in Linux; a path is found
 * through its socket file, of any type.
 */
static struct unix_sock *lookup(const struct unix_name *n, int type, int32_t *err)
{
    struct unix_sock *s;
    struct vfs_mount *mnt = NULL;
    uint32_t ino = 0;

    if (!abstract(n)) {
        int32_t rc = fs_socket_lookup(n->path, &mnt, &ino);

        if (rc != 0) {
            *err = rc;
            return NULL;
        }
    }
    for (s = socks; s; s = s->next) {
        if (!s->bound || s->closed) {
            continue;
        }
        if (abstract(n) ? (!s->bound_fs && s->type == type && same_name(&s->name, n) &&
                           s->ns == proc_netns(current_proc))
                        : (s->bound_fs && vfs_mount_base(s->fs_ref.mnt) == vfs_mount_base(mnt) &&
                           s->fs_ino == ino)) {
            s->refs++;
            return s;
        }
    }
    *err = -ECONNREFUSED;
    return NULL;
}

/* Bind an unnamed socket to an abstract name of five hex digits, as Linux's
 * autobind does. */
static int32_t autobind(struct unix_sock *s)
{
    static uint32_t order;
    static const char hex[] = "0123456789abcdef";
    struct unix_name n;
    uint32_t tries, i, v;
    int32_t err;

    for (tries = 0; tries < 0x100000u; tries++) {
        struct unix_sock *other;

        v = order++ & 0xFFFFFu;
        k_memset(&n, 0, sizeof(n));
        n.len = 6;
        for (i = 0; i < 5; i++) {
            n.path[5 - i] = hex[(v >> (4 * i)) & 15u];
        }
        other = lookup(&n, s->type, &err);
        if (!other) {
            s->name = n;
            s->bound = 1;
            return 0;
        }
        unix_put(other);
    }
    return -ENOSPC;
}

static int bind_idle(void *arg)
{
    return !((const struct unix_sock *)arg)->binding;
}

/**
 * Take `s`'s bind lock: 0, or -EINTR when a signal ends the wait for it.
 *
 * A bind to a path holds it across the file system's sleeps, so that a
 * second bind, and the name a connect or a send gives a socket with
 * SO_PASSCRED, waits for the first to finish and then finds the socket
 * named, as Linux's bindlock has it.
 */
static int32_t bind_lock(struct unix_sock *s)
{
    while (s->binding) {
        if (wq_wait_event(&s->lock_wq, bind_idle, s) < 0) {
            return -EINTR;
        }
    }
    s->binding = 1;
    return 0;
}

static void bind_unlock(struct unix_sock *s)
{
    s->binding = 0;
    wq_wake_all(&s->lock_wq);
}

static int recv_idle(void *arg)
{
    return !((const struct unix_sock *)arg)->reading;
}

/**
 * Take `s`'s receive lock, which a receive holds from choosing a message to
 * consuming it, as Linux holds its iolock.
 *
 * The copy to the caller's buffers can sleep in a page fault, even for pages
 * made present before it, since another thread sharing the address space
 * can unmap one. The message must stay while the copy sleeps, so another
 * receiver, and a connect that drops the queue, wait for the lock. The wait
 * is the length of one copy, and a signal does not end it.
 */
static void recv_lock(struct unix_sock *s)
{
    while (s->reading) {
        wq_wait_event_nointr(&s->lock_wq, recv_idle, s);
    }
    s->reading = 1;
}

static void recv_unlock(struct unix_sock *s)
{
    s->reading = 0;
    wq_wake_all(&s->lock_wq);
}

/* Give a socket with SO_PASSCRED and no name the automatic one, before a
 * connect or a send, as Linux does. */
static int32_t passcred_name(struct unix_sock *s)
{
    int32_t rc;

    if (!s->passcred || s->bound) {
        return 0;
    }
    rc = bind_lock(s);
    if (rc == 0) {
        rc = s->bound ? 0 : autobind(s);
        bind_unlock(s);
    }
    return rc;
}

/* Bind the socket `s`, whose bind lock the caller holds, to the name `n`. */
static int32_t bind_name(struct unix_sock *s, const struct unix_name *n)
{
    struct vfs_ref ref;
    uint32_t ino;
    int32_t rc;

    if (s->bound || s->closed) {
        return -EINVAL;
    }
    if (n->len == 0) {
        return autobind(s);
    }
    if (abstract(n)) {
        struct unix_sock *other = lookup(n, s->type, &rc);

        if (other) {
            unix_put(other);
            return -EADDRINUSE;
        }
    } else {
        rc = fs_socket_bind(n->path, 0777u & ~current_proc->fs->umask, &ref, &ino);
        if (rc != 0) {
            return rc;
        }
        s->fs_ref = ref;
        s->fs_ino = ino;
        s->bound_fs = 1;
    }
    s->name = *n;
    s->bound = 1;
    return 0;
}

/**
 * bind(2). The name is judged free and taken under the socket's bind lock
 * (`bind_lock`), and the socket file a path makes is recorded only once it
 * is made, so two binds of one socket cannot both succeed and lose the
 * first one's hold on its file and its mount.
 */
int32_t unix_bind(fd_entry_t *e, uintptr_t addr, uint32_t addrlen)
{
    struct unix_sock *s = sock_of(e);
    struct unix_name n;
    int32_t rc = get_name(addr, addrlen, &n);

    if (rc == 0) {
        rc = bind_lock(s);
    }
    if (rc == 0) {
        rc = bind_name(s, &n);
        bind_unlock(s);
    }
    return rc;
}

/**
 * listen(2), which a listening socket may call again to change its backlog.
 * The connects waiting for room in the queue are woken, since a larger
 * backlog may have made it.
 */
int32_t unix_listen(fd_entry_t *e, int backlog)
{
    struct unix_sock *s = sock_of(e);

    if (s->type == SOCK_DGRAM) {
        return -EOPNOTSUPP;
    }
    if (s->state == US_CONNECTED || !s->bound) {
        return -EINVAL;
    }
    if (backlog < 0 || backlog > UNIX_SOMAXCONN) {
        backlog = UNIX_SOMAXCONN;
    }
    s->backlog = backlog;
    s->state = US_LISTENING;
    own_cred(&s->peercred, 1);
    s->has_peercred = 1;
    wq_wake_all(&s->wq);
    return 0;
}

/* Whether a connect waiting for room on the listener `l` can stop: there
 * is room, or `l` takes connections no more. */
static int listener_has_room(void *arg)
{
    const struct unix_sock *l = (const struct unix_sock *)arg;

    return l->closed || l->state != US_LISTENING || (l->shut & SHUT_RCV) ||
           (int)l->acc_len <= l->backlog;
}

/**
 * Make `target` the peer of the datagram socket `s`, which takes the
 * reference the caller had, or with NULL leave `s` with none.
 *
 * A socket that leaves its peer for another, or for none, drops the
 * messages it has queued, whoever sent them, as Linux's
 * unix_dgram_disconnected does; the receive lock keeps a receive's message
 * until its copy ends. The peer it leaves then gets ECONNRESET when it was
 * connected back, its end of a pair whose messages were lost.
 */
static void dgram_repeer(struct unix_sock *s, struct unix_sock *target)
{
    struct unix_sock *old = s->peer;

    s->peer = target;
    if (!old) {
        return;
    }
    if (old != target && s->rq_head) {
        recv_lock(s);
        purge(s);
        recv_unlock(s);
        if (!old->closed && old->peer == s) {
            old->err = ECONNRESET;
            wq_wake_all(&old->wq);
        }
    }
    unix_put(old);
}

/**
 * connect(2) of a datagram socket: the target becomes the peer its sends
 * without a name go to, or with AF_UNSPEC it has none (`dgram_repeer`). A
 * target connected to another socket refuses with EPERM and the connection
 * is left as it was, as Linux's unix_may_send has it.
 */
static int32_t connect_dgram(struct unix_sock *s, uintptr_t addr, uint32_t addrlen)
{
    struct unix_name n;
    struct unix_sock *target;
    uint8_t family[2];
    int32_t rc;

    if (addrlen >= SUN_PATH_OFF &&
        copy_from_user(family, (const void __user *)addr, sizeof(family)) == 0 &&
        (family[0] | (family[1] << 8)) == AF_UNSPEC) {
        dgram_repeer(s, NULL);
        return 0;
    }
    rc = get_name(addr, addrlen, &n);
    if (rc != 0) {
        return rc;
    }
    if (n.len == 0) {
        return -EINVAL;
    }
    rc = passcred_name(s);
    if (rc != 0) {
        return rc;
    }
    target = lookup(&n, SOCK_DGRAM, &rc);
    if (!target) {
        return rc;
    }
    if (target->type != SOCK_DGRAM) {
        unix_put(target);
        return -EPROTOTYPE;
    }
    if (target->peer && target->peer != s) {
        unix_put(target);
        return -EPERM;
    }
    dgram_repeer(s, target);
    return 0;
}

/*
 * Why the stream or sequenced-packet socket `s` cannot connect to `target`
 * now, or 0 when it can. A listener shut down for reading refuses, as in
 * Linux.
 */
static int32_t connect_refusal(const struct unix_sock *s, const struct unix_sock *target)
{
    if (s->state != US_UNCONNECTED) {
        return s->state == US_CONNECTED ? -EISCONN : -EINVAL;
    }
    if (target->type != s->type) {
        return -EPROTOTYPE;
    }
    if (target->state != US_LISTENING || (target->shut & SHUT_RCV)) {
        return -ECONNREFUSED;
    }
    return 0;
}

/**
 * connect(2). A stream or sequenced-packet socket queues a new socket on the
 * listener, joined to it, and a datagram socket takes the target as its peer
 * (`connect_dgram`).
 *
 * Copying the address, finding a path's socket file and waiting for room in
 * the listener's queue can each sleep, and another thread may connect the
 * socket, or make it listen, while this one sleeps; so the state of both
 * sockets is judged after the last of them, and nothing between that and
 * the join can sleep.
 */
int32_t unix_connect(fd_entry_t *e, uintptr_t addr, uint32_t addrlen)
{
    struct unix_sock *s = sock_of(e);
    struct unix_sock *target, *c;
    struct unix_name n;
    int32_t rc;

    if (s->type == SOCK_DGRAM) {
        return connect_dgram(s, addr, addrlen);
    }
    if (s->state == US_CONNECTED) {
        return -EISCONN;
    }
    if (s->state == US_LISTENING) {
        return -EINVAL;
    }
    rc = get_name(addr, addrlen, &n);
    if (rc != 0) {
        return rc;
    }
    if (n.len == 0) {
        return -EINVAL;
    }
    rc = passcred_name(s);
    if (rc != 0) {
        return rc;
    }
    for (;;) {
        target = lookup(&n, s->type, &rc);
        if (!target) {
            return rc;
        }
        rc = connect_refusal(s, target);
        if (rc == 0 && (int)target->acc_len <= target->backlog) {
            break;
        }
        if (rc == 0 && nonblocking(e, 0)) {
            rc = -EAGAIN;
        }
        if (rc == 0) {
            rc = unix_wait(&target->wq, listener_has_room, target, s->sndtimeo);
        }
        unix_put(target);
        if (rc != 0) {
            return rc;
        }
    }
    c = unix_alloc(s->type);
    if (!c) {
        unix_put(target);
        return -ENOMEM;
    }
    c->uid = target->uid;
    c->name = target->name;
    c->passcred = target->passcred;
    own_cred(&c->peercred, 1);
    c->has_peercred = 1;
    join(s, c);
    s->peercred = target->peercred;
    s->has_peercred = 1;
    if (target->acc_tail) {
        target->acc_tail->acc_next = c;
    } else {
        target->acc_head = c;
    }
    target->acc_tail = c;
    target->acc_len++;
    wq_wake(&target->wq, WQ_DATA);
    unix_put(target);
    return 0;
}

/* Whether an accept waiting on the listener `l` can stop: a connection is
 * queued, or `l` takes connections no more. */
static int has_connection(void *arg)
{
    const struct unix_sock *l = (const struct unix_sock *)arg;

    return l->acc_head != NULL || l->state != US_LISTENING || (l->shut & SHUT_RCV);
}

/**
 * accept4(2). The descriptor is reserved before the connection is taken off
 * the queue, so a caller with none free leaves it queued, and published only
 * after the peer's name is copied out: a copy that fails frees the slot and
 * ends the connection as a close of it would, and no other thread can have
 * used the socket or freed the peer whose name is copied. An address with
 * no length pointer is such a failure, EFAULT, as in Linux. A listener shut
 * down for reading gives up the connections queued before and then fails
 * with EINVAL, as Linux's does.
 */
int32_t unix_accept(fd_entry_t *e, uintptr_t addr, uintptr_t addrlen_ptr, int flags)
{
    static const struct unix_name none;
    struct unix_sock *s = sock_of(e);
    struct unix_sock *c;
    int32_t rc;
    int fd;

    if (s->type == SOCK_DGRAM) {
        return -EOPNOTSUPP;
    }
    if (flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) {
        return -EINVAL;
    }
    while (!s->acc_head) {
        if (s->state != US_LISTENING || (s->shut & SHUT_RCV)) {
            return -EINVAL;
        }
        if (nonblocking(e, 0)) {
            return -EAGAIN;
        }
        rc = unix_wait(&s->wq, has_connection, s, s->rcvtimeo);
        if (rc != 0) {
            return rc;
        }
    }
    fd = reserve();
    if (fd < 0) {
        return fd;
    }
    c = s->acc_head;
    s->acc_head = c->acc_next;
    if (!s->acc_head) {
        s->acc_tail = NULL;
    }
    c->acc_next = NULL;
    s->acc_len--;
    wq_wake(&s->wq, 0);
    if (addr) {
        rc = put_name(addr, addrlen_ptr, c->peer ? &c->peer->name : &none, 0);
        if (rc != 0) {
            fd_free(table(), fd);
            unix_release(c, 0);
            return rc;
        }
    }
    publish(fd, c, flags);
    return fd;
}

int32_t unix_getname(fd_entry_t *e, uintptr_t addr, uintptr_t addrlen_ptr, int peer)
{
    struct unix_sock *s = sock_of(e);

    if (!peer) {
        return put_name(addr, addrlen_ptr, &s->name, 0);
    }
    if (!s->peer || (s->type != SOCK_DGRAM && s->state != US_CONNECTED)) {
        return -ENOTCONN;
    }
    return put_name(addr, addrlen_ptr, &s->peer->name, 0);
}

/**
 * shutdown(2). A connection's other end has the matching direction shut
 * too. Whoever waits on either socket is woken, and so are the sockets
 * connected to `s`, whose sends to it now fail and which poll therefore
 * reports writable.
 */
int32_t unix_shutdown(fd_entry_t *e, int how)
{
    struct unix_sock *s = sock_of(e);
    int mode;

    if (how < 0 || how > 2) {
        return -EINVAL;
    }
    mode = how + 1;
    s->shut |= mode;
    if (s->peer && s->type != SOCK_DGRAM) {
        s->peer->shut |= ((mode & SHUT_RCV) << 1) | ((mode & SHUT_SEND) >> 1);
        wq_wake_all(&s->peer->wq);
    }
    wq_wake_all(&s->wq);
    wake_writers(s);
    return 0;
}

/* A time limit of SO_RCVTIMEO or SO_SNDTIMEO, a struct timeval of two
 * longs, in ticks, 0 for none, or 0xFFFFFFFF for a timeval out of range. */
static uint32_t timeval_ticks(const long tv[2])
{
    uint32_t ticks;

    if (tv[0] < 0 || tv[1] < 0 || tv[1] >= 1000000) {
        return 0xFFFFFFFFu;
    }
    if ((unsigned long)tv[0] > 0x7FFFFFFEu / 100u - 1u) {
        return 0x7FFFFFFEu;
    }
    ticks = (uint32_t)tv[0] * 100u + ((uint32_t)tv[1] + 9999u) / 10000u;
    return ticks;
}

/* A buffer size set by SO_SNDBUF or SO_RCVBUF: twice the value asked, at
 * most twice Linux's limit, as Linux keeps it. */
static uint32_t buf_size(int32_t v)
{
    if (v < 2304) {
        return 4608u;
    }
    if ((uint32_t)v > UNIX_BUF) {
        return UNIX_BUF * 2u;
    }
    return (uint32_t)v * 2u;
}

int32_t unix_setsockopt(fd_entry_t *e, int level, int name, uintptr_t val, uint32_t len)
{
    struct unix_sock *s = sock_of(e);
    int32_t v = 0;

    if (level != SOL_SOCKET) {
        return -ENOPROTOOPT;
    }
    if (name == SO_RCVTIMEO || name == SO_SNDTIMEO) {
        long tv[2];
        uint32_t ticks;

        if (len < sizeof(tv)) {
            return -EINVAL;
        }
        if (copy_from_user(tv, (const void __user *)val, sizeof(tv)) != 0) {
            return -EFAULT;
        }
        ticks = timeval_ticks(tv);
        if (ticks == 0xFFFFFFFFu) {
            return -EDOM;
        }
        if (name == SO_RCVTIMEO) {
            s->rcvtimeo = ticks;
        } else {
            s->sndtimeo = ticks;
        }
        return 0;
    }
    if (name == SO_LINGER) {
        return len < 8 ? -EINVAL : 0;
    }
    if (len < sizeof(v)) {
        return -EINVAL;
    }
    if (copy_from_user(&v, (const void __user *)val, sizeof(v)) != 0) {
        return -EFAULT;
    }
    switch (name) {
    case SO_PASSCRED:
        s->passcred = v != 0;
        return 0;
    case SO_SNDBUF:
    case SO_SNDBUFFORCE:
        s->sndbuf = buf_size(v);
        return 0;
    case SO_RCVBUF:
    case SO_RCVBUFFORCE:
        s->rcvbuf = buf_size(v);
        return 0;
    case SO_REUSEADDR:
    case SO_KEEPALIVE:
    case SO_BROADCAST:
    case SO_RCVLOWAT:
    case SO_SNDLOWAT:
        return 0;
    default:
        return -ENOPROTOOPT;
    }
}

int32_t unix_getsockopt(fd_entry_t *e, int level, int name, uintptr_t val, uintptr_t len_ptr)
{
    struct unix_sock *s = sock_of(e);
    struct unix_cred c;
    uint8_t out[sizeof(struct unix_cred)];
    uint32_t n = 4;
    int32_t given, v = 0;

    if (level != SOL_SOCKET) {
        return -ENOPROTOOPT;
    }
    if (copy_from_user(&given, (const void __user *)len_ptr, sizeof(given)) != 0) {
        return -EFAULT;
    }
    if (given < 0) {
        return -EINVAL;
    }
    switch (name) {
    case SO_TYPE:
        v = s->type;
        break;
    case SO_ERROR:
        v = s->err;
        s->err = 0;
        break;
    case SO_PASSCRED:
        v = s->passcred;
        break;
    case SO_SNDBUF:
        v = (int32_t)s->sndbuf;
        break;
    case SO_RCVBUF:
        v = (int32_t)s->rcvbuf;
        break;
    case SO_ACCEPTCONN:
        v = s->state == US_LISTENING;
        break;
    case SO_DOMAIN:
        v = AF_UNIX;
        break;
    case SO_PROTOCOL:
    case SO_REUSEADDR:
    case SO_KEEPALIVE:
    case SO_BROADCAST:
        break;
    case SO_RCVLOWAT:
    case SO_SNDLOWAT:
        v = 1;
        break;
    case SO_PEERCRED:
        c.pid = 0;
        c.uid = 0xFFFFFFFFu;
        c.gid = 0xFFFFFFFFu;
        if (s->has_peercred) {
            c = cred_seen(s->peercred);
        }
        n = sizeof(c);
        break;
    default:
        return -ENOPROTOOPT;
    }
    if (name == SO_PEERCRED) {
        k_memcpy(out, &c, sizeof(c));
    } else {
        k_memcpy(out, &v, sizeof(v));
    }
    if ((uint32_t)given < n) {
        n = (uint32_t)given;
    }
    if (copy_to_user((void __user *)val, out, n) != 0 ||
        copy_to_user((void __user *)len_ptr, &n, sizeof(n)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/* The bytes of a caller's buffers from `skip` on, copied to or from `k`. */
static int iov_copy(const struct kernel_iovec *iov, uint32_t iovlen, uint32_t skip, uint8_t *k,
                    uint32_t n, int to_user)
{
    uint32_t i;

    for (i = 0; i < iovlen && n > 0; i++) {
        uint32_t take;

        if (skip >= iov[i].iov_len) {
            skip -= (uint32_t)iov[i].iov_len;
            continue;
        }
        take = (uint32_t)(iov[i].iov_len - skip);
        if (take > n) {
            take = n;
        }
        if (to_user ? copy_to_user((void __user *)(iov[i].iov_base + skip), k, take)
                    : copy_from_user(k, (const void __user *)(iov[i].iov_base + skip), take)) {
            return -EFAULT;
        }
        k += take;
        n -= take;
        skip = 0;
    }
    return 0;
}

/* The descriptors of one SCM_RIGHTS message added to `out`, each with the
 * references of a descriptor taken. */
static int32_t get_rights(uintptr_t data, uint32_t n, struct unix_out *out)
{
    fd_entry_t *grown;
    uint32_t i;

    if (out->nfds + n > UNIX_MAX_FDS) {
        return -EINVAL;
    }
    grown = kmalloc(sizeof(fd_entry_t) * (out->nfds + n + 1u));
    if (!grown) {
        return -ENOMEM;
    }
    if (out->nfds) {
        k_memcpy(grown, out->fds, sizeof(fd_entry_t) * out->nfds);
    }
    kfree(out->fds);
    out->fds = grown;
    for (i = 0; i < n; i++) {
        int32_t fd;
        fd_entry_t *e;

        if (copy_from_user(&fd, (const void __user *)(data + 4u * i), sizeof(fd)) != 0) {
            return -EFAULT;
        }
        e = fd_get(table(), fd);
        if (!e) {
            return -EBADF;
        }
        out->fds[out->nfds] = *e;
        out->fds[out->nfds].flags &= ~(uint32_t)FD_FLAG_CLOEXEC;
        flight_hold(&out->fds[out->nfds]);
        out->nfds++;
    }
    return 0;
}

/* The credentials of an SCM_CREDENTIALS message: the sender's own process
 * id, the thread group's and not a thread's, and ids, unless it is root, as
 * Linux checks them. They are named in the sender's namespaces and kept as
 * the kernel's ids. */
static int32_t get_cred(uintptr_t data, struct unix_out *out)
{
    const process_t *p = current_proc;
    struct unix_cred c;
    int64_t uid;
    int64_t gid;

    if (copy_from_user(&c, (const void __user *)data, sizeof(c)) != 0) {
        return -EFAULT;
    }
    uid = uid_named(c.uid);
    gid = gid_named(c.gid);
    if (c.uid == 0xFFFFFFFFu || c.gid == 0xFFFFFFFFu || uid < 0 || gid < 0) {
        return -EINVAL;
    }
    c.pid = pid_named(c.pid);
    c.uid = (uint32_t)uid;
    c.gid = (uint32_t)gid;
    if (!proc_privileged(p) &&
        (c.pid != p->tgid || (c.uid != p->ruid && c.uid != p->euid && c.uid != p->suid) ||
         (c.gid != p->rgid && c.gid != p->egid && c.gid != p->sgid))) {
        return -EPERM;
    }
    out->cred = c;
    out->has_cred = 1;
    return 0;
}

/* Read the caller's control messages into `out`. */
static int32_t get_control(uintptr_t control, uint32_t controllen, struct unix_out *out)
{
    uint32_t off = 0;

    while (off + CMSG_HDR <= controllen) {
        struct kernel_cmsghdr hdr;
        int32_t rc;

        if (copy_from_user(&hdr, (const void __user *)(control + off), sizeof(hdr)) != 0) {
            return -EFAULT;
        }
        if (hdr.cmsg_len < CMSG_HDR || hdr.cmsg_len > controllen - off ||
            hdr.cmsg_level != SOL_SOCKET) {
            return -EINVAL;
        }
        if (hdr.cmsg_type == SCM_RIGHTS) {
            rc =
                get_rights(control + off + CMSG_HDR, (uint32_t)(hdr.cmsg_len - CMSG_HDR) / 4u, out);
        } else if (hdr.cmsg_type == SCM_CREDENTIALS &&
                   hdr.cmsg_len == CMSG_HDR + sizeof(struct unix_cred)) {
            rc = get_cred(control + off + CMSG_HDR, out);
        } else {
            rc = -EINVAL;
        }
        if (rc != 0) {
            return rc;
        }
        off += (uint32_t)CMSG_ALIGN(hdr.cmsg_len);
    }
    return 0;
}

static void release_out(struct unix_out *out)
{
    drop_fds(out->fds, 0, out->nfds);
    out->fds = NULL;
    out->nfds = 0;
}

/* A message of `len` bytes of the caller's buffers from `skip` bytes in,
 * with the sender's name and credentials, or NULL with the reason. */
static struct unix_msg *build(const struct unix_sock *s, const struct kernel_iovec *iov,
                              uint32_t iovlen, uint32_t skip, uint32_t len,
                              const struct unix_out *out, int32_t *err)
{
    struct unix_msg *m = msg_alloc(len);

    if (!m) {
        *err = -ENOBUFS;
        return NULL;
    }
    if (iov_copy(iov, iovlen, skip, m->data, len, 0) != 0) {
        kfree(m);
        *err = -EFAULT;
        return NULL;
    }
    if (out->has_cred) {
        m->cred = out->cred;
    } else {
        own_cred(&m->cred, 0);
    }
    m->from = s->name;
    return m;
}

/* Move the descriptors `out` carries into `m`. */
static void attach(struct unix_msg *m, struct unix_out *out)
{
    m->nfds = out->nfds;
    m->fds = out->fds;
    out->nfds = 0;
    out->fds = NULL;
}

static void broken_pipe(int flags)
{
    struct k_siginfo info;

    if (flags & MSG_NOSIGNAL) {
        return;
    }
    k_memset(&info, 0, sizeof(info));
    info.code = SI_USER;
    info.pid = current_proc->tgid;
    info.uid = current_proc->ruid;
    signal_post_thread(current_proc, SIGPIPE, &info);
}

static int peer_has_room(void *arg)
{
    const struct unix_sock *s = (const struct unix_sock *)arg;
    const struct unix_sock *p = s->peer;

    return !p || p->closed || (s->shut & SHUT_SEND) || s->err || !queue_full(p);
}

/* Whether a send on the connected socket `s` to its peer `p` must end with
 * EPIPE: the connection is gone, or shut down in the send's direction. */
static int send_stopped(const struct unix_sock *s, const struct unix_sock *p)
{
    return !p || p->closed || (s->shut & SHUT_SEND) || (p->shut & SHUT_RCV);
}

/*
 * A send on a connected stream or sequenced-packet socket. A stream's bytes
 * go in pieces as the peer's queue has room; a packet goes whole. Copying a
 * piece from the caller can sleep, and the connection is judged again after
 * it, so a piece is not queued past an error or a shutdown that came
 * meanwhile.
 */
static int32_t send_conn(struct unix_sock *s, int nonblock, const struct kernel_iovec *iov,
                         uint32_t iovlen, uint32_t total, struct unix_out *out, int flags)
{
    uint32_t done = 0;
    int32_t rc;

    if (s->state != US_CONNECTED) {
        return -ENOTCONN;
    }
    if (s->type == SOCK_SEQPACKET && total > s->sndbuf - 32u) {
        return -EMSGSIZE;
    }
    if (s->type == SOCK_STREAM && total == 0) {
        return 0;
    }
    for (;;) {
        struct unix_sock *p = s->peer;
        struct unix_msg *m;
        uint32_t chunk;

        if (s->err) {
            rc = -s->err;
            s->err = 0;
            return done ? (int32_t)done : rc;
        }
        if (send_stopped(s, p)) {
            if (done == 0) {
                broken_pipe(flags);
            }
            return done ? (int32_t)done : -EPIPE;
        }
        if (queue_full(p)) {
            if (nonblock) {
                return done ? (int32_t)done : -EAGAIN;
            }
            rc = unix_wait(&p->wq, peer_has_room, s, s->sndtimeo);
            if (rc != 0) {
                return done ? (int32_t)done : rc;
            }
            continue;
        }
        chunk = total - done;
        if (s->type == SOCK_STREAM) {
            if (chunk > p->rcvbuf - p->rq_bytes) {
                chunk = p->rcvbuf - p->rq_bytes;
            }
            if (chunk > UNIX_CHUNK) {
                chunk = UNIX_CHUNK;
            }
        }
        m = build(s, iov, iovlen, done, chunk, out, &rc);
        if (!m) {
            return done ? (int32_t)done : rc;
        }
        if (s->peer != p || s->err || send_stopped(s, p) || queue_full(p)) {
            kfree(m);
            continue;
        }
        attach(m, out);
        enqueue(p, m);
        done += chunk;
        if (done >= total) {
            return (int32_t)done;
        }
    }
}

/* Whether a datagram send waiting for room in the queue of `t` can stop:
 * there is room, or `t` is gone or takes nothing more, when the send fails. */
static int target_has_room(void *arg)
{
    const struct unix_sock *t = (const struct unix_sock *)arg;

    return t->closed || (t->shut & SHUT_RCV) || !queue_full(t);
}

/* A send on a datagram socket, to `to` or to the connected peer. */
static int32_t send_dgram(struct unix_sock *s, int nonblock, const struct kernel_iovec *iov,
                          uint32_t iovlen, uint32_t total, const struct unix_name *to,
                          struct unix_out *out)
{
    struct unix_sock *t;
    struct unix_msg *m;
    int32_t rc;

    if (total > s->sndbuf - 32u) {
        return -EMSGSIZE;
    }
    if (s->shut & SHUT_SEND) {
        return -EPIPE;
    }
    rc = passcred_name(s);
    if (rc != 0) {
        return rc;
    }
    if (to) {
        t = lookup(to, SOCK_DGRAM, &rc);
        if (!t) {
            return rc;
        }
        if (t->type != SOCK_DGRAM) {
            unix_put(t);
            return -EPROTOTYPE;
        }
    } else {
        if (!s->peer) {
            return -ENOTCONN;
        }
        t = s->peer;
        t->refs++;
    }
    m = build(s, iov, iovlen, 0, total, out, &rc);
    if (!m) {
        unix_put(t);
        return rc;
    }
    for (;;) {
        if (s->shut & SHUT_SEND) {
            rc = -EPIPE;
            break;
        }
        if (t->closed) {
            /* As Linux: a peer that is gone disconnects, and then ENOTCONN. */
            if (!to && s->peer == t) {
                s->peer = NULL;
                unix_put(t);
            }
            rc = -ECONNREFUSED;
            break;
        }
        if (t->peer && t->peer != s) {
            rc = -EPERM;
            break;
        }
        if (t->shut & SHUT_RCV) {
            rc = -EPIPE;
            break;
        }
        if (!queue_full(t)) {
            attach(m, out);
            enqueue(t, m);
            unix_put(t);
            return (int32_t)total;
        }
        if (nonblock) {
            rc = -EAGAIN;
            break;
        }
        rc = unix_wait(&t->wq, target_has_room, t, s->sndtimeo);
        if (rc != 0) {
            break;
        }
    }
    kfree(m);
    unix_put(t);
    return rc;
}

static int32_t send_core(fd_entry_t *e, const struct kernel_iovec *iov, uint32_t iovlen,
                         uint32_t total, const struct unix_name *to, struct unix_out *out,
                         int flags)
{
    struct unix_sock *s = sock_of(e);
    int nonblock = nonblocking(e, flags);
    int32_t rc;

    if (flags & MSG_OOB) {
        rc = -EOPNOTSUPP;
    } else if (s->type == SOCK_DGRAM) {
        rc = send_dgram(s, nonblock, iov, iovlen, total, to, out);
    } else if (to) {
        rc = s->state == US_CONNECTED ? -EISCONN : -EOPNOTSUPP;
    } else {
        rc = send_conn(s, nonblock, iov, iovlen, total, out, flags);
    }
    release_out(out);
    return rc;
}

static int has_data(void *arg)
{
    const struct unix_sock *s = (const struct unix_sock *)arg;

    return s->rq_head != NULL || s->err || (s->shut & SHUT_RCV);
}

/* Wait for a message: 0 with one at the head of the queue, 1 at the end of
 * input, or -errno. */
static int32_t wait_data(struct unix_sock *s, int nonblock)
{
    int32_t rc;

    while (!s->rq_head) {
        if (s->err) {
            rc = -s->err;
            s->err = 0;
            return rc;
        }
        if (s->shut & SHUT_RCV) {
            return 1;
        }
        if (nonblock) {
            return -EAGAIN;
        }
        rc = unix_wait(&s->wq, has_data, s, s->rcvtimeo);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* Move a message's descriptors into `in`, or with `peek` copies of them.
 * Whether it had any. */
static int take_fds(struct unix_msg *m, struct unix_in *in, int peek)
{
    uint32_t i;

    if (m->nfds == 0) {
        return 0;
    }
    if (!peek) {
        in->fds = m->fds;
        in->nfds = m->nfds;
        m->fds = NULL;
        m->nfds = 0;
        return 1;
    }
    in->fds = kmalloc(sizeof(fd_entry_t) * m->nfds);
    if (in->fds) {
        for (i = 0; i < m->nfds; i++) {
            in->fds[i] = m->fds[i];
            flight_hold(&in->fds[i]);
        }
        in->nfds = m->nfds;
    }
    return 1;
}

static int same_cred(const struct unix_cred *a, const struct unix_cred *b)
{
    return a->pid == b->pid && a->uid == b->uid && a->gid == b->gid;
}

/* The message a stream read continues from after `copied` bytes, and the
 * offset in it; a peek does not consume, so it counts them off. */
static struct unix_msg *stream_at(struct unix_sock *s, uint32_t copied, int peek, uint32_t *off)
{
    struct unix_msg *m = s->rq_head;

    if (!peek) {
        *off = m ? m->off : 0;
        return m;
    }
    while (m && copied >= m->len - m->off) {
        copied -= m->len - m->off;
        m = m->next;
    }
    *off = m ? m->off + copied : 0;
    return m;
}

/* Consume `chunk` bytes of the stream message `m` at the head of `s`'s
 * queue, and the message once they are all read. */
static void stream_consume(struct unix_sock *s, struct unix_msg *m, uint32_t chunk)
{
    int was_full = queue_full(s);

    m->off += chunk;
    s->rq_bytes -= chunk;
    if (m->off == m->len) {
        msg_free(dequeue(s));
    }
    if (was_full && !queue_full(s)) {
        wake_writers(s);
    }
}

/*
 * A receive on a stream socket: bytes across messages, up to and including
 * one that carries descriptors, and not past a change of credentials when
 * they are asked for. The receive lock is held throughout but for the wait
 * for data.
 */
static int32_t recv_stream(struct unix_sock *s, int nonblock, const struct kernel_iovec *iov,
                           uint32_t iovlen, uint32_t total, int flags, struct unix_in *in)
{
    uint32_t copied = 0;
    uint32_t want = (flags & MSG_WAITALL) ? total : 1u;
    int peek = (flags & MSG_PEEK) != 0;
    int32_t rc = 0;

    if (s->state != US_CONNECTED) {
        return -EINVAL;
    }
    if (total == 0) {
        return 0;
    }
    recv_lock(s);
    for (;;) {
        uint32_t off, chunk;
        struct unix_msg *m = stream_at(s, copied, peek, &off);
        int had_fds;

        if (!m) {
            if (copied >= want || (copied > 0 && (peek || nonblock))) {
                break;
            }
            recv_unlock(s);
            rc = wait_data(s, nonblock);
            recv_lock(s);
            if (rc == 0) {
                continue;
            }
            if (rc == 1 || copied > 0) {
                rc = 0;
            }
            break;
        }
        if (s->passcred && copied > 0 && !same_cred(&in->cred, &m->cred)) {
            break;
        }
        chunk = m->len - off;
        if (chunk > total - copied) {
            chunk = total - copied;
        }
        if (iov_copy(iov, iovlen, copied, m->data + off, chunk, 1) != 0) {
            rc = -EFAULT;
            break;
        }
        if (copied == 0) {
            in->from = m->from;
            in->cred = m->cred;
            in->has_cred = s->passcred;
        }
        copied += chunk;
        had_fds = take_fds(m, in, peek);
        if (!peek) {
            stream_consume(s, m, chunk);
        }
        if (had_fds || copied >= total) {
            break;
        }
    }
    recv_unlock(s);
    return copied ? (int32_t)copied : rc;
}

/* A receive on a datagram or sequenced-packet socket: one message, which
 * the receive lock keeps from its choice to its consumption. */
static int32_t recv_msg(struct unix_sock *s, int nonblock, const struct kernel_iovec *iov,
                        uint32_t iovlen, uint32_t total, int flags, struct unix_in *in)
{
    int peek = (flags & MSG_PEEK) != 0;
    struct unix_msg *m;
    uint32_t chunk;
    int32_t rc;

    if (s->type == SOCK_SEQPACKET && s->state != US_CONNECTED) {
        return -ENOTCONN;
    }
    for (;;) {
        rc = wait_data(s, nonblock);
        if (rc == 1) {
            return 0;
        }
        if (rc < 0) {
            return rc;
        }
        recv_lock(s);
        if (s->rq_head) {
            break;
        }
        recv_unlock(s);
    }
    m = s->rq_head;
    chunk = m->len < total ? m->len : total;
    if (iov_copy(iov, iovlen, 0, m->data, chunk, 1) != 0) {
        recv_unlock(s);
        return -EFAULT;
    }
    if (m->len > total) {
        in->flags |= MSG_TRUNC;
    }
    in->from = m->from;
    in->cred = m->cred;
    in->has_cred = s->passcred;
    take_fds(m, in, peek);
    rc = (flags & MSG_TRUNC) ? (int32_t)m->len : (int32_t)chunk;
    if (!peek) {
        msg_free(dequeue(s));
    }
    recv_unlock(s);
    return rc;
}

static int32_t recv_core(fd_entry_t *e, const struct kernel_iovec *iov, uint32_t iovlen,
                         uint32_t total, int flags, struct unix_in *in)
{
    struct unix_sock *s = sock_of(e);
    int nonblock = nonblocking(e, flags);

    if (flags & MSG_OOB) {
        return -EOPNOTSUPP;
    }
    if (s->type == SOCK_STREAM) {
        return recv_stream(s, nonblock, iov, iovlen, total, flags, in);
    }
    return recv_msg(s, nonblock, iov, iovlen, total, flags, in);
}

static void release_in(struct unix_in *in)
{
    drop_fds(in->fds, 0, in->nfds);
    in->fds = NULL;
    in->nfds = 0;
}

/**
 * Install the received descriptors, as many as the `room` of the control
 * buffer takes, and write their numbers after the header at `at`. Returns
 * the number given.
 *
 * Each descriptor is reserved, its number copied out, and only then given
 * its open file, as Linux's receive_fd_user does: the copy can sleep, and a
 * thread sharing the table must find nothing there it could use or close.
 * The descriptors that do not fit are closed and MSG_CTRUNC set, and so are
 * the rest once the table or the heap has no room for one or its number
 * cannot be written; the receive succeeds, as in Linux.
 */
static uint32_t put_rights(uintptr_t at, uint32_t room, struct unix_in *in, int flags)
{
    uint32_t given = 0;

    while (given < in->nfds && given < room) {
        int fd = fd_alloc(table());
        fd_entry_t *slot;

        if (fd < 0) {
            break;
        }
        if (copy_to_user((void __user *)(at + CMSG_HDR + 4u * given), &fd, sizeof(fd)) != 0) {
            fd_free(table(), fd);
            break;
        }
        slot = &table()->entries[fd];
        fd_ofd_unref(slot->ofd);
        *slot = in->fds[given];
        in->fds[given++].type = FD_TYPE_NONE;
        flight_sync(slot);
        if (flags & MSG_CMSG_CLOEXEC) {
            slot->flags |= FD_FLAG_CLOEXEC;
        }
    }
    if (given < in->nfds) {
        in->flags |= MSG_CTRUNC;
    }
    drop_fds(in->fds, given, in->nfds);
    in->fds = NULL;
    in->nfds = 0;
    return given;
}

/**
 * Write the control messages of a receive to the caller's buffer: the
 * credentials, and the descriptors, each installed as a new descriptor.
 * Returns the bytes of the buffer used.
 */
static uint32_t put_control(uintptr_t control, uint32_t controllen, struct unix_in *in, int flags)
{
    uint32_t used = 0;
    struct kernel_cmsghdr hdr;
    uint32_t given;
    struct unix_cred seen;

    k_memset(&hdr, 0, sizeof(hdr));
    if (in->has_cred) {
        seen = cred_seen(in->cred);
        hdr.cmsg_len = CMSG_HDR + sizeof(seen);
        hdr.cmsg_level = SOL_SOCKET;
        hdr.cmsg_type = SCM_CREDENTIALS;
        if (!control || controllen < hdr.cmsg_len ||
            copy_to_user((void __user *)control, &hdr, sizeof(hdr)) != 0 ||
            copy_to_user((void __user *)(control + CMSG_HDR), &seen, sizeof(seen)) != 0) {
            in->flags |= MSG_CTRUNC;
        } else {
            used = (uint32_t)CMSG_ALIGN(hdr.cmsg_len);
        }
    }
    if (in->nfds == 0) {
        return used;
    }
    given = put_rights(control + used,
                       (control && controllen > used + CMSG_HDR)
                           ? (uint32_t)(controllen - used - CMSG_HDR) / 4u
                           : 0u,
                       in, flags);
    if (given > 0) {
        hdr.cmsg_len = CMSG_HDR + 4u * given;
        hdr.cmsg_level = SOL_SOCKET;
        hdr.cmsg_type = SCM_RIGHTS;
        if (copy_to_user((void __user *)(control + used), &hdr, sizeof(hdr)) == 0) {
            used += (uint32_t)CMSG_ALIGN(hdr.cmsg_len);
        }
    }
    return used;
}

int32_t unix_sendto(fd_entry_t *e, uintptr_t buf, uint32_t len, int flags, uintptr_t addr,
                    uint32_t addrlen)
{
    struct kernel_iovec iov;
    struct unix_out out;
    struct unix_name to;
    int32_t rc;

    if (len > 0x7FFFFFFFu) {
        return -EINVAL;
    }
    if (addr) {
        rc = get_name(addr, addrlen, &to);
        if (rc != 0) {
            return rc;
        }
    }
    iov.iov_base = buf;
    iov.iov_len = len;
    k_memset(&out, 0, sizeof(out));
    return send_core(e, &iov, 1, len, addr ? &to : NULL, &out, flags);
}

/**
 * recvfrom(2), and recv(2) and read(2) with no address. An address with no
 * length pointer is EFAULT before anything is taken, so the message stays
 * queued; Linux fails the call only after it has taken the message.
 */
int32_t unix_recvfrom(fd_entry_t *e, uintptr_t buf, uint32_t len, int flags, uintptr_t addr,
                      uintptr_t addrlen_ptr)
{
    struct kernel_iovec iov;
    struct unix_in in;
    int32_t rc;

    if (len > 0x7FFFFFFFu) {
        return -EINVAL;
    }
    if (addr && !addrlen_ptr) {
        return -EFAULT;
    }
    iov.iov_base = buf;
    iov.iov_len = len;
    k_memset(&in, 0, sizeof(in));
    transit_enter(&in);
    rc = recv_core(e, &iov, 1, len, flags, &in);
    release_in(&in);
    transit_leave(&in);
    if (rc >= 0 && addr) {
        int32_t err = put_name(addr, addrlen_ptr, &in.from, 1);

        if (err != 0) {
            return err;
        }
    }
    return rc;
}

/* The caller's iovec array, copied in and checked, with its total length
 * in `*total`. The caller frees the array, whatever the result. */
static int32_t get_iov(uintptr_t uiov, unsigned long iovlen, struct kernel_iovec **iov,
                       uint32_t *total)
{
    unsigned long i;

    *iov = NULL;
    *total = 0;
    if (iovlen > IOV_MAX_LOCAL) {
        return -EMSGSIZE;
    }
    if (iovlen == 0) {
        return 0;
    }
    *iov = kmalloc(sizeof(struct kernel_iovec) * iovlen);
    if (!*iov) {
        return -ENOMEM;
    }
    if (copy_from_user(*iov, (const void __user *)uiov, sizeof(struct kernel_iovec) * iovlen) !=
        0) {
        return -EFAULT;
    }
    for (i = 0; i < iovlen; i++) {
        if ((*iov)[i].iov_len > 0x7FFFFFFFu - *total) {
            return -EINVAL;
        }
        *total += (uint32_t)(*iov)[i].iov_len;
    }
    return 0;
}

int32_t unix_sendmsg(fd_entry_t *e, uintptr_t msg, int flags)
{
    struct kernel_msghdr hdr;
    struct kernel_iovec *iov;
    struct unix_out out;
    struct unix_name to;
    int named;
    uint32_t total;
    int32_t rc;

    if (copy_from_user(&hdr, (const void __user *)msg, sizeof(hdr)) != 0) {
        return -EFAULT;
    }
    named = hdr.msg_name != 0 && hdr.msg_namelen != 0;
    if (named) {
        rc = get_name(hdr.msg_name, hdr.msg_namelen, &to);
        if (rc != 0) {
            return rc;
        }
    }
    k_memset(&out, 0, sizeof(out));
    rc = get_iov(hdr.msg_iov, hdr.msg_iovlen, &iov, &total);
    if (rc == 0 && hdr.msg_control && hdr.msg_controllen) {
        rc = get_control(hdr.msg_control, USER_LEN32(hdr.msg_controllen), &out);
    }
    if (rc == 0) {
        rc = send_core(e, iov, (uint32_t)hdr.msg_iovlen, total, named ? &to : NULL, &out, flags);
    } else {
        release_out(&out);
    }
    kfree(iov);
    return rc;
}

int32_t unix_recvmsg(fd_entry_t *e, uintptr_t msg, int flags)
{
    struct kernel_msghdr hdr;
    struct kernel_iovec *iov;
    struct unix_in in;
    uint32_t total;
    int32_t rc;

    if (copy_from_user(&hdr, (const void __user *)msg, sizeof(hdr)) != 0) {
        return -EFAULT;
    }
    if (!user_buffer_ok(msg, sizeof(hdr), 1) ||
        (hdr.msg_control && !user_buffer_ok(hdr.msg_control, USER_LEN32(hdr.msg_controllen), 1))) {
        return -EFAULT;
    }
    rc = get_iov(hdr.msg_iov, hdr.msg_iovlen, &iov, &total);
    if (rc != 0) {
        kfree(iov);
        return rc;
    }
    k_memset(&in, 0, sizeof(in));
    transit_enter(&in);
    rc = recv_core(e, iov, (uint32_t)hdr.msg_iovlen, total, flags, &in);
    kfree(iov);
    if (rc < 0) {
        release_in(&in);
        transit_leave(&in);
        return rc;
    }
    hdr.msg_controllen = put_control(hdr.msg_control, USER_LEN32(hdr.msg_controllen), &in, flags);
    transit_leave(&in);
    if (hdr.msg_name) {
        uint8_t sa[SUN_PATH_OFF + UNIX_PATH_MAX + 1];
        uint32_t full = in.from.len ? name_addrlen(&in.from) : 0u;
        uint32_t give = (uint32_t)hdr.msg_namelen < full ? (uint32_t)hdr.msg_namelen : full;

        k_memset(sa, 0, sizeof(sa));
        sa[0] = AF_UNIX;
        k_memcpy(sa + SUN_PATH_OFF, in.from.path, in.from.len);
        if (give > 0 && copy_to_user((void __user *)hdr.msg_name, sa, give) != 0) {
            return -EFAULT;
        }
        hdr.msg_namelen = full;
    }
    hdr.msg_flags = in.flags;
    if (copy_to_user((void __user *)msg, &hdr, sizeof(hdr)) != 0) {
        return -EFAULT;
    }
    return rc;
}

static int32_t unix_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    return unix_recvfrom(e, (__force uintptr_t)buf, count, 0, 0, 0);
}

static int32_t unix_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    return unix_sendto(e, (__force uintptr_t)buf, count, 0, 0, 0);
}

/* Whether a send would not wait: a connected socket's peer has room in its
 * queue, or the send fails at once, the peer being gone or the connection
 * shut down in the send's direction, which the send then reports. A
 * datagram socket that names its receiver in each send cannot tell and says
 * yes, as Linux does; so does a stream socket that is not connected, whose
 * send fails at once. */
static int writable(const struct unix_sock *s)
{
    const struct unix_sock *p = s->peer;

    if (s->state == US_LISTENING) {
        return 0;
    }
    if (!p || p->closed || (s->shut & SHUT_SEND) || (p->shut & SHUT_RCV)) {
        return 1;
    }
    return !queue_full(p);
}

static short unix_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct unix_sock *s = sock_of(e);
    short mask = 0;

    if (wq) {
        *wq = &s->wq;
    }
    if (s->err) {
        mask |= POLLERR;
    }
    if (s->shut == SHUT_BOTH || (s->type != SOCK_DGRAM && s->state == US_UNCONNECTED)) {
        mask |= POLLHUP;
    }
    if (s->shut & SHUT_RCV) {
        mask |= POLLRDHUP | POLLIN | POLLRDNORM;
    }
    if (s->rq_head || s->acc_head) {
        mask |= POLLIN | POLLRDNORM;
    }
    if (writable(s)) {
        mask |= POLLOUT | POLLWRNORM;
    }
    return (short)((mask & events) | (mask & (POLLERR | POLLHUP)));
}

static int32_t unix_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct unix_sock *s = sock_of(e);
    int32_t n;

    switch (request) {
    case FIONREAD:
        if (s->state == US_LISTENING) {
            return -EINVAL;
        }
        if (s->type == SOCK_STREAM) {
            n = (int32_t)s->rq_bytes;
        } else {
            n = s->rq_head ? (int32_t)s->rq_head->len : 0;
        }
        break;
    case TIOCOUTQ:
        n = (s->type != SOCK_DGRAM && s->peer) ? (int32_t)s->peer->rq_bytes : 0;
        break;
    default:
        return -ENOTTY;
    }
    if (copy_to_user((void __user *)arg, &n, sizeof(n)) != 0) {
        return -EFAULT;
    }
    return 0;
}

static void unix_close(fd_entry_t *e)
{
    struct unix_sock *s = sock_of(e);

    if (s) {
        e->priv = NULL;
        unix_release(s, 0);
    }
}

void unix_ns_gone(const struct netns *ns)
{
    static const struct netns gone;
    struct unix_sock *s;

    for (s = socks; s; s = s->next) {
        if (s->ns == ns) {
            s->ns = &gone;
        }
    }
}
