/*
 * unix.c: AF_UNIX sockets, as unix.h describes them.
 */

#include "unix.h"

#include "../irq.h"
#include "../fs/file.h"
#include "../fs/krffs.h"
#include "../fs/vfs.h"
#include "../proc/process.h"
#include "../proc/mutex.h"
#include "../proc/smp.h"
#include "../lib/kirq.h"
#include "../proc/ns.h"
#include "../proc/wait_queue.h"
#include "../mm/kheap.h"
#include "../mm/paging.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../lib/ksignal.h"
#include "../lib/kstring.h"
#include "../network/socket/socket.h"
#include "../rtc.h"
#include "../timer.h"
#include "../syscall_internal.h"

#define SOCK_CLOEXEC  02000000
#define SOCK_NONBLOCK 04000
#define AF_UNSPEC     0

/* Format: where sun_path starts in a sockaddr_un. */
#define SUN_PATH_OFF 2

/* Policy: the bytes a socket's receive queue takes before a sender waits,
 * Linux's default buffer size, and the datagrams it takes, the value
 * systemd gives Linux's net.unix.max_dgram_qlen at boot, whose kernel default
 * is 10. A message is charged its record and descriptor array beside its
 * bytes, as Linux charges an skb's true size, so that 512 small messages
 * still fit and a queue of messages with descriptors fills by bytes first.
 * wpa_supplicant drops an event and detaches its monitor at a full queue, and
 * wpa_cli reads none while its action runs, so 10 lose events in a two-second
 * action. A stream send is queued in pieces of at most UNIX_CHUNK bytes, so a
 * reader drains the first while the rest waits. */
#define UNIX_BUF        212992u
#define UNIX_DGRAM_QLEN 512u
#define UNIX_CHUNK      65536u
/* Format: the descriptors one message may have, Linux's SCM_MAX_FD. */
#define UNIX_MAX_FDS    253u
/* Policy: the connections a listener queues at most, Linux's somaxconn. */
#define UNIX_SOMAXCONN  4096
/* A socket time limit that waits not at all, which negative seconds set. */
#define UNIX_TIMEO_NOW  (~(uint64_t)0)

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
#define SO_PRIORITY    12
#define SO_LINGER      13
#define SO_PASSCRED    16
#define SO_PEERCRED    17
#define SO_RCVLOWAT    18
#define SO_SNDLOWAT    19
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

/* Credentials, Linux's struct ucred, as SO_PEERCRED and SCM_CREDENTIALS
 * give them; kept as the kernel's ids. */
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

/* Descriptors in flight, under flight_lock from acquisition to installation
 * or close, including sends and receives whose user copies may sleep. */
struct unix_flight {
    struct unix_flight *next;
    uint32_t nfds;
    fd_entry_t *fds;
};

/* A message on a receive queue: `len` bytes at `data`, after the record,
 * and the sender's credentials and name. A stream reader has taken `off`
 * bytes of it so far; the others take it whole. `fds` are the `nfds`
 * descriptors sent with it, each holding its open file as a descriptor in a
 * table would. */
struct unix_msg {
    struct unix_msg *next;
    uint32_t len;
    uint32_t off;
    uint32_t charge; /* what it holds of its queue's `rq_charge`, in bytes */
    int64_t tsec;    /* when it was sent, by `rtc_realtime`: seconds */
    int32_t tnsec;   /* and nanoseconds */
    struct unix_cred cred;
    struct unix_name from;
    struct unix_flight flight;
    uint8_t *data;
};

/*
 * A socket. `refs` counts the descriptors' side, one reference however many
 * descriptors share it; each socket connected to this one; and the accept
 * queue it waits on before accept; and temporary lookups and peer reads.
 * References change atomically; table_lock keeps the last removal from
 * racing a name lookup. The descriptors' side is let go at the
 * last close, and the record is freed when the count reaches zero.
 */
struct unix_sock {
    struct spinlock state_lock; /* state, peer and options */
    struct spinlock queue_lock; /* receive queue and its counts, rcvbuf */
    struct mutex bindlock;      /* a bind across file-system sleeps */
    struct mutex iolock;        /* one receive or queue purge across a copy */
    struct unix_sock *next;
    /* The network namespace it was made in, whose abstract names it alone
     * finds (proc/ns.h). */
    const struct netns *ns;
    uint32_t refs;
    int type;
    int state;
    int closed; /* its descriptors' side has ended */
    uint32_t inode;
    uint32_t uid; /* its maker's effective uid; an accepted one has the listener's */
    /* The name bind gave it, or for an accepted socket the listener's name,
     * which it reports; only a `bound` socket is found by its name. */
    struct unix_name name;
    int bound;
    int bound_fs; /* bound to a path, whose socket file `fs_ref` holds */
    struct vfs_ref fs_ref;
    uint32_t fs_ino;
    struct unix_sock *peer;
    struct unix_msg *rq_head;
    struct unix_msg *rq_tail;
    uint32_t rq_bytes;  /* bytes queued and not yet read */
    uint32_t rq_charge; /* those bytes and the queued messages' records and
                           descriptor arrays, which `rcvbuf` bounds */
    uint32_t rq_count;
    int shut; /* SHUT_RCV and SHUT_SEND */
    int err;  /* a pending positive errno, for SO_ERROR or the next call */
    int passcred;
    uint8_t tstamp;            /* SO_TIMESTAMP and SO_TIMESTAMPNS, as SOCK_RX_TS* bits */
    int priority;              /* SO_PRIORITY, kept for getsockopt; it orders nothing here */
    struct unix_cred peercred; /* what SO_PEERCRED gives, when `has_peercred` */
    int has_peercred;
    int backlog;
    struct unix_sock *acc_head;
    struct unix_sock *acc_tail;
    struct unix_sock *acc_next; /* the next on the listener's queue it is on */
    uint32_t acc_len;
    uint32_t sndbuf; /* bytes */
    uint32_t rcvbuf;
    uint64_t rcvtimeo; /* ticks, 0 for none, UNIX_TIMEO_NOW for no wait */
    uint64_t sndtimeo;
    wait_queue_t wq;
};

/* What a send has besides its bytes: descriptors and credentials. */
struct unix_out {
    struct unix_flight flight;
    int has_cred;
    struct unix_cred cred;
};

/* What a receive gives besides its bytes. */
struct unix_in {
    struct unix_name from;
    struct unix_cred cred;
    int has_cred;
    uint8_t tstamp; /* the timestamp asked for, as SOCK_RX_TS* bits, or 0 */
    int64_t tsec;   /* the message's time, when `tstamp` */
    int32_t tnsec;
    struct unix_flight flight;
    uint32_t flags; /* MSG_TRUNC, MSG_CTRUNC */
};

static int32_t unix_read(fd_entry_t *e, char __user *buf, uint32_t count);
static int32_t unix_write(fd_entry_t *e, const char __user *buf, uint32_t count);
static short unix_poll(fd_entry_t *e, short events, struct wait_queue **wq);
static int32_t unix_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg);
static void unix_close(fd_entry_t *e);

static const struct file_ops unix_ops = {unix_read,  unix_write, unix_poll, unix_ioctl,
                                         unix_close, NULL,       NULL,      FILE_OPS_UNLOCKED};

/* Order: bindlock or iolock, table_lock, state_lock (two by address),
 * queue_lock, flight_lock, wait queues. No spinlock spans a user copy,
 * backing-file operation, reference release or sleep. */
static struct spinlock table_lock = SPINLOCK_INIT_NAMED("unix_table");
static struct spinlock flight_lock = SPINLOCK_INIT_NAMED("unix_flight");
static struct unix_flight *flights;

/* Every socket not yet freed, and its names, under table_lock. */
static struct unix_sock *socks;

/* The messages whose descriptors are being closed, first to last, and
 * whether a call is closing them now (`msg_free`). */
static struct unix_msg *reap_head;
static struct unix_msg *reap_tail;
static int reaping;

void unix_init(void)
{
    socks = NULL;
    reap_head = NULL;
    reap_tail = NULL;
    reaping = 0;
    flights = NULL;
}

/** Add a reference to a live socket, held by the caller or table_lock. */
static void unix_get(struct unix_sock *s)
{
    unsigned int old;

    do {
        old = *(volatile unsigned int *)&s->refs;
    } while (arch_atomic_cmpxchg(&s->refs, old, old + 1u) != old);
}

/** Lock two sockets in address order, once for a self connection, with
 * interrupts off, as Linux's unix_state_double_lock (af_unix.c:1400-1412). */
static uint32_t state_pair(struct unix_sock *a, struct unix_sock *b)
{
    uint32_t flags = kirq_save();

    if (!b || a == b) {
        spin_lock(&a->state_lock);
    } else if ((uintptr_t)a < (uintptr_t)b) {
        spin_lock(&a->state_lock);
        spin_lock_nested(&b->state_lock, 1);
    } else {
        spin_lock(&b->state_lock);
        spin_lock_nested(&a->state_lock, 1);
    }
    return flags;
}

/** Release the state locks taken by state_pair and restore interrupts. */
static void state_unpair(struct unix_sock *a, struct unix_sock *b, uint32_t flags)
{
    if (b && a != b) {
        if ((uintptr_t)a < (uintptr_t)b) {
            spin_unlock(&b->state_lock);
            spin_unlock(&a->state_lock);
        } else {
            spin_unlock(&a->state_lock);
            spin_unlock(&b->state_lock);
        }
    } else {
        spin_unlock(&a->state_lock);
    }
    kirq_restore(flags);
}

/** Hold the current peer, or return NULL; the caller drops the reference. */
static struct unix_sock *peer_get(struct unix_sock *s)
{
    uint32_t flags = spin_lock_irqsave(&s->state_lock);
    struct unix_sock *p = s->peer;

    if (p) {
        unix_get(p);
    }
    spin_unlock_irqrestore(&s->state_lock, flags);
    return p;
}

/** Track descriptors while their owner's call, message or copy can sleep. */
static void flight_enter(struct unix_flight *f)
{
    uint32_t flags = spin_lock_irqsave(&flight_lock);

    f->next = flights;
    flights = f;
    spin_unlock_irqrestore(&flight_lock, flags);
}

/** Stop tracking an empty flight before its owner leaves or is freed. */
static void flight_leave(struct unix_flight *f)
{
    struct unix_flight **link;
    uint32_t flags = spin_lock_irqsave(&flight_lock);

    for (link = &flights; *link && *link != f; link = &(*link)->next) {
    }
    if (*link) {
        *link = f->next;
    }
    spin_unlock_irqrestore(&flight_lock, flags);
}

/** The caller's descriptor table. */
static fd_table_t *table(void)
{
    return current_proc->fds;
}

/** The socket of the descriptor `e`. */
static struct unix_sock *sock_of(const fd_entry_t *e)
{
    return (struct unix_sock *)e->priv;
}

/** Whether a call on `e` with `flags` must not wait: MSG_DONTWAIT, or
 * O_NONBLOCK on the description. */
static int nonblocking(const fd_entry_t *e, int flags)
{
    return (flags & MSG_DONTWAIT) || (e->ofd && (e->ofd->status_flags & OFD_NONBLOCK));
}

int unix_inode_used(uint32_t ino)
{
    const struct unix_sock *s;
    int used = 0;
    uint32_t flags = spin_lock_irqsave(&table_lock);

    for (s = socks; s; s = s->next) {
        if (s->inode == ino) {
            used = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&table_lock, flags);
    return used;
}

int unix_count(void)
{
    struct unix_sock *s;
    int n = 0;
    uint32_t flags = spin_lock_irqsave(&table_lock);

    for (s = socks; s; s = s->next) {
        spin_lock(&s->state_lock);
        n += !s->closed;
        spin_unlock(&s->state_lock);
    }
    spin_unlock_irqrestore(&table_lock, flags);
    return n;
}

int unix_snapshot(struct unix_row **rows)
{
    struct unix_sock *s;
    struct unix_row *out;
    int n = 0, room = unix_count();
    uint32_t flags;

    *rows = NULL;
    if (room == 0) {
        return 0;
    }
    out = kmalloc(sizeof(*out) * (size_t)room);
    if (!out) {
        return -1;
    }
    flags = spin_lock_irqsave(&table_lock);
    for (s = socks; s && n < room; s = s->next) {
        struct unix_sock *peer;

        spin_lock(&s->state_lock);
        if (s->closed || s->ns != proc_netns(current_proc)) {
            spin_unlock(&s->state_lock);
            continue;
        }
        out[n].refs = s->refs;
        out[n].type = s->type;
        out[n].listening = s->state == US_LISTENING;
        out[n].connected = s->state == US_CONNECTED;
        out[n].inode = s->inode;
        out[n].uid = s->uid;
        peer = s->peer;
        out[n].peer = peer ? peer->inode : 0;
        spin_lock(&s->queue_lock);
        out[n].rqueue = s->state == US_LISTENING ? s->acc_len : s->rq_bytes;
        spin_unlock(&s->queue_lock);
        out[n].wqueue = 0;
        if (s->state == US_LISTENING) {
            out[n].wqueue = (uint32_t)s->backlog;
        } else if (peer) {
            spin_lock(&peer->queue_lock);
            out[n].wqueue = peer->rq_bytes;
            spin_unlock(&peer->queue_lock);
        }
        out[n].name_len = s->name.len;
        k_memcpy(out[n].path, s->name.path, s->name.len);
        out[n].path[s->name.len] = '\0';
        spin_unlock(&s->state_lock);
        n++;
    }
    spin_unlock_irqrestore(&table_lock, flags);
    *rows = out;
    return n;
}

uint32_t unix_inode(const fd_entry_t *e)
{
    return sock_of(e)->inode;
}

/** A new unconnected socket of `type`, the caller's, on the list with the
 * descriptors' reference; NULL when the heap has no room. */
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
    bkl_enter();
    s->inode = socket_inode_next();
    bkl_exit();
    s->uid = proc_fsuid(current_proc); /* as Linux owns a socket's inode */
    s->sndbuf = UNIX_BUF;
    s->rcvbuf = UNIX_BUF;
    wait_queue_init(&s->wq);
    spin_lock_init(&s->state_lock, "unix_state");
    spin_lock_init(&s->queue_lock, "unix_queue");
    mutex_init(&s->bindlock, "unix_bind");
    mutex_init(&s->iolock, "unix_io");
    {
        uint32_t flags = spin_lock_irqsave(&table_lock);

        s->next = socks;
        socks = s;
        spin_unlock_irqrestore(&table_lock, flags);
    }
    return s;
}

/** The caller's credentials into `c`: the effective ids SO_PEERCRED
 * reports, or the real ids a message has unless the sender gives others. */
static void own_cred(struct unix_cred *c, int effective)
{
    c->pid = current_proc->tgid;
    c->uid = effective ? current_proc->euid : current_proc->ruid;
    c->gid = effective ? current_proc->egid : current_proc->rgid;
}

/** Credentials of kernel ids as the current process sees them in its user
 * and PID namespaces. */
static struct unix_cred cred_seen(struct unix_cred c)
{
    int locked =
        proc_pidns(current_proc) != &init_pidns || proc_userns(current_proc) != &init_userns;

    if (locked) {
        bkl_enter();
    }
    c.pid = pid_seen(c.pid);
    c.uid = uid_seen(c.uid);
    c.gid = gid_seen(c.gid);
    if (locked) {
        bkl_exit();
    }
    return c;
}

/** Take the references a descriptor in a table holds, for the copy `e` of
 * it that goes in flight; `flight_drop` drops them as a close would. */
static void flight_hold(fd_entry_t *e)
{
    fd_ofd_ref(e->ofd);
    if (e->type == FD_TYPE_SOCKET && e->sock) {
        bkl_enter();
        socket_ref(e->sock);
        bkl_exit();
    }
}

/**
 * Bring a KRFFS file or directory in flight to the node its description
 * has now.
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
 * a message has and the count is lowered. It is closed from a copy, taken
 * from its place first, since the close can sleep and the one being closed
 * is no holder of its own file. A descriptor of a file of the KRFFS
 * root leaves its place only once the root's namespace lock is held
 * (`fd_release_lock`): a release of the file's last descriptor elsewhere
 * counts it until then, and the count and any reclaim of the file its own
 * close makes come in the same hold.
 */
static void flight_drop(fd_entry_t *slot, uint32_t *count)
{
    int big = file_ops_lock(slot);
    struct vfs_mount *locked = fd_release_lock(slot);
    fd_entry_t e;
    uint32_t flags = spin_lock_irqsave(&flight_lock);

    e = *slot;
    if (count) {
        (*count)--;
    } else {
        slot->type = FD_TYPE_NONE;
    }
    spin_unlock_irqrestore(&flight_lock, flags);
    flight_sync(&e);
    fd_release_backing(&e);
    if (locked) {
        vfs_ns_unlock(locked);
    }
    file_ops_unlock(big);
}

/** Track a receive before it takes descriptors from a message, until its
 * installation or close ends. The flight registry also tracks outgoing
 * sends, queued messages and messages on the reap list. */
static void transit_enter(struct unix_in *in)
{
    flight_enter(&in->flight);
}

/** Stop tracking a receive whose descriptors are installed or closed. */
static void transit_leave(struct unix_in *in)
{
    flight_leave(&in->flight);
}

/** A message of `len` bytes, its data after the record and nothing else
 * set; NULL when the heap has no room. */
static struct unix_msg *msg_alloc(uint32_t len)
{
    struct unix_msg *m = kmalloc(sizeof(*m) + len);

    if (!m) {
        return NULL;
    }
    k_memset(m, 0, sizeof(*m));
    m->len = len;
    m->data = (uint8_t *)(m + 1);
    flight_enter(&m->flight);
    return m;
}

/**
 * Free the message `m` and close the descriptors sent with it.
 *
 * Closing a descriptor in flight can close an AF_UNIX socket, whose queue
 * can have the only descriptor of another, and so on down a chain as long
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
    uint32_t flags = spin_lock_irqsave(&flight_lock);

    m->next = NULL;
    if (reap_tail) {
        reap_tail->next = m;
    } else {
        reap_head = m;
    }
    reap_tail = m;
    if (reaping) {
        spin_unlock_irqrestore(&flight_lock, flags);
        return;
    }
    reaping = 1;
    for (;;) {
        m = reap_head;
        if (!m) {
            reaping = 0;
            spin_unlock_irqrestore(&flight_lock, flags);
            return;
        }
        if (m->flight.nfds) {
            spin_unlock_irqrestore(&flight_lock, flags);
            flight_drop(&m->flight.fds[m->flight.nfds - 1], &m->flight.nfds);
            flags = spin_lock_irqsave(&flight_lock);
            continue;
        }
        reap_head = m->next;
        if (!reap_head) {
            reap_tail = NULL;
        }
        spin_unlock_irqrestore(&flight_lock, flags);
        flight_leave(&m->flight);
        kfree(m->flight.fds);
        kfree(m);
        flags = spin_lock_irqsave(&flight_lock);
    }
}

/** Whether one of the `n` descriptors in flight at `fds` is the KRFFS file
 * whose description has the node `node` (`flight_sync`). */
static int fds_hold(const fd_entry_t *fds, uint32_t n, const void *node)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        const fd_entry_t *e = &fds[i];

        if (node && fd_root_node(e) == node) {
            return 1;
        }
    }
    return 0;
}

int unix_inflight_holds(const void *node)
{
    const struct unix_flight *f;
    int held = 0;
    uint32_t flags = spin_lock_irqsave(&flight_lock);

    for (f = flights; f; f = f->next) {
        if (fds_hold(f->fds, f->nfds, node)) {
            held = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&flight_lock, flags);
    return held;
}

/** Whether `s`'s receive queue is full: at `rcvbuf` bytes charged, or for
 * a datagram or sequenced-packet socket at more than UNIX_DGRAM_QLEN messages
 * as well. */
static int queue_full(const struct unix_sock *s)
{
    if (s->type == SOCK_STREAM) {
        return s->rq_charge >= s->rcvbuf;
    }
    return s->rq_count > UNIX_DGRAM_QLEN || s->rq_charge >= s->rcvbuf;
}

/** Append `m`, with its descriptors attached, to `s`'s receive queue,
 * charging it its record, bytes and descriptor array. queue_lock is held;
 * the caller wakes data waiters once it releases the socket locks. */
static void enqueue(struct unix_sock *s, struct unix_msg *m)
{
    m->charge = (uint32_t)sizeof(*m) + m->len + m->flight.nfds * (uint32_t)sizeof(fd_entry_t);
    m->next = NULL;
    if (s->rq_tail) {
        s->rq_tail->next = m;
    } else {
        s->rq_head = m;
    }
    s->rq_tail = m;
    s->rq_bytes += m->len;
    s->rq_charge += m->charge;
    s->rq_count++;
}

/**
 * Wake whoever waits for room in `s`'s queue: a sender to it waits on its
 * queue, where the room changes none of `s`'s own events, and a socket
 * connected to it polls on its own, where it is room to write. Streams
 * and sequenced packets have one reciprocal peer, pinned by state_lock;
 * only datagrams need the table walk for every socket connected to them.
 */
static void wake_writers(struct unix_sock *s)
{
    struct unix_sock *o;
    uint32_t flags;

    wq_wake(&s->wq, 0);
    if (s->type != SOCK_DGRAM) {
        flags = spin_lock_irqsave(&s->state_lock);
        if (s->peer) {
            wq_wake(&s->peer->wq, WQ_ROOM);
        }
        spin_unlock_irqrestore(&s->state_lock, flags);
        return;
    }
    flags = spin_lock_irqsave(&table_lock);
    for (o = socks; o; o = o->next) {
        spin_lock(&o->state_lock);
        if (o->peer == s) {
            wq_wake(&o->wq, WQ_ROOM);
        }
        spin_unlock(&o->state_lock);
    }
    spin_unlock_irqrestore(&table_lock, flags);
}

/** Take the head message off `s`'s queue, with whatever of it was not
 * read; queue_lock is held. The caller frees it and wakes writers with
 * no socket spinlock held. */
static struct unix_msg *dequeue(struct unix_sock *s)
{
    struct unix_msg *m = s->rq_head;

    s->rq_head = m->next;
    if (!s->rq_head) {
        s->rq_tail = NULL;
    }
    s->rq_bytes -= m->len - m->off;
    s->rq_charge -= m->charge;
    s->rq_count--;
    m->next = NULL;
    return m;
}

/** Detach every message under queue_lock, then close its descriptors with
 * no socket lock held. The socket has ended, so no receive can be active. */
static void purge(struct unix_sock *s)
{
    struct unix_msg *head, *m;
    uint32_t flags = spin_lock_irqsave(&s->queue_lock);

    head = s->rq_head;
    s->rq_head = NULL;
    s->rq_tail = NULL;
    s->rq_bytes = 0;
    s->rq_charge = 0;
    s->rq_count = 0;
    spin_unlock_irqrestore(&s->queue_lock, flags);
    while (head) {
        m = head;
        head = m->next;
        msg_free(m);
    }
    wake_writers(s);
}

/** Drop a reference to `s`; the last drops its queue, takes it off the
 * list and frees it. No socket spinlock is held. */
static void unix_put(struct unix_sock *s)
{
    struct unix_sock **link;
    unsigned int old;
    uint32_t flags = spin_lock_irqsave(&table_lock);

    do {
        old = *(volatile unsigned int *)&s->refs;
    } while (arch_atomic_cmpxchg(&s->refs, old, old - 1u) != old);
    if (old != 1) {
        spin_unlock_irqrestore(&table_lock, flags);
        return;
    }
    for (link = &socks; *link && *link != s; link = &(*link)->next) {
    }
    if (*link) {
        *link = s->next;
    }
    spin_unlock_irqrestore(&table_lock, flags);
    purge(s);
    kfree(s);
}

/** Forget the name: an abstract one is free at once, and the socket file
 * of a path stays until it is removed, as in Linux. */
static void unbind(struct unix_sock *s)
{
    struct vfs_ref ref;
    int bound_fs;
    uint32_t flags = spin_lock_irqsave(&table_lock);

    spin_lock(&s->state_lock);
    bound_fs = s->bound_fs;
    ref = s->fs_ref;
    s->bound_fs = 0;
    s->bound = 0;
    spin_unlock(&s->state_lock);
    spin_unlock_irqrestore(&table_lock, flags);
    if (bound_fs) {
        bkl_enter();
        fs_socket_unbind(&ref);
        bkl_exit();
    }
}

static void unix_release(struct unix_sock *s, int embryo);

/** End a listener's queued connections, never accepted, as if closed. */
static void drop_embryos(struct unix_sock *s)
{
    struct unix_sock *c, *head;
    uint32_t flags = spin_lock_irqsave(&s->state_lock);

    head = s->acc_head;
    s->acc_head = NULL;
    s->acc_tail = NULL;
    s->acc_len = 0;
    spin_unlock_irqrestore(&s->state_lock, flags);
    while (head) {
        c = head;
        head = c->acc_next;
        c->acc_next = NULL;
        unix_release(c, 1);
    }
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
    struct unix_sock *peer;
    int reset;
    uint32_t flags = spin_lock_irqsave(&s->state_lock);

    s->closed = 1;
    s->shut = SHUT_BOTH;
    peer = s->peer;
    s->peer = NULL;
    spin_lock(&s->queue_lock);
    reset = s->rq_head != NULL || embryo;
    spin_unlock(&s->queue_lock);
    spin_unlock_irqrestore(&s->state_lock, flags);
    unbind(s);
    drop_embryos(s);
    if (peer) {
        flags = spin_lock_irqsave(&peer->state_lock);
        if (s->type != SOCK_DGRAM && peer->peer == s) {
            peer->shut = SHUT_BOTH;
            if (reset) {
                peer->err = ECONNRESET;
            }
        }
        spin_unlock_irqrestore(&peer->state_lock, flags);
        wq_wake_all(&peer->wq);
        unix_put(peer);
    }
    wq_wake_all(&s->wq);
    purge(s);
    unix_put(s);
}

/* The wait of `unix_wait`: its condition, or the socket's time limit. */
struct timed_cond {
    wq_cond_fn cond;
    void *arg;
    uint64_t deadline;
    int timed;
};

/** The wait condition of `unix_wait`, `arg` its `struct timed_cond`: the
 * caller's condition holds, or the time limit has passed. */
static int timed_ready(void *arg)
{
    struct timed_cond *t = (struct timed_cond *)arg;

    return t->cond(t->arg) || (t->timed && tick_count() >= t->deadline);
}

/** Test the signal-work flag's pending signal under the big lock, which
 * protects signal_pending's queue changes, with no socket lock held. */
static int unix_signal_pending(void)
{
    int big = !bkl_held();
    int pending;

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
 * Wait on `wq` until `cond(arg)`, a signal, or the end of the socket time
 * limit `timeo` in ticks, 0 for none and UNIX_TIMEO_NOW for no wait at all.
 * Returns 0 when the condition holds, -EINTR for a signal, -EAGAIN when the
 * time ran out, as Linux reports it. The wait tests only the signal-work
 * flag, as Linux tests TIF_SIGPENDING; signal_pending runs after it under
 * the big lock, since it changes the signal queues (unix_signal_pending).
 * The deadline is armed again before each retry, since the signal's wake
 * cleared it (sched_wake_signal) and the signal may have gone to a sibling
 * thread; Linux keeps the remaining timeout the same way
 * (net/unix/af_unix.c:2608).
 */
static int unix_wait(wait_queue_t *wq, wq_cond_fn cond, void *arg, uint64_t timeo)
{
    struct timed_cond t;
    int rc;

    if (timeo == UNIX_TIMEO_NOW) {
        return cond(arg) ? 0 : -EAGAIN;
    }
    t.cond = cond;
    t.arg = arg;
    t.timed = timeo != 0;
    t.deadline = rtc_ticks_now() + timeo + 1u;
    do {
        if (t.timed) {
            current_proc->wakeup_tick = t.deadline;
        }
        rc = wq_wait_event_sigflag(wq, timed_ready, &t, "unix_wait");
    } while (rc == -EINTR && !unix_signal_pending());
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

/** Give the reserved descriptor `fd` the socket `s`, which takes the
 * reference the caller had, with the creation `flags` SOCK_CLOEXEC and
 * SOCK_NONBLOCK. */
static void publish(int fd, struct unix_sock *s, int flags)
{
    fd_entry_t *e = fd_slot(table(), fd);

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
    fd_install(table(), fd);
}

/** Check the type `*type` and the protocol of a new socket: 0, or
 * `unix_socket`'s -ESOCKTNOSUPPORT or -EPROTONOSUPPORT. */
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

/** Join `a` and `b` as the two ends of one connection, each taking a
 * reference to the other. */
static void join(struct unix_sock *a, struct unix_sock *b)
{
    a->peer = b;
    unix_get(b);
    b->peer = a;
    unix_get(a);
    a->state = US_CONNECTED;
    b->state = US_CONNECTED;
}

/**
 * The two descriptors are reserved and their numbers copied to `sv` before
 * either is published, so a failed copy leaves nothing that another thread
 * could have used, and both sockets are released. Each socket's peer
 * credentials are the caller's.
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
    {
        uint32_t saved = state_pair(a, b);

        join(a, b);
        own_cred(&a->peercred, 1);
        b->peercred = a->peercred;
        a->has_peercred = 1;
        b->has_peercred = 1;
        state_unpair(a, b, saved);
    }
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
 * the address. EINVAL for a length or a family that is not AF_UNIX's,
 * EFAULT for an address it cannot read.
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

/** Whether `n` is a name in the abstract namespace. */
static int abstract(const struct unix_name *n)
{
    return n->len > 0 && n->path[0] == '\0';
}

/** Whether `a` and `b` are the same name. */
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

/** The length of the sockaddr_un a name is reported in: the family alone
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
 * receive, with `sender` set, reports a sender with no name as a length of
 * 0. Returns 0, -EINVAL for a negative length, or -EFAULT.
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
    struct unix_sock *s, *found = NULL;
    struct vfs_mount *mnt = NULL;
    uint32_t ino = 0, flags;

    if (!abstract(n)) {
        int32_t rc;

        bkl_enter();
        rc = fs_socket_lookup(n->path, &mnt, &ino);
        if (rc != 0) {
            *err = rc;
            bkl_exit();
            return NULL;
        }
    }
    flags = spin_lock_irqsave(&table_lock);
    for (s = socks; s; s = s->next) {
        spin_lock(&s->state_lock);
        if (s->bound && !s->closed &&
            (abstract(n) ? (!s->bound_fs && s->type == type && same_name(&s->name, n) &&
                            s->ns == proc_netns(current_proc))
                         : (s->bound_fs && vfs_mount_base(s->fs_ref.mnt) == vfs_mount_base(mnt) &&
                            s->fs_ino == ino))) {
            unix_get(s);
            found = s;
        }
        spin_unlock(&s->state_lock);
        if (found) {
            break;
        }
    }
    spin_unlock_irqrestore(&table_lock, flags);
    if (!abstract(n)) {
        bkl_exit();
    }
    if (!found) {
        *err = -ECONNREFUSED;
    }
    return found;
}

/** Claim an abstract name under the name table lock and the socket's state
 * lock; bindlock is held. Return -EADDRINUSE if another live socket owns it. */
static int32_t bind_abstract(struct unix_sock *s, const struct unix_name *n)
{
    struct unix_sock *o;
    int32_t rc = 0;
    uint32_t flags = spin_lock_irqsave(&table_lock);

    for (o = socks; o; o = o->next) {
        if (o->bound && !o->bound_fs && o->type == s->type && o->ns == s->ns &&
            same_name(&o->name, n)) {
            rc = -EADDRINUSE;
            break;
        }
    }
    if (rc == 0) {
        spin_lock(&s->state_lock);
        s->name = *n;
        s->bound = 1;
        spin_unlock(&s->state_lock);
    }
    spin_unlock_irqrestore(&table_lock, flags);
    return rc;
}

/** Bind an unnamed socket, whose bind lock the caller holds, to a free
 * abstract name of five hex digits, as Linux's autobind does; -ENOSPC when
 * every one is taken. */
static int32_t autobind(struct unix_sock *s)
{
    static uint32_t order;
    static const char hex[] = "0123456789abcdef";
    struct unix_name n;
    uint32_t tries, i, v, flags;

    for (tries = 0; tries < 0x100000u; tries++) {
        flags = spin_lock_irqsave(&table_lock);
        v = order++ & 0xFFFFFu;
        spin_unlock_irqrestore(&table_lock, flags);
        k_memset(&n, 0, sizeof(n));
        n.len = 6;
        for (i = 0; i < 5; i++) {
            n.path[5 - i] = hex[(v >> (4 * i)) & 15u];
        }
        if (bind_abstract(s, &n) == 0) {
            return 0;
        }
    }
    return -ENOSPC;
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
    return mutex_lock_interruptible(&s->bindlock);
}

/** Let go of `s`'s bind lock and wake its waiters. */
static void bind_unlock(struct unix_sock *s)
{
    mutex_unlock(&s->bindlock);
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
    mutex_lock(&s->iolock);
}

/** Let go of `s`'s receive lock and wake its waiters. */
static void recv_unlock(struct unix_sock *s)
{
    mutex_unlock(&s->iolock);
}

/** Give a socket with SO_PASSCRED and no name the automatic one, before a
 * connect or a send, as Linux does; 0, or the error of `bind_lock` or
 * `autobind`. */
static int32_t passcred_name(struct unix_sock *s)
{
    uint32_t flags = spin_lock_irqsave(&s->state_lock);
    int needed = s->passcred && !s->bound;
    int32_t rc;

    spin_unlock_irqrestore(&s->state_lock, flags);
    if (!needed) {
        return 0;
    }
    rc = bind_lock(s);
    if (rc == 0) {
        rc = s->bound ? 0 : autobind(s);
        bind_unlock(s);
    }
    return rc;
}

/** Bind the socket `s`, whose bind lock the caller holds, to the name `n`:
 * 0, or `unix_bind`'s errors but those of the address and the lock. */
static int32_t bind_name(struct unix_sock *s, const struct unix_name *n)
{
    struct vfs_ref ref;
    uint32_t ino, flags;
    int32_t rc;

    if (s->bound || s->closed) {
        return -EINVAL;
    }
    if (n->len == 0) {
        return autobind(s);
    }
    if (abstract(n)) {
        return bind_abstract(s, n);
    }
    bkl_enter();
    rc = fs_socket_bind(n->path, 0777u & ~current_proc->fs->umask, &ref, &ino);
    bkl_exit();
    if (rc != 0) {
        return rc;
    }
    flags = spin_lock_irqsave(&table_lock);
    spin_lock(&s->state_lock);
    s->fs_ref = ref;
    s->fs_ino = ino;
    s->bound_fs = 1;
    s->name = *n;
    s->bound = 1;
    spin_unlock(&s->state_lock);
    spin_unlock_irqrestore(&table_lock, flags);
    return 0;
}

/**
 * The name is judged free and taken under the socket's bind lock
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

/** The connects waiting for room in the queue are woken, since a larger
 * backlog may have made room. */
int32_t unix_listen(fd_entry_t *e, int backlog)
{
    struct unix_sock *s = sock_of(e);
    uint32_t flags;
    int32_t rc = 0;

    if (s->type == SOCK_DGRAM) {
        return -EOPNOTSUPP;
    }
    flags = spin_lock_irqsave(&s->state_lock);
    if (s->state == US_CONNECTED || !s->bound) {
        rc = -EINVAL;
    } else {
        if (backlog < 0 || backlog > UNIX_SOMAXCONN) {
            backlog = UNIX_SOMAXCONN;
        }
        s->backlog = backlog;
        s->state = US_LISTENING;
        own_cred(&s->peercred, 1);
        s->has_peercred = 1;
    }
    spin_unlock_irqrestore(&s->state_lock, flags);
    wq_wake_all(&s->wq);
    return rc;
}

/** Whether a connect waiting for room on the listener `l` can stop: there
 * is room, or `l` takes connections no more. */
static int listener_has_room(void *arg)
{
    struct unix_sock *l = (struct unix_sock *)arg;
    uint32_t flags = spin_lock_irqsave(&l->state_lock);
    int ready = l->closed || l->state != US_LISTENING || (l->shut & SHUT_RCV) ||
                (int)l->acc_len <= l->backlog;

    spin_unlock_irqrestore(&l->state_lock, flags);
    return ready;
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
    struct unix_sock *old;
    struct unix_msg *head = NULL, *m;
    uint32_t flags;

    recv_lock(s);
    flags = state_pair(s, target);
    old = s->peer;
    s->peer = target;
    if (old && old != target) {
        spin_lock(&s->queue_lock);
        head = s->rq_head;
        s->rq_head = NULL;
        s->rq_tail = NULL;
        s->rq_bytes = s->rq_charge = s->rq_count = 0;
        spin_unlock(&s->queue_lock);
    }
    state_unpair(s, target, flags);
    recv_unlock(s);
    if (old) {
        flags = spin_lock_irqsave(&old->state_lock);
        if (head && !old->closed && old->peer == s) {
            old->err = ECONNRESET;
        }
        spin_unlock_irqrestore(&old->state_lock, flags);
        wq_wake_all(&old->wq);
        unix_put(old);
    }
    while (head) {
        m = head;
        head = m->next;
        msg_free(m);
    }
    wake_writers(s);
}

/**
 * connect(2) of a datagram socket: the target becomes the peer its sends
 * without a name go to, or with AF_UNSPEC it has none (`dgram_repeer`). A
 * target connected to another socket gives EPERM and the connection is left
 * as it was, as Linux's unix_may_send has it.
 */
static int32_t connect_dgram(struct unix_sock *s, uintptr_t addr, uint32_t addrlen)
{
    struct unix_name n;
    struct unix_sock *target, *old;
    struct unix_msg *head = NULL, *m;
    uint8_t family[2];
    uint32_t flags;
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
    recv_lock(s);
    flags = state_pair(s, target);
    if (target->type != SOCK_DGRAM) {
        rc = -EPROTOTYPE;
    } else if (target->closed) {
        rc = -ECONNREFUSED;
    } else if (target->peer && target->peer != s) {
        rc = -EPERM;
    } else {
        rc = 0;
    }
    old = NULL;
    if (rc == 0) {
        old = s->peer;
        s->peer = target;
        if (old && old != target) {
            spin_lock(&s->queue_lock);
            head = s->rq_head;
            s->rq_head = NULL;
            s->rq_tail = NULL;
            s->rq_bytes = s->rq_charge = s->rq_count = 0;
            spin_unlock(&s->queue_lock);
        }
    }
    state_unpair(s, target, flags);
    recv_unlock(s);
    if (rc != 0) {
        unix_put(target);
    }
    if (old) {
        flags = spin_lock_irqsave(&old->state_lock);
        if (head && !old->closed && old->peer == s) {
            old->err = ECONNRESET;
        }
        spin_unlock_irqrestore(&old->state_lock, flags);
        wq_wake_all(&old->wq);
        unix_put(old);
    }
    while (head) {
        m = head;
        head = m->next;
        msg_free(m);
    }
    wake_writers(s);
    return rc;
}

/**
 * Why the stream or sequenced-packet socket `s` cannot connect to `target`
 * now, or 0 when it can. A listener shut down for reading is
 * ECONNREFUSED, as in Linux.
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
    uint32_t flags;
    uint64_t timeo;
    int32_t rc;

    if (s->type == SOCK_DGRAM) {
        return connect_dgram(s, addr, addrlen);
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
    c = unix_alloc(s->type);
    if (!c) {
        return -ENOMEM;
    }
    for (;;) {
        target = lookup(&n, s->type, &rc);
        if (!target) {
            break;
        }
        flags = state_pair(s, target);
        rc = target->closed ? -ECONNREFUSED : connect_refusal(s, target);
        if (rc == 0 && (int)target->acc_len <= target->backlog) {
            spin_lock_nested(&c->state_lock, 2);
            c->uid = target->uid;
            c->name = target->name;
            c->passcred = target->passcred;
            own_cred(&c->peercred, 1);
            c->has_peercred = 1;
            join(s, c);
            s->peercred = target->peercred;
            s->has_peercred = 1;
            spin_unlock(&c->state_lock);
            if (target->acc_tail) {
                target->acc_tail->acc_next = c;
            } else {
                target->acc_head = c;
            }
            target->acc_tail = c;
            target->acc_len++;
            state_unpair(s, target, flags);
            wq_wake(&target->wq, WQ_DATA);
            unix_put(target);
            return 0;
        }
        timeo = s->sndtimeo;
        state_unpair(s, target, flags);
        if (rc == 0 && nonblocking(e, 0)) {
            rc = -EAGAIN;
        }
        if (rc == 0) {
            rc = unix_wait(&target->wq, listener_has_room, target, timeo);
        }
        unix_put(target);
        if (rc != 0) {
            break;
        }
    }
    unix_put(c);
    return rc;
}

/** Whether an accept waiting on the listener `l` can stop: a connection is
 * queued, or `l` takes connections no more. */
static int has_connection(void *arg)
{
    struct unix_sock *l = (struct unix_sock *)arg;
    uint32_t flags = spin_lock_irqsave(&l->state_lock);
    int ready = l->acc_head != NULL || l->state != US_LISTENING || (l->shut & SHUT_RCV);

    spin_unlock_irqrestore(&l->state_lock, flags);
    return ready;
}

/**
 * The descriptor is reserved before the connection is taken off the queue,
 * so a caller with none free leaves it queued, and published only after the
 * peer's name is copied out: a copy that fails frees the slot and ends the
 * connection as a close of it would, and no other thread can have used the
 * socket or freed the peer whose name is copied. An address with no length
 * pointer is such a failure, EFAULT, as in Linux. A listener shut down for
 * reading still hands out the connections queued before the shutdown, and
 * then fails with EINVAL, as Linux's does.
 */
int32_t unix_accept(fd_entry_t *e, uintptr_t addr, uintptr_t addrlen_ptr, int flags)
{
    struct unix_sock *s = sock_of(e), *c;
    struct unix_name n;
    uint32_t saved;
    uint64_t timeo;
    int32_t rc;
    int fd;

    if (s->type == SOCK_DGRAM) {
        return -EOPNOTSUPP;
    }
    if (flags & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) {
        return -EINVAL;
    }
    fd = reserve();
    if (fd < 0) {
        return fd;
    }
    for (;;) {
        saved = spin_lock_irqsave(&s->state_lock);
        c = s->acc_head;
        if (c) {
            s->acc_head = c->acc_next;
            if (!s->acc_head) {
                s->acc_tail = NULL;
            }
            c->acc_next = NULL;
            s->acc_len--;
            spin_unlock_irqrestore(&s->state_lock, saved);
            break;
        }
        rc = s->state != US_LISTENING || (s->shut & SHUT_RCV) ? -EINVAL : 0;
        timeo = s->rcvtimeo;
        spin_unlock_irqrestore(&s->state_lock, saved);
        if (rc == 0 && nonblocking(e, 0)) {
            rc = -EAGAIN;
        }
        if (rc == 0) {
            rc = unix_wait(&s->wq, has_connection, s, timeo);
        }
        if (rc != 0) {
            fd_free(table(), fd);
            return rc;
        }
    }
    wq_wake(&s->wq, 0);
    if (addr) {
        struct unix_sock *peer = peer_get(c);

        k_memset(&n, 0, sizeof(n));
        if (peer) {
            saved = spin_lock_irqsave(&peer->state_lock);
            n = peer->name;
            spin_unlock_irqrestore(&peer->state_lock, saved);
            unix_put(peer);
        }
        rc = put_name(addr, addrlen_ptr, &n, 0);
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
    struct unix_sock *s = sock_of(e), *p = peer ? peer_get(s) : NULL;
    struct unix_name n;
    uint32_t flags;

    if (peer && !p) {
        return -ENOTCONN;
    }
    flags = state_pair(s, p);
    n = p ? p->name : s->name;
    state_unpair(s, p, flags);
    if (p) {
        unix_put(p);
    }
    return put_name(addr, addrlen_ptr, &n, 0);
}

/**
 * A connection's other end has the matching direction shut too. Whoever
 * waits on either socket is woken, and so are the sockets connected to
 * `s`, whose sends to it now fail and which poll therefore reports
 * writable.
 */
int32_t unix_shutdown(fd_entry_t *e, int how)
{
    struct unix_sock *s = sock_of(e), *p;
    uint32_t flags;
    int mode;

    if (how < 0 || how > 2) {
        return -EINVAL;
    }
    mode = how + 1;
    flags = spin_lock_irqsave(&s->state_lock);
    s->shut |= mode;
    p = s->type != SOCK_DGRAM ? s->peer : NULL;
    if (p) {
        unix_get(p);
    }
    spin_unlock_irqrestore(&s->state_lock, flags);
    if (p) {
        flags = spin_lock_irqsave(&p->state_lock);
        p->shut |= ((mode & SHUT_RCV) << 1) | ((mode & SHUT_SEND) >> 1);
        spin_unlock_irqrestore(&p->state_lock, flags);
        wq_wake_all(&p->wq);
        unix_put(p);
    }
    wq_wake_all(&s->wq);
    wake_writers(s);
    return 0;
}

/** A time limit of SO_RCVTIMEO or SO_SNDTIMEO, seconds and microseconds,
 * in ticks into `*ticks`, 0 for none (`tick_span`), as Linux's
 * sock_set_timeout keeps it: negative seconds wait not at all
 * (UNIX_TIMEO_NOW). -EDOM for microseconds out of range. */
static int32_t timeval_ticks(const int64_t tv[2], uint64_t *ticks)
{
    if (tv[1] < 0 || tv[1] >= 1000000) {
        return -EDOM;
    }
    *ticks = tv[0] < 0 ? UNIX_TIMEO_NOW : tick_span(tv[0], (uint32_t)tv[1] * 1000u);
    return 0;
}

/** The seconds and microseconds of the time limit `ticks` into `tv`, 0 for
 * none or none to wait, as Linux's sock_get_timeout gives it. */
static void ticks_timeval(uint64_t ticks, int64_t tv[2])
{
    uint32_t rem = 0;

    tv[0] = 0;
    tv[1] = 0;
    if (ticks != UNIX_TIMEO_NOW && ticks != 0) {
        tv[0] = (int64_t)tick_div(ticks, TICK_HZ, &rem);
        tv[1] = (int64_t)rem * TICK_US;
    }
}

/** A buffer size set by SO_SNDBUF or SO_RCVBUF: twice the value asked, at
 * least 4608 and at most twice Linux's limit, as Linux keeps it. */
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
    int32_t v = 0, rc = 0;
    uint32_t saved;

    if (level != SOL_SOCKET) {
        return -ENOPROTOOPT;
    }
    if (name == SO_RCVTIMEO_OLD || name == SO_SNDTIMEO_OLD || name == SO_RCVTIMEO_NEW ||
        name == SO_SNDTIMEO_NEW) {
        int64_t tv[2];
        uint64_t ticks;
        int32_t err = sock_timeo_get(name, val, len, tv);

        if (err != 0) {
            return err;
        }
        if (timeval_ticks(tv, &ticks) != 0) {
            return -EDOM;
        }
        saved = spin_lock_irqsave(&s->state_lock);
        if (name == SO_RCVTIMEO_OLD || name == SO_RCVTIMEO_NEW) {
            s->rcvtimeo = ticks;
        } else {
            s->sndtimeo = ticks;
        }
        spin_unlock_irqrestore(&s->state_lock, saved);
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
    saved = spin_lock_irqsave(&s->state_lock);
    switch (name) {
    case SO_PASSCRED:
        s->passcred = v != 0;
        break;
    case SO_TIMESTAMP_OLD:
    case SO_TIMESTAMPNS_OLD:
    case SO_TIMESTAMP_NEW:
    case SO_TIMESTAMPNS_NEW:
        s->tstamp = sock_tstamp_set(s->tstamp, name, v);
        break;
    case SO_PRIORITY:
        if ((v < 0 || v > 6) && !cred_privileged()) {
            rc = -EPERM;
            break;
        }
        s->priority = v;
        break;
    case SO_SNDBUF:
    case SO_SNDBUFFORCE:
        s->sndbuf = buf_size(v);
        break;
    case SO_RCVBUF:
    case SO_RCVBUFFORCE:
        spin_lock(&s->queue_lock);
        s->rcvbuf = buf_size(v);
        spin_unlock(&s->queue_lock);
        break;
    case SO_REUSEADDR:
    case SO_KEEPALIVE:
    case SO_BROADCAST:
    case SO_RCVLOWAT:
    case SO_SNDLOWAT:
        break;
    default:
        rc = -ENOPROTOOPT;
        break;
    }
    spin_unlock_irqrestore(&s->state_lock, saved);
    if (name == SO_RCVBUF || name == SO_RCVBUFFORCE) {
        wake_writers(s);
    }
    return rc;
}

int32_t unix_getsockopt(fd_entry_t *e, int level, int name, uintptr_t val, uintptr_t len_ptr)
{
    struct unix_sock *s = sock_of(e);
    struct unix_cred c = {0, 0xFFFFFFFFu, 0xFFFFFFFFu};
    uint8_t out[16];
    uint32_t n = 4, saved;
    int32_t given, v = 0;
    int64_t tv[2];

    if (level != SOL_SOCKET) {
        return -ENOPROTOOPT;
    }
    if (copy_from_user(&given, (const void __user *)len_ptr, sizeof(given)) != 0) {
        return -EFAULT;
    }
    if (given < 0) {
        return -EINVAL;
    }
    saved = spin_lock_irqsave(&s->state_lock);
    spin_lock(&s->queue_lock);
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
    case SO_TIMESTAMP_OLD:
    case SO_TIMESTAMPNS_OLD:
    case SO_TIMESTAMP_NEW:
    case SO_TIMESTAMPNS_NEW:
        v = sock_tstamp_get(s->tstamp, name);
        break;
    case SO_PRIORITY:
        v = s->priority;
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
        if (s->has_peercred) {
            c = s->peercred;
        }
        n = sizeof(c);
        break;
    case SO_RCVTIMEO_OLD:
    case SO_RCVTIMEO_NEW:
        ticks_timeval(s->rcvtimeo, tv);
        n = sock_timeo_put(name, tv, out);
        break;
    case SO_SNDTIMEO_OLD:
    case SO_SNDTIMEO_NEW:
        ticks_timeval(s->sndtimeo, tv);
        n = sock_timeo_put(name, tv, out);
        break;
    default:
        spin_unlock(&s->queue_lock);
        spin_unlock_irqrestore(&s->state_lock, saved);
        return -ENOPROTOOPT;
    }
    spin_unlock(&s->queue_lock);
    spin_unlock_irqrestore(&s->state_lock, saved);
    if (name == SO_PEERCRED) {
        c = cred_seen(c);
        k_memcpy(out, &c, sizeof(c));
    } else if (name != SO_RCVTIMEO_OLD && name != SO_RCVTIMEO_NEW && name != SO_SNDTIMEO_OLD &&
               name != SO_SNDTIMEO_NEW) {
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

/** Copy `n` bytes of the caller's buffers from `skip` on to `k`, or with
 * `to_user` from `k` to them; 0, or -EFAULT. */
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

/** Add to `out` the `n` descriptors of one SCM_RIGHTS message, whose
 * numbers are at `data`, each with the references of a descriptor taken.
 * -EINVAL past UNIX_MAX_FDS in all, -EBADF for one not open, -ENOMEM,
 * -EFAULT; the ones added before an error stay in `out`. */
static int32_t get_rights(uintptr_t data, uint32_t n, struct unix_out *out)
{
    fd_entry_t *grown, *old;
    uint32_t i, flags;

    if (out->flight.nfds + n > UNIX_MAX_FDS) {
        return -EINVAL;
    }
    grown = kmalloc(sizeof(fd_entry_t) * (out->flight.nfds + n + 1u));
    if (!grown) {
        return -ENOMEM;
    }
    flags = spin_lock_irqsave(&flight_lock);
    old = out->flight.fds;
    if (out->flight.nfds) {
        k_memcpy(grown, old, sizeof(fd_entry_t) * out->flight.nfds);
    }
    out->flight.fds = grown;
    spin_unlock_irqrestore(&flight_lock, flags);
    kfree(old);
    for (i = 0; i < n; i++) {
        int32_t fd;
        fd_entry_t *e, copy;

        if (copy_from_user(&fd, (const void __user *)(data + 4u * i), sizeof(fd)) != 0) {
            return -EFAULT;
        }
        e = fd_get(table(), fd);
        if (!e) {
            return -EBADF;
        }
        flags = fd_table_lock(table());
        copy = *e;
        fd_table_unlock(table(), flags);
        copy.flags &= ~(uint32_t)(FD_FLAG_CLOEXEC | FD_FLAG_CLOSING | FD_FLAG_RELEASING);
        flight_hold(&copy);
        flags = spin_lock_irqsave(&flight_lock);
        out->flight.fds[out->flight.nfds] = copy;
        out->flight.nfds++;
        spin_unlock_irqrestore(&flight_lock, flags);
    }
    return 0;
}

/** The credentials of an SCM_CREDENTIALS message at `data` into `out`: the
 * sender's own process id, the thread group's and not a thread's, and ids,
 * unless it is privileged, as Linux checks them. They are given in the
 * sender's namespaces and kept as the kernel's ids. -EINVAL for an id of
 * -1 or one its user namespace does not map, -EPERM, -EFAULT. */
static int32_t get_cred(uintptr_t data, struct unix_out *out)
{
    const process_t *p = current_proc;
    struct unix_cred c;
    int64_t uid;
    int64_t gid;
    int locked;

    if (copy_from_user(&c, (const void __user *)data, sizeof(c)) != 0) {
        return -EFAULT;
    }
    locked = proc_pidns(p) != &init_pidns || proc_userns(p) != &init_userns;
    if (locked) {
        bkl_enter();
    }
    uid = uid_named(c.uid);
    gid = gid_named(c.gid);
    if (c.uid == 0xFFFFFFFFu || c.gid == 0xFFFFFFFFu || uid < 0 || gid < 0) {
        if (locked) {
            bkl_exit();
        }
        return -EINVAL;
    }
    c.pid = pid_named(c.pid);
    c.uid = (uint32_t)uid;
    c.gid = (uint32_t)gid;
    if (!proc_privileged(p) &&
        (c.pid != p->tgid || (c.uid != p->ruid && c.uid != p->euid && c.uid != p->suid) ||
         (c.gid != p->rgid && c.gid != p->egid && c.gid != p->sgid))) {
        if (locked) {
            bkl_exit();
        }
        return -EPERM;
    }
    if (locked) {
        bkl_exit();
    }
    out->cred = c;
    out->has_cred = 1;
    return 0;
}

/** Read the caller's control messages, in its layout (`cmsg_get`), into
 * `out`: -EINVAL for a malformed
 * header, a level other than SOL_SOCKET or another type, and the errors of
 * `get_rights` and `get_cred`. */
static int32_t get_control(uintptr_t control, uint32_t controllen, struct unix_out *out)
{
    uint32_t off = 0;

    while (off + cmsg_hdr() <= controllen) {
        struct kernel_cmsghdr hdr;
        uint32_t head = cmsg_hdr();
        int32_t rc;

        if (cmsg_get(&hdr, control + off) != 0) {
            return -EFAULT;
        }
        if (hdr.cmsg_len < head || hdr.cmsg_len > controllen - off ||
            hdr.cmsg_level != SOL_SOCKET) {
            return -EINVAL;
        }
        if (hdr.cmsg_type == SCM_RIGHTS) {
            rc = get_rights(control + off + head, (uint32_t)(hdr.cmsg_len - head) / 4u, out);
        } else if (hdr.cmsg_type == SCM_CREDENTIALS &&
                   hdr.cmsg_len == head + sizeof(struct unix_cred)) {
            rc = get_cred(control + off + head, out);
        } else {
            rc = -EINVAL;
        }
        if (rc != 0) {
            return rc;
        }
        off += cmsg_align((uint32_t)hdr.cmsg_len);
    }
    return 0;
}

/** Close the descriptors in flight `out` has and forget them. */
static void release_out(struct unix_out *out)
{
    fd_entry_t *fds = out->flight.fds;
    uint32_t flags;

    while (out->flight.nfds) {
        flight_drop(&fds[out->flight.nfds - 1], &out->flight.nfds);
    }
    flags = spin_lock_irqsave(&flight_lock);
    out->flight.fds = NULL;
    spin_unlock_irqrestore(&flight_lock, flags);
    kfree(fds);
}

/** A message of `len` bytes of the caller's buffers from `skip` bytes in,
 * with the sender's name and credentials, or NULL with -ENOBUFS or -EFAULT
 * in `*err`. */
static struct unix_msg *build(struct unix_sock *s, const struct kernel_iovec *iov, uint32_t iovlen,
                              uint32_t skip, uint32_t len, const struct unix_out *out, int32_t *err)
{
    struct unix_msg *m = msg_alloc(len);

    if (!m) {
        *err = -ENOBUFS;
        return NULL;
    }
    if (iov_copy(iov, iovlen, skip, m->data, len, 0) != 0) {
        flight_leave(&m->flight);
        kfree(m);
        *err = -EFAULT;
        return NULL;
    }
    if (out->has_cred) {
        m->cred = out->cred;
    } else {
        own_cred(&m->cred, 0);
    }
    {
        uint32_t flags = spin_lock_irqsave(&s->state_lock);

        m->from = s->name;
        spin_unlock_irqrestore(&s->state_lock, flags);
    }
    {
        int64_t sec, nsec;

        rtc_realtime(&sec, &nsec);
        m->tsec = sec;
        m->tnsec = (int32_t)nsec;
    }
    return m;
}

/** Move the descriptors in flight `out` has into `m`. */
static void attach(struct unix_msg *m, struct unix_out *out)
{
    uint32_t flags = spin_lock_irqsave(&flight_lock);

    m->flight.nfds = out->flight.nfds;
    m->flight.fds = out->flight.fds;
    out->flight.nfds = 0;
    out->flight.fds = NULL;
    spin_unlock_irqrestore(&flight_lock, flags);
}

/** Raise SIGPIPE in the calling thread, as Linux sends it, unless `flags`
 * has MSG_NOSIGNAL. */
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
    bkl_enter();
    signal_post_thread(current_proc, SIGPIPE, &info);
    bkl_exit();
}

/** The wait condition of a send on the connected socket `arg`: its peer
 * has room, or the send ends at once. */
static int peer_has_room(void *arg)
{
    struct unix_sock *s = (struct unix_sock *)arg, *p = peer_get(s);
    uint32_t flags = state_pair(s, p);
    int ready = !p || p->closed || (s->shut & SHUT_SEND) || s->err;

    if (p) {
        spin_lock(&p->queue_lock);
        ready |= (p->shut & SHUT_RCV) || !queue_full(p);
        spin_unlock(&p->queue_lock);
    }
    state_unpair(s, p, flags);
    if (p) {
        unix_put(p);
    }
    return ready;
}

/** Whether a send on the connected socket `s` to its peer `p` must end with
 * EPIPE: the connection is gone, or shut down in the send's direction. */
static int send_stopped(const struct unix_sock *s, const struct unix_sock *p)
{
    return !p || p->closed || (s->shut & SHUT_SEND) || (p->shut & SHUT_RCV);
}

/** Choose a connected send's next piece under both state locks and the
 * peer's queue lock. A packet stays whole; a stream takes the room left. */
static uint32_t send_chunk(const struct unix_sock *s, const struct unix_sock *p, uint32_t remaining)
{
    uint32_t chunk = remaining;

    if (s->type == SOCK_STREAM) {
        if (chunk > p->rcvbuf - p->rq_charge) {
            chunk = p->rcvbuf - p->rq_charge;
        }
        if (chunk > UNIX_CHUNK) {
            chunk = UNIX_CHUNK;
        }
    }
    return chunk;
}

/**
 * A send of `total` bytes of `iov` on a connected stream or
 * sequenced-packet socket, with what `out` has, which the first piece
 * queued takes; the bytes queued, or `unix_sendto`'s errors for its kind.
 * A stream's bytes go in pieces as the peer's queue has room; a packet goes
 * whole. Copying a piece from the caller can sleep, and the connection is
 * judged again after it, so a piece is not queued past an error or a
 * shutdown that came meanwhile.
 */
static int32_t send_conn(struct unix_sock *s, int nonblock, const struct kernel_iovec *iov,
                         uint32_t iovlen, uint32_t total, struct unix_out *out, int flags)
{
    struct unix_sock *p;
    struct unix_msg *m = NULL;
    uint32_t done = 0, saved, chunk = 0;
    uint64_t timeo;
    int32_t rc = 0;

    saved = spin_lock_irqsave(&s->state_lock);
    if (s->state != US_CONNECTED) {
        rc = -ENOTCONN;
    } else if (s->type == SOCK_SEQPACKET && total > s->sndbuf - 32u) {
        rc = -EMSGSIZE;
    }
    spin_unlock_irqrestore(&s->state_lock, saved);
    if (rc || (s->type == SOCK_STREAM && total == 0)) {
        return rc;
    }
    p = peer_get(s);
    for (;;) {
        int full = 0, queued = 0;

        saved = state_pair(s, p);
        timeo = s->sndtimeo;
        if (s->err) {
            rc = -s->err;
            s->err = 0;
        } else if (send_stopped(s, p)) {
            rc = -EPIPE;
        }
        if (!rc) {
            spin_lock(&p->queue_lock);
            full = queue_full(p);
            if (!full && m) {
                attach(m, out);
                enqueue(p, m);
                done += m->len;
                m = NULL;
                queued = 1;
            } else if (!full) {
                chunk = send_chunk(s, p, total - done);
            }
            spin_unlock(&p->queue_lock);
        }
        state_unpair(s, p, saved);
        if (rc) {
            break;
        }
        if (full) {
            if (nonblock) {
                rc = -EAGAIN;
                break;
            }
            rc = unix_wait(&p->wq, peer_has_room, s, timeo);
            if (rc) {
                break;
            }
            continue;
        }
        if (queued) {
            wq_wake(&p->wq, WQ_DATA);
            if (done >= total) {
                break;
            }
            continue;
        }
        m = build(s, iov, iovlen, done, chunk, out, &rc);
        if (!m) {
            break;
        }
    }
    if (m) {
        msg_free(m);
    }
    if (p) {
        unix_put(p);
    }
    if (rc == -EPIPE && done == 0) {
        broken_pipe(flags);
    }
    return done ? (int32_t)done : rc;
}

/** Whether a datagram send waiting for room in the queue of `t` can stop:
 * there is room, or `t` is gone or takes nothing more, when the send fails. */
static int target_has_room(void *arg)
{
    struct unix_sock *t = (struct unix_sock *)arg;
    uint32_t flags = spin_lock_irqsave(&t->state_lock);
    int ready;

    spin_lock(&t->queue_lock);
    ready = t->closed || (t->shut & SHUT_RCV) || !queue_full(t);
    spin_unlock(&t->queue_lock);
    spin_unlock_irqrestore(&t->state_lock, flags);
    return ready;
}

/** Judge a datagram send under both state locks. A closed connected peer
 * is detached, with its reference left for the caller to drop after unlock. */
static int32_t dgram_refusal(struct unix_sock *s, struct unix_sock *t, const struct unix_name *to,
                             int *drop_peer)
{
    if (s->shut & SHUT_SEND) {
        return -EPIPE;
    }
    if (t->closed) {
        if (!to && s->peer == t) {
            s->peer = NULL;
            *drop_peer = 1;
        }
        return -ECONNREFUSED;
    }
    if (t->peer && t->peer != s) {
        return -EPERM;
    }
    return t->shut & SHUT_RCV ? -EPIPE : 0;
}

/** A send of one message of `total` bytes of `iov` on a datagram socket, to
 * `to` or to the connected peer, with what `out` has; `total`, or
 * `unix_sendto`'s errors for its kind. */
static int32_t send_dgram(struct unix_sock *s, int nonblock, const struct kernel_iovec *iov,
                          uint32_t iovlen, uint32_t total, const struct unix_name *to,
                          struct unix_out *out)
{
    struct unix_sock *t;
    struct unix_msg *m;
    uint32_t flags;
    uint64_t timeo;
    int32_t rc = 0;
    int drop_peer = 0;

    flags = spin_lock_irqsave(&s->state_lock);
    if (total > s->sndbuf - 32u) {
        rc = -EMSGSIZE;
    } else if (s->shut & SHUT_SEND) {
        rc = -EPIPE;
    }
    spin_unlock_irqrestore(&s->state_lock, flags);
    if (rc) {
        return rc;
    }
    rc = passcred_name(s);
    if (rc) {
        return rc;
    }
    t = to ? lookup(to, SOCK_DGRAM, &rc) : peer_get(s);
    if (!t) {
        return to ? rc : -ENOTCONN;
    }
    if (t->type != SOCK_DGRAM) {
        unix_put(t);
        return -EPROTOTYPE;
    }
    m = build(s, iov, iovlen, 0, total, out, &rc);
    if (!m) {
        unix_put(t);
        return rc;
    }
    for (;;) {
        flags = state_pair(s, t);
        timeo = s->sndtimeo;
        rc = dgram_refusal(s, t, to, &drop_peer);
        if (!rc) {
            spin_lock(&t->queue_lock);
            if (!queue_full(t)) {
                attach(m, out);
                enqueue(t, m);
                m = NULL;
            }
            spin_unlock(&t->queue_lock);
        }
        state_unpair(s, t, flags);
        if (rc || !m) {
            break;
        }
        if (nonblock) {
            rc = -EAGAIN;
            break;
        }
        rc = unix_wait(&t->wq, target_has_room, t, timeo);
        if (rc) {
            break;
        }
    }
    if (m) {
        msg_free(m);
    } else {
        wq_wake(&t->wq, WQ_DATA);
    }
    if (drop_peer) {
        unix_put(t);
    }
    unix_put(t);
    return rc ? rc : (int32_t)total;
}

/** A send on `e` of `total` bytes of `iov`, to `to` or to the peer, with
 * what `out` has, which it releases whatever the result (`unix_sendto`). */
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
        uint32_t saved = spin_lock_irqsave(&s->state_lock);

        rc = s->state == US_CONNECTED ? -EISCONN : -EOPNOTSUPP;
        spin_unlock_irqrestore(&s->state_lock, saved);
    } else {
        rc = send_conn(s, nonblock, iov, iovlen, total, out, flags);
    }
    release_out(out);
    return rc;
}

/** The wait condition of `wait_data`. */
static int has_data(void *arg)
{
    struct unix_sock *s = (struct unix_sock *)arg;
    uint32_t flags = spin_lock_irqsave(&s->state_lock);
    int ready;

    spin_lock(&s->queue_lock);
    ready = s->rq_head != NULL || s->err || (s->shut & SHUT_RCV);
    spin_unlock(&s->queue_lock);
    spin_unlock_irqrestore(&s->state_lock, flags);
    return ready;
}

/** Wait for a message: 0 with one at the head of the queue, 1 at the end of
 * input, or the socket's pending error, -EAGAIN or -EINTR. */
static int32_t wait_data(struct unix_sock *s, int nonblock)
{
    int32_t rc;
    uint32_t flags;
    uint64_t timeo;

    for (;;) {
        flags = spin_lock_irqsave(&s->state_lock);
        spin_lock(&s->queue_lock);
        if (s->rq_head) {
            rc = 0;
        } else if (s->err) {
            rc = -s->err;
            s->err = 0;
        } else if (s->shut & SHUT_RCV) {
            rc = 1;
        } else {
            rc = -EAGAIN;
        }
        timeo = s->rcvtimeo;
        spin_unlock(&s->queue_lock);
        spin_unlock_irqrestore(&s->state_lock, flags);
        if (rc != -EAGAIN || nonblock) {
            return rc;
        }
        rc = unix_wait(&s->wq, has_data, s, timeo);
        if (rc) {
            return rc;
        }
    }
}

/** Move a message's descriptors into `in`, or with `peek` copies of them;
 * whether it had any. */
static int take_fds(struct unix_msg *m, struct unix_in *in, int peek)
{
    uint32_t i, flags;
    fd_entry_t *fds;

    if (m->flight.nfds == 0) {
        return 0;
    }
    if (!peek) {
        flags = spin_lock_irqsave(&flight_lock);
        in->flight.fds = m->flight.fds;
        in->flight.nfds = m->flight.nfds;
        m->flight.fds = NULL;
        m->flight.nfds = 0;
        spin_unlock_irqrestore(&flight_lock, flags);
        return 1;
    }
    fds = kmalloc(sizeof(fd_entry_t) * m->flight.nfds);
    if (fds) {
        for (i = 0; i < m->flight.nfds; i++) {
            fds[i] = m->flight.fds[i];
            flight_hold(&fds[i]);
        }
        flags = spin_lock_irqsave(&flight_lock);
        in->flight.fds = fds;
        in->flight.nfds = m->flight.nfds;
        spin_unlock_irqrestore(&flight_lock, flags);
    }
    return 1;
}

/** Whether `a` and `b` are the same credentials. */
static int same_cred(const struct unix_cred *a, const struct unix_cred *b)
{
    return a->pid == b->pid && a->uid == b->uid && a->gid == b->gid;
}

/** The message a stream read continues from after `copied` bytes, and the
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

/** Consume `chunk` bytes of the stream message `m` at the head of `s`'s
 * queue, and the message once they are all read. */
static void stream_consume(struct unix_sock *s, struct unix_msg *m, uint32_t chunk)
{
    struct unix_msg *dead = NULL;
    uint32_t flags = spin_lock_irqsave(&s->queue_lock);

    m->off += chunk;
    m->charge -= chunk;
    s->rq_bytes -= chunk;
    s->rq_charge -= chunk;
    if (m->off == m->len) {
        dead = dequeue(s);
    }
    spin_unlock_irqrestore(&s->queue_lock, flags);
    if (dead) {
        msg_free(dead);
    }
}

/**
 * A receive of up to `total` bytes into `iov` on a stream socket: bytes
 * across messages, up to and including one that has descriptors, and not
 * past a change of credentials when they are asked for; the bytes read, or
 * `unix_recvfrom`'s errors before any. The receive lock is held throughout
 * but for the wait for data.
 */
static int32_t recv_stream(struct unix_sock *s, int nonblock, const struct kernel_iovec *iov,
                           uint32_t iovlen, uint32_t total, int flags, struct unix_in *in)
{
    uint32_t copied = 0, saved;
    uint32_t want = (flags & MSG_WAITALL) ? total : 1u;
    int peek = (flags & MSG_PEEK) != 0;
    int32_t rc = 0;

    saved = spin_lock_irqsave(&s->state_lock);
    rc = s->state == US_CONNECTED ? 0 : -EINVAL;
    spin_unlock_irqrestore(&s->state_lock, saved);
    if (rc || total == 0) {
        return rc;
    }
    recv_lock(s);
    for (;;) {
        uint32_t off, chunk;
        struct unix_msg *m;
        int had_fds, passcred;

        saved = spin_lock_irqsave(&s->state_lock);
        passcred = s->passcred;
        spin_lock(&s->queue_lock);
        m = stream_at(s, copied, peek, &off);
        spin_unlock(&s->queue_lock);
        spin_unlock_irqrestore(&s->state_lock, saved);
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
        if (passcred && copied > 0 && !same_cred(&in->cred, &m->cred)) {
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
            in->has_cred = passcred;
        }
        copied += chunk;
        had_fds = take_fds(m, in, peek);
        if (!peek) {
            stream_consume(s, m, chunk);
            wake_writers(s);
        }
        if (had_fds || copied >= total) {
            break;
        }
    }
    recv_unlock(s);
    if (!peek) {
        wake_writers(s);
    }
    return copied ? (int32_t)copied : rc;
}

/** A receive of one message into `iov` on a datagram or sequenced-packet
 * socket, which the receive lock keeps from its choice to its consumption;
 * its length as `unix_recvfrom` gives it, or its errors. A copy that faults
 * is -EFAULT and consumes the message, closing its descriptors, unless
 * MSG_PEEK, as Linux's __unix_dgram_recvmsg has dequeued the skb before its
 * copy and frees it (net/unix/af_unix.c:2459-2460, 2501-2502 and
 * 2541-2542 in 6.12). */
static int32_t recv_msg(struct unix_sock *s, int nonblock, const struct kernel_iovec *iov,
                        uint32_t iovlen, uint32_t total, int flags, struct unix_in *in)
{
    int peek = (flags & MSG_PEEK) != 0;
    struct unix_msg *m;
    uint32_t chunk, saved;
    int32_t rc;

    saved = spin_lock_irqsave(&s->state_lock);
    rc = s->type == SOCK_SEQPACKET && s->state != US_CONNECTED ? -ENOTCONN : 0;
    spin_unlock_irqrestore(&s->state_lock, saved);
    if (rc) {
        return rc;
    }
    for (;;) {
        rc = wait_data(s, nonblock);
        if (rc) {
            return rc == 1 ? 0 : rc;
        }
        recv_lock(s);
        saved = spin_lock_irqsave(&s->queue_lock);
        m = s->rq_head;
        if (m && !peek) {
            dequeue(s);
        }
        spin_unlock_irqrestore(&s->queue_lock, saved);
        if (m) {
            break;
        }
        recv_unlock(s);
    }
    chunk = m->len < total ? m->len : total;
    rc = iov_copy(iov, iovlen, 0, m->data, chunk, 1);
    if (!rc) {
        if (m->len > total) {
            in->flags |= MSG_TRUNC;
        }
        in->from = m->from;
        in->cred = m->cred;
        saved = spin_lock_irqsave(&s->state_lock);
        in->has_cred = s->passcred;
        in->tstamp = s->tstamp;
        spin_unlock_irqrestore(&s->state_lock, saved);
        in->tsec = m->tsec;
        in->tnsec = m->tnsec;
        take_fds(m, in, peek);
        rc = (flags & MSG_TRUNC) ? (int32_t)m->len : (int32_t)chunk;
    }
    recv_unlock(s);
    if (!peek) {
        msg_free(m);
        wake_writers(s);
    }
    return rc;
}

/** A receive on `e` into `iov`, with the sender's name and credentials and
 * the message's descriptors in `in` (`unix_recvfrom`). */
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

/** Close the descriptors `in` received and forget them. */
static void release_in(struct unix_in *in)
{
    fd_entry_t *fds = in->flight.fds;
    uint32_t flags;

    while (in->flight.nfds) {
        flight_drop(&fds[in->flight.nfds - 1], &in->flight.nfds);
    }
    flags = spin_lock_irqsave(&flight_lock);
    in->flight.fds = NULL;
    spin_unlock_irqrestore(&flight_lock, flags);
    kfree(fds);
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

    while (given < in->flight.nfds && given < room) {
        int fd = fd_alloc(table());
        fd_entry_t *slot, copy;
        struct vfs_mount *locked;
        uint32_t saved;
        int big;

        if (fd < 0) {
            break;
        }
        if (copy_to_user((void __user *)(at + cmsg_hdr() + 4u * given), &fd, sizeof(fd)) != 0) {
            fd_free(table(), fd);
            break;
        }
        big = file_ops_lock(&in->flight.fds[given]);
        locked = fd_release_lock(&in->flight.fds[given]);
        slot = fd_slot(table(), fd);
        fd_ofd_unref(slot->ofd);
        saved = spin_lock_irqsave(&flight_lock);
        copy = in->flight.fds[given];
        copy.pending = 1;
        *slot = copy;
        in->flight.fds[given++].type = FD_TYPE_NONE;
        flight_sync(slot);
        if (flags & MSG_CMSG_CLOEXEC) {
            slot->flags |= FD_FLAG_CLOEXEC;
        }
        spin_unlock_irqrestore(&flight_lock, saved);
        fd_install(table(), fd);
        if (locked) {
            vfs_ns_unlock(locked);
        }
        file_ops_unlock(big);
    }
    if (given < in->flight.nfds) {
        in->flags |= MSG_CTRUNC;
    }
    while (in->flight.nfds > given) {
        flight_drop(&in->flight.fds[in->flight.nfds - 1], &in->flight.nfds);
    }
    {
        fd_entry_t *fds;
        uint32_t saved = spin_lock_irqsave(&flight_lock);

        fds = in->flight.fds;
        in->flight.fds = NULL;
        in->flight.nfds = 0;
        spin_unlock_irqrestore(&flight_lock, saved);
        kfree(fds);
    }
    return given;
}

/**
 * Write the control messages of a receive to the caller's buffer, in its
 * layout (`cmsg_put`), in Linux's order: the timestamp the socket asked for
 * (`sock_tstamp_put`), the credentials, and the descriptors, each installed
 * as a new descriptor.
 * Returns the bytes of the buffer used, a message's padding to the next
 * alignment counted only as far as the buffer goes, as Linux's `put_cmsg`
 * counts it, so that the length given back covers no byte past the buffer.
 */
static uint32_t put_control(uintptr_t control, uint32_t controllen, struct unix_in *in, int flags)
{
    uint32_t head = cmsg_hdr();
    uint32_t used = 0;
    struct kernel_cmsghdr hdr;
    uint32_t given;
    struct unix_cred seen;

    k_memset(&hdr, 0, sizeof(hdr));
    if (in->tstamp & SOCK_RX_TSTAMP) {
        uint8_t ts[16];
        uint32_t n = sock_tstamp_put(in->tstamp, in->tsec, in->tnsec, &hdr.cmsg_type, ts);

        hdr.cmsg_len = head + n;
        hdr.cmsg_level = SOL_SOCKET;
        if (!control || controllen < hdr.cmsg_len || cmsg_put(control, &hdr) != 0 ||
            copy_to_user((void __user *)(control + head), ts, n) != 0) {
            in->flags |= MSG_CTRUNC;
        } else {
            used = cmsg_align((uint32_t)hdr.cmsg_len);
            used = used > controllen ? controllen : used;
        }
    }
    if (in->has_cred) {
        seen = cred_seen(in->cred);
        hdr.cmsg_len = head + sizeof(seen);
        hdr.cmsg_level = SOL_SOCKET;
        hdr.cmsg_type = SCM_CREDENTIALS;
        if (!control || controllen - used < hdr.cmsg_len || cmsg_put(control + used, &hdr) != 0 ||
            copy_to_user((void __user *)(control + used + head), &seen, sizeof(seen)) != 0) {
            in->flags |= MSG_CTRUNC;
        } else {
            used += cmsg_align((uint32_t)hdr.cmsg_len);
            used = used > controllen ? controllen : used;
        }
    }
    if (in->flight.nfds == 0) {
        return used;
    }
    given = put_rights(
        control + used,
        (control && controllen > used + head) ? (uint32_t)(controllen - used - head) / 4u : 0u, in,
        flags);
    if (given > 0) {
        hdr.cmsg_len = head + 4u * given;
        hdr.cmsg_level = SOL_SOCKET;
        hdr.cmsg_type = SCM_RIGHTS;
        if (cmsg_put(control + used, &hdr) == 0) {
            used += cmsg_align((uint32_t)hdr.cmsg_len);
            used = used > controllen ? controllen : used;
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
    flight_enter(&out.flight);
    rc = send_core(e, &iov, 1, len, addr ? &to : NULL, &out, flags);
    flight_leave(&out.flight);
    return rc;
}

/** An address with no length pointer is EFAULT before anything is taken,
 * so the message stays queued; Linux fails the call only after it has
 * taken the message. */
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

int32_t unix_readv(fd_entry_t *e, const struct kernel_iovec *iov, int count, uint32_t total)
{
    struct unix_in in;
    int32_t rc;
    uint32_t saved = kirq_save();
    int depth = bkl_drop();

    kirq_restore(saved);

    k_memset(&in, 0, sizeof(in));
    transit_enter(&in);
    rc = recv_core(e, iov, (uint32_t)count, total, 0, &in);
    release_in(&in);
    transit_leave(&in);
    saved = kirq_save();
    bkl_take(depth);
    kirq_restore(saved);
    return rc;
}

/** Send the imported vector as one socket write, preserving a datagram's
 * boundary and committing each stream piece only after every element of
 * that piece has been copied, as Linux's sock_write_iter (net/socket.c:
 * 1132-1149 in 6.12). The descriptor is held; no big lock is held on return
 * unless the caller held it on entry. */
int32_t unix_writev(fd_entry_t *e, const struct kernel_iovec *iov, int count, uint32_t total)
{
    struct unix_out out;
    int32_t rc;
    uint32_t saved = kirq_save();
    int depth = bkl_drop();

    kirq_restore(saved);
    k_memset(&out, 0, sizeof(out));
    flight_enter(&out.flight);
    rc = send_core(e, iov, (uint32_t)count, total, NULL, &out, 0);
    flight_leave(&out.flight);
    saved = kirq_save();
    bkl_take(depth);
    kirq_restore(saved);
    return rc;
}

/** The caller's iovec array in `*iov`, copied in in its layout and checked
 * (`iov_import`), with its total length, cut at MAX_RW_COUNT, in `*total`:
 * 0, or -EMSGSIZE past IOV_MAX_LOCAL vectors, -EINVAL for a negative
 * length, -ENOMEM, -EFAULT. The caller frees the array, whatever the
 * result. */
static int32_t get_iov(uintptr_t uiov, unsigned long iovlen, struct kernel_iovec **iov,
                       uint32_t *total)
{
    long rc;

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
    rc = iov_import(*iov, uiov, (int)iovlen, -1);
    if (rc < 0) {
        return (int32_t)rc;
    }
    *total = (uint32_t)rc;
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

    if (msghdr_get(&hdr, msg) != 0) {
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
    flight_enter(&out.flight);
    rc = get_iov(hdr.msg_iov, hdr.msg_iovlen, &iov, &total);
    if (rc == 0 && hdr.msg_control && hdr.msg_controllen) {
        rc = get_control(hdr.msg_control, USER_LEN32(hdr.msg_controllen), &out);
    }
    if (rc == 0) {
        rc = send_core(e, iov, (uint32_t)hdr.msg_iovlen, total, named ? &to : NULL, &out, flags);
    } else {
        release_out(&out);
    }
    flight_leave(&out.flight);
    kfree(iov);
    return rc;
}

/** The header and the control buffer are checked writable before the
 * receive, so that a bad one fails before a message is taken. */
int32_t unix_recvmsg(fd_entry_t *e, uintptr_t msg, int flags)
{
    struct kernel_msghdr hdr;
    struct kernel_iovec *iov;
    struct unix_in in;
    uint32_t total;
    int32_t rc;

    if (msghdr_get(&hdr, msg) != 0) {
        return -EFAULT;
    }
    if (!user_buffer_ok(msg, msghdr_size(), 1) ||
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
    if (msghdr_put(msg, &hdr) != 0) {
        return -EFAULT;
    }
    return rc;
}

/** read(2) of a socket: a receive with no flags (`unix_recvfrom`). */
static int32_t unix_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    uint32_t saved = kirq_save();
    int depth = bkl_drop();
    int32_t rc;

    kirq_restore(saved);
    rc = unix_recvfrom(e, (__force uintptr_t)buf, count, 0, 0, 0);
    saved = kirq_save();
    bkl_take(depth);
    kirq_restore(saved);
    return rc;
}

/** write(2) of a socket: a send with no flags (`unix_sendto`). */
static int32_t unix_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    uint32_t saved = kirq_save();
    int depth = bkl_drop();
    int32_t rc;

    kirq_restore(saved);
    rc = unix_sendto(e, (__force uintptr_t)buf, count, 0, 0, 0);
    saved = kirq_save();
    bkl_take(depth);
    kirq_restore(saved);
    return rc;
}

/** Whether a send would not wait: a connected socket's peer has room in its
 * queue, or the send fails at once, the peer being gone or the connection
 * shut down in the send's direction, which the send then reports. A
 * datagram socket whose sends each give the receiver's name cannot tell
 * and says yes, as Linux does; so does a stream socket that is not
 * connected, whose send fails at once. A listener is never writable. */
static int writable(struct unix_sock *s)
{
    struct unix_sock *p = s->peer;

    if (s->state == US_LISTENING) {
        return 0;
    }
    if (!p || *(volatile int *)&p->closed || (s->shut & SHUT_SEND) ||
        (*(volatile int *)&p->shut & SHUT_RCV)) {
        return 1;
    }
    return !queue_full(p);
}

/**
 * The poll of a socket, on its queue `wq`: POLLERR with a pending error;
 * POLLHUP when it is shut down both ways, or for a stream or
 * sequenced-packet socket never connected; POLLIN with POLLRDHUP once it is
 * shut down for reading, and POLLIN with a message or a connection queued;
 * POLLOUT while a send would not wait (`writable`). POLLERR and POLLHUP are
 * reported whatever `events` asks. The state lock pins the peer until the
 * snapshot ends; no reference is released here, since epoll polls under a
 * spinlock and the last reference can close descriptors and sleep.
 */
static short unix_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct unix_sock *s = sock_of(e), *p;
    short mask = 0;
    uint32_t flags = spin_lock_irqsave(&s->state_lock);

    if (wq) {
        *wq = &s->wq;
    }
    p = s->peer;
    if (s->err) {
        mask |= POLLERR;
    }
    if (s->shut == SHUT_BOTH || (s->type != SOCK_DGRAM && s->state == US_UNCONNECTED)) {
        mask |= POLLHUP;
    }
    if (s->shut & SHUT_RCV) {
        mask |= POLLRDHUP | POLLIN | POLLRDNORM;
    }
    spin_lock(&s->queue_lock);
    if (s->rq_head || s->acc_head) {
        mask |= POLLIN | POLLRDNORM;
    }
    spin_unlock(&s->queue_lock);
    if (p) {
        spin_lock(&p->queue_lock);
    }
    if (writable(s)) {
        mask |= POLLOUT | POLLWRNORM;
    }
    if (p) {
        spin_unlock(&p->queue_lock);
    }
    spin_unlock_irqrestore(&s->state_lock, flags);
    return (short)((mask & events) | (mask & (POLLERR | POLLHUP)));
}

/** The ioctls of a socket, an int stored at `arg`: FIONREAD gives the bytes
 * queued to a stream socket, the length of the next message to another, or
 * -EINVAL for a listener; TIOCOUTQ the bytes waiting in the peer's queue of
 * a connected stream or sequenced-packet socket, else 0. -EFAULT, or
 * -ENOTTY for another request. */
static int32_t unix_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct unix_sock *s = sock_of(e), *p;
    int32_t n;
    uint32_t flags;

    switch (request) {
    case FIONREAD:
        flags = spin_lock_irqsave(&s->state_lock);
        if (s->state == US_LISTENING) {
            spin_unlock_irqrestore(&s->state_lock, flags);
            return -EINVAL;
        }
        spin_lock(&s->queue_lock);
        n = s->type == SOCK_STREAM ? (int32_t)s->rq_bytes
            : s->rq_head           ? (int32_t)s->rq_head->len
                                   : 0;
        spin_unlock(&s->queue_lock);
        spin_unlock_irqrestore(&s->state_lock, flags);
        break;
    case TIOCOUTQ:
        p = s->type != SOCK_DGRAM ? peer_get(s) : NULL;
        n = 0;
        if (p) {
            flags = spin_lock_irqsave(&p->queue_lock);
            n = (int32_t)p->rq_bytes;
            spin_unlock_irqrestore(&p->queue_lock, flags);
            unix_put(p);
        }
        break;
    default:
        return -ENOTTY;
    }
    if (copy_to_user((void __user *)arg, &n, sizeof(n)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/** The last close of a socket's description: the end of its descriptors'
 * side (`unix_release`). */
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
    uint32_t flags = spin_lock_irqsave(&table_lock);

    for (s = socks; s; s = s->next) {
        if (s->ns == ns) {
            s->ns = &gone;
        }
    }
    spin_unlock_irqrestore(&table_lock, flags);
}
