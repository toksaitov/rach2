/*
 * pty.c: pseudo-terminals. The /dev/ptmx + /dev/pts/N master/slave pairs a
 * network login uses to hand a remote user a real terminal. See pty.h for the
 * data-flow model. The slave is a full struct tty (the N_TTY discipline plus
 * sessions and job control), so it reuses every terminal service; the master is
 * a byte ring (m_buf) carrying the slave's output and the discipline's echo.
 * Each endpoint rides the file_ops layer.
 */

#include "pty.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../proc/process.h"
#include "../mm/usercopy.h"
#include "../mm/kheap.h"
#include "../io/poll.h"
#include "../lib/kstring.h"
#include "../lib/kerrno.h"
#include "../lib/ksignal.h"
#include "../lib/kirq.h"

/* Linux ioctls. The pty-number requests are pinned exactly (musl uses
 * these). */
#define FIONREAD   0x541B
#define FIONBIO    0x5421
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGPTN   0x80045430 /* master: get the slave index N (arg int*) */
#define TIOCSPTLCK 0x40045431 /* master: set/clear the slave lock (arg int*, 0 = unlock) */

/* O_* flags honored on open (mirror syscall.c / fd.h). */
#define O_CLOEXEC_FLAG  02000000
#define O_NONBLOCK_FLAG 04000

/* The live ptys, each allocated from the heap at its master's open and
 * freed when both ends are gone. The index is the lowest one no live pty
 * has, so `/dev/pts` names stay small and are reused as Unix98 reuses them. */
static struct pty *ptys;

void pty_init_pool(void)
{
    ptys = NULL;
}

static struct pty *pty_by_index(int index)
{
    struct pty *p;

    for (p = ptys; p; p = p->next) {
        if (p->index == index) {
            return p;
        }
    }
    return NULL;
}

/* Off the list and back to the heap; nothing refers to it after this. */
static void pty_free(struct pty *p)
{
    struct pty **link = &ptys;

    while (*link && *link != p) {
        link = &(*link)->next;
    }
    if (*link) {
        *link = p->next;
    }
    kfree(p);
}

static uint32_t pty_m_avail(struct pty *p)
{
    return p->m_tail - p->m_head;
}

/**
 * Push one byte from the slave's output into the master ring.
 *
 * Drops the byte if the master is not draining fast enough; a real pty also
 * bounds this. The caller wakes master readers once per write op (ptm_write
 * echo, pts_write output), not per byte. Runs in thread context, since no IRQ
 * feeds a pty.
 */
static void pty_slave_sink(void *ctx, unsigned char c)
{
    struct pty *p = (struct pty *)ctx;
    uint32_t flags;

    flags = kirq_save();
    if (p->m_tail - p->m_head < PTY_BUF_SIZE) {
        p->m_buf[p->m_tail & (PTY_BUF_SIZE - 1)] = c;
        p->m_tail++;
    }
    kirq_restore(flags);
}

/** Initialize a pair with one queue for master readers, writers and pollers. */
static struct pty *pty_alloc(void)
{
    struct pty *p = kzalloc(sizeof(*p));
    int i;

    if (!p) {
        return NULL;
    }
    for (i = 0; pty_by_index(i); i++) {
    }
    p->in_use = 1;
    p->index = i;
    p->locked = 1; /* unlockpt() (TIOCSPTLCK 0) must run before pts open */
    p->uid = current_proc ? current_proc->euid : 0;
    p->gid = current_proc ? current_proc->egid : 0;
    p->mode = 0620;                         /* crw--w----, the Unix98 slave default */
    tty_init(&p->slave, pty_slave_sink, p); /* slave.pump stays NULL */
    wait_queue_init(&p->m_read_wq);
    p->slave.space_wq = &p->m_read_wq;
    p->next = ptys;
    ptys = p;
    return p;
}

static int fd_is_nonblock(fd_entry_t *e)
{
    return e->ofd && (e->ofd->status_flags & OFD_NONBLOCK);
}

/* the MASTER endpoint */

/* A master reader can stop blocking: the slave produced output, or every slave
 * descriptor has closed (EOF). */
static int ptm_readable(void *arg)
{
    struct pty *p = (struct pty *)arg;
    return pty_m_avail(p) != 0 || p->slave_opens == 0;
}

static int32_t ptm_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct pty *p = (struct pty *)e->priv;
    uint8_t kbuf[512];
    uint32_t got = 0;
    uint32_t to_read;

    if (count == 0) {
        return 0;
    }
    if (!user_range_ok((uint32_t)(__force uintptr_t)buf, count)) {
        return -EFAULT;
    }
    while (pty_m_avail(p) == 0) {
        if (p->slave_opens == 0) {
            return 0; /* EOF: every slave has closed and the ring is drained */
        }
        if (fd_is_nonblock(e)) {
            return -EAGAIN;
        }
        if (wq_wait_event(&p->m_read_wq, ptm_readable, p) < 0) {
            return -EINTR;
        }
    }
    to_read = pty_m_avail(p);
    if (to_read > count) {
        to_read = count;
    }
    while (got < to_read) {
        uint32_t chunk = to_read - got;
        uint32_t i;
        uint32_t flags;

        if (chunk > sizeof(kbuf)) {
            chunk = sizeof(kbuf);
        }
        /* Drain the ring under interrupts off (vs a concurrent slave producer),
         * then copy out to user memory (which may fault) outside the lock. */
        flags = kirq_save();
        for (i = 0; i < chunk; i++) {
            kbuf[i] = p->m_buf[p->m_head & (PTY_BUF_SIZE - 1)];
            p->m_head++;
        }
        kirq_restore(flags);
        if (copy_to_user((void __user *)(buf + got), kbuf, chunk) != 0) {
            return (got > 0) ? (int32_t)got : -EFAULT;
        }
        got += chunk;
    }
    return (int32_t)got;
}

/* Whether the slave's discipline can take a line: the master's write wait. */
static int ptm_has_room(void *arg)
{
    struct pty *p = (struct pty *)arg;

    return n_tty_has_room(&p->slave.ldisc);
}

/* The offset of the first signal character in `bytes`, or -1: VINTR, VQUIT
 * or VSUSP with ISIG on, a disabled (zero) one never matching. */
static int signal_char_at(const struct n_tty *ld, const uint8_t *bytes, int n)
{
    int i;

    if (!(ld->tio.c_lflag & ISIG)) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        unsigned char c = bytes[i];

        if ((ld->tio.c_cc[VINTR] && c == ld->tio.c_cc[VINTR]) ||
            (ld->tio.c_cc[VQUIT] && c == ld->tio.c_cc[VQUIT]) ||
            (ld->tio.c_cc[VSUSP] && c == ld->tio.c_cc[VSUSP])) {
            return i;
        }
    }
    return -1;
}

/**
 * Feed slave input and wake master readers for echo after partial writes too.
 */
static int32_t ptm_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct pty *p = (struct pty *)e->priv;
    uint8_t kbuf[256];
    uint32_t done = 0;

    if (count == 0) {
        return 0;
    }
    if (!user_range_ok((uint32_t)(__force uintptr_t)buf, count)) {
        return -EFAULT;
    }
    /* Feed each byte to the slave's line discipline: it cooks the input, echoes
     * (the echo lands in m_buf via the slave sink), and wakes a blocked slave
     * reader once a line is ready (tty_input_char). */
    while (done < count) {
        uint32_t chunk = count - done;
        uint32_t i;

        if (chunk > sizeof(kbuf)) {
            chunk = sizeof(kbuf);
        }
        if (copy_from_user(kbuf, (const void __user *)(buf + done), chunk) != 0) {
            wq_wake_all(&p->m_read_wq);
            return (done > 0) ? (int32_t)done : -EFAULT;
        }
        for (i = 0; i < chunk; i++) {
            /* The discipline takes a whole line more or nothing. A write
             * that would overflow it is short; one that fits nothing waits
             * for the slave to read, or asks a non-blocking writer to. A
             * signal character in the held bytes is fed over them, since
             * what it does is discard what came before it. */
            while (!n_tty_has_room(&p->slave.ldisc)) {
                int at = signal_char_at(&p->slave.ldisc, kbuf + i, (int)(chunk - i));

                if (at >= 0) {
                    p->slave.ldisc.counters.flush_bytes += (unsigned int)at;
                    i += (uint32_t)at;
                    tty_input_char(&p->slave, kbuf[i]);
                    i++;
                    continue;
                }
                wq_wake_all(&p->m_read_wq);
                if (done + i > 0 || (e->ofd && (e->ofd->status_flags & OFD_NONBLOCK))) {
                    return done + i > 0 ? (int32_t)(done + i) : -EAGAIN;
                }
                if (wq_wait_event(&p->m_read_wq, ptm_has_room, p) < 0) {
                    return -EINTR;
                }
            }
            if (i >= chunk) {
                break;
            }
            tty_input_char(&p->slave, kbuf[i]);
        }
        done += chunk;
    }
    /* The echo just pushed into m_buf: wake any master reader/poller once. */
    wq_wake_all(&p->m_read_wq);
    return (int32_t)done;
}

/** Report output, input space and slave hangup on the same master queue. */
static short ptm_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct pty *p = (struct pty *)e->priv;
    short revents = 0;

    if (wq) {
        *wq = &p->m_read_wq;
    }
    if (pty_m_avail(p) != 0 || p->slave_opens == 0) {
        revents |= (short)(events & POLLIN);
    }
    if (p->slave_opens == 0) {
        revents |= POLLHUP; /* every slave hung up */
    }
    if (n_tty_has_room(&p->slave.ldisc)) {
        revents |= (short)(events & POLLOUT);
    }
    return revents;
}

static int32_t ptm_ioctl(fd_entry_t *e, uint32_t request, uint32_t arg)
{
    struct pty *p = (struct pty *)e->priv;

    switch (request) {
    case TIOCGPTN: {
        int n = p->index;
        if (copy_to_user((void __user *)arg, &n, sizeof(n)) != 0) {
            return -EFAULT;
        }
        return 0;
    }
    case TIOCSPTLCK: {
        int lock;
        if (copy_from_user(&lock, (const void __user *)arg, sizeof(lock)) != 0) {
            return -EFAULT;
        }
        p->locked = lock ? 1 : 0;
        return 0;
    }
    case TIOCGWINSZ:
        if (copy_to_user((void __user *)arg, &p->slave.winsize, sizeof(p->slave.winsize)) != 0) {
            return -EFAULT;
        }
        return 0;
    case TIOCSWINSZ:
        if (copy_from_user(&p->slave.winsize, (const void __user *)arg, sizeof(p->slave.winsize)) !=
            0) {
            return -EFAULT;
        }
        /* A resize notifies the slave's foreground group (SIGWINCH). */
        if (p->slave.fg_pgid != 0) {
            tty_signal_group(p->slave.fg_pgid, SIGWINCH);
        }
        return 0;
    case FIONREAD: {
        int n = (int)pty_m_avail(p);
        if (copy_to_user((void __user *)arg, &n, sizeof(n)) != 0) {
            return -EFAULT;
        }
        return 0;
    }
    case FIONBIO: {
        int on;
        if (copy_from_user(&on, (const void __user *)arg, sizeof(on)) != 0) {
            return -EFAULT;
        }
        if (e->ofd) {
            if (on) {
                e->ofd->status_flags |= OFD_NONBLOCK;
            } else {
                e->ofd->status_flags &= ~(uint32_t)OFD_NONBLOCK;
            }
        }
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

/** Mark the master closed, signal the foreground group, wake the readers. */
static void ptm_close(fd_entry_t *e)
{
    struct pty *p = (struct pty *)e->priv;

    if (!p) {
        return;
    }
    p->master_open = 0;
    if (p->slave.fg_pgid != 0) {
        tty_signal_group(p->slave.fg_pgid, SIGHUP);
    }
    tty_hangup(&p->slave);
    if (p->slave_opens == 0) {
        pty_free(p);
    }
}

static const struct file_ops ptm_ops = {ptm_read, ptm_write, ptm_poll, ptm_ioctl, ptm_close};

/* the SLAVE endpoint */

/** Include persistent master closure in every slave read wait. */
static int pts_read_ready(void *arg)
{
    struct pty *p = (struct pty *)arg;

    return !p->master_open || n_tty_readable(&p->slave.ldisc);
}

/**
 * Read slave input, draining queued bytes before the master's closing EOF.
 *
 * A session leader claims an unowned slave as its sole controlling terminal.
 * A background reader takes SIGTTIN, or EIO when SIGTTIN is blocked or
 * ignored.
 * The endpoint owns the wait so each wake rechecks master closure even after
 * another reader consumes the discipline's single canonical EOF marker.
 */
static int32_t pts_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct pty *p = (struct pty *)e->priv;
    unsigned char kbuf[N_TTY_READ_CHUNK];
    uint32_t want;
    int n;

    if (!(e->flags & FD_FLAG_READ)) {
        return -EBADF;
    }
    if (count == 0) {
        return 0;
    }
    want = count;
    if (want > sizeof(kbuf)) {
        want = sizeof(kbuf);
    }
    if (p->slave.sid == 0 && (int)current_proc->sid == (int)current_proc->pid) {
        tty_make_sole_ctty(&p->slave, (int)current_proc->sid, (int)current_proc->pgid);
    }
    if (tty_is_background(&p->slave, (int)current_proc->sid, (int)current_proc->pgid)) {
        if ((current_proc->sig_blocked & KSIG_BIT(SIGTTIN)) ||
            current_proc->sigacts[SIGTTIN].handler == KSIG_IGN) {
            return -EIO;
        }
        tty_signal_group((int)current_proc->pgid, SIGTTIN);
        return -EINTR;
    }
    for (;;) {
        n = tty_read(&p->slave, kbuf, (int)want, 1);
        if (n != -EAGAIN) {
            break;
        }
        if (!p->master_open) {
            return 0;
        }
        if (e->ofd && (e->ofd->status_flags & OFD_NONBLOCK)) {
            return -EAGAIN;
        }
        n = wq_wait_event(&p->slave.input_wq, pts_read_ready, p);
        if (n != 0) {
            return n;
        }
    }
    if (n > 0) {
        wq_wake_all(&p->m_read_wq);
    }
    if (n < 0) {
        return n;
    }
    if (copy_to_user(buf, kbuf, (uint32_t)n) != 0) {
        return -EFAULT;
    }
    return n;
}

/** Queue slave output and wake master readers even if a later copy fails. */
static int32_t pts_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct pty *p = (struct pty *)e->priv;
    uint8_t kbuf[256];
    uint32_t done = 0;

    if (!(e->flags & FD_FLAG_WRITE)) {
        return -EBADF;
    }
    if (count == 0) {
        return 0;
    }
    if (!p->master_open) {
        return -EIO; /* hung up: nobody will ever read the output */
    }
    /* A background writer is stopped with SIGTTOU only when TOSTOP is set (the
     * same rule as the console). */
    if ((p->slave.ldisc.tio.c_lflag & TOSTOP) &&
        tty_is_background(&p->slave, (int)current_proc->sid, (int)current_proc->pgid)) {
        if ((current_proc->sig_blocked & KSIG_BIT(SIGTTOU)) ||
            current_proc->sigacts[SIGTTOU].handler == KSIG_IGN) {
            return -EIO;
        }
        tty_signal_group((int)current_proc->pgid, SIGTTOU);
        return -EINTR;
    }
    while (done < count) {
        uint32_t chunk = count - done;

        if (chunk > sizeof(kbuf)) {
            chunk = sizeof(kbuf);
        }
        if (copy_from_user(kbuf, (const void __user *)(buf + done), chunk) != 0) {
            wq_wake_all(&p->m_read_wq);
            return (done > 0) ? (int32_t)done : -EFAULT;
        }
        /* Output processing (OPOST/ONLCR) lands the bytes in m_buf via the
         * sink. */
        tty_write(&p->slave, kbuf, (int)chunk);
        done += chunk;
    }
    wq_wake_all(&p->m_read_wq); /* output pushed into m_buf: wake the master */
    return (int32_t)done;
}

static short pts_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct pty *p = (struct pty *)e->priv;
    short revents = tty_poll(&p->slave, events, wq);

    /* tty_poll registers the wait queue only for a POLLIN query, but a master
     * close wakes that same input queue (tty_hangup). A poller waiting only for
     * the hangup (events without POLLIN, e.g. a bare POLLHUP wait) must park on
     * it too, or it would sleep through ptm_close and never see POLLHUP. */
    if (!*wq) {
        *wq = &p->slave.input_wq;
    }
    if (!p->master_open) {
        revents |= POLLHUP; /* the master hung up */
    }
    return revents;
}

static int32_t pts_ioctl(fd_entry_t *e, uint32_t request, uint32_t arg)
{
    /* The termios/session ioctls and FIONREAD are handled by tty_common_ioctl
     * (sys_ioctl routes a pts slave there before this op runs); only FIONBIO
     * reaches here. */
    switch (request) {
    case FIONBIO: {
        int on;
        if (copy_from_user(&on, (const void __user *)arg, sizeof(on)) != 0) {
            return -EFAULT;
        }
        if (e->ofd) {
            if (on) {
                e->ofd->status_flags |= OFD_NONBLOCK;
            } else {
                e->ofd->status_flags &= ~(uint32_t)OFD_NONBLOCK;
            }
        }
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

static void pts_close(fd_entry_t *e)
{
    struct pty *p = (struct pty *)e->priv;

    if (!p) {
        return;
    }
    if (p->slave_opens > 0) {
        p->slave_opens--;
    }
    if (p->slave_opens == 0) {
        wq_wake_all(&p->m_read_wq); /* master read now returns EOF */
        if (!p->master_open) {
            pty_free(p);
        }
    }
}

static const struct file_ops pts_ops = {pts_read, pts_write, pts_poll, pts_ioctl, pts_close};

/* open paths */

int pty_open_master(int flags)
{
    struct pty *p;
    int fd;
    fd_entry_t *e;

    if (!current_proc) {
        return -ESRCH;
    }
    p = pty_alloc();
    if (!p) {
        return -ENFILE;
    }
    fd = fd_alloc(&current_proc->fds);
    if (fd < 0) {
        pty_free(p);
        return -EMFILE;
    }
    e = &current_proc->fds.entries[fd];
    e->type = FD_TYPE_PTM;
    e->node = NULL;
    e->sock = NULL;
    e->ops = &ptm_ops;
    e->priv = p;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    if (flags & O_CLOEXEC_FLAG) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    if ((flags & O_NONBLOCK_FLAG) && e->ofd) {
        e->ofd->status_flags |= OFD_NONBLOCK;
    }
    p->master_open = 1;
    return fd;
}

int pty_open_slave(int index, int flags)
{
    struct pty *p;
    int fd;
    fd_entry_t *e;

    if (!current_proc) {
        return -ESRCH;
    }
    p = index < 0 ? NULL : pty_by_index(index);
    if (!p || !p->master_open) {
        return -ENXIO; /* no such pty */
    }
    if (p->locked) {
        return -EIO; /* not unlocked yet (unlockpt not called) */
    }
    fd = fd_alloc(&current_proc->fds);
    if (fd < 0) {
        return -EMFILE;
    }
    e = &current_proc->fds.entries[fd];
    e->type = FD_TYPE_PTS;
    e->node = NULL;
    e->sock = NULL;
    e->ops = &pts_ops;
    e->priv = p;
    /* Honour the access mode: a slave (or /dev/tty resolving to it) opened
     * O_RDONLY/O_WRONLY must not silently gain the other direction. */
    if ((flags & 3) == 1) {
        e->flags = FD_FLAG_WRITE;
    } else if ((flags & 3) == 2) {
        e->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    } else {
        e->flags = FD_FLAG_READ;
    }
    if (flags & O_CLOEXEC_FLAG) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    if ((flags & O_NONBLOCK_FLAG) && e->ofd) {
        e->ofd->status_flags |= OFD_NONBLOCK;
    }
    p->slave_opens++;
    return fd;
}

/**
 * Return the index of the allocated pty whose slave is the controlling terminal
 * of session `sid` (slave.sid == sid), or -1.
 *
 * /dev/tty resolves through this to the caller's controlling pty rather than
 * unconditionally the console.
 */
int pty_session_slave(int sid)
{
    struct pty *p;

    if (sid == 0) {
        return -1;
    }
    for (p = ptys; p; p = p->next) {
        if (p->slave.sid == sid) {
            return p->index;
        }
    }
    return -1;
}

/**
 * Atomically make `keep` the sole controlling terminal of session `sid`.
 *
 * In one interrupts-off region, publish `keep` as the session's terminal (sid
 * and foreground group pgid) and drop every other terminal the session
 * controls: the console and any other pty. The whole claim-and-release is
 * atomic against the timer-IRQ console drain, which gates on console_tty.sid.
 * That drain must never observe the new terminal claimed while the old console
 * is still owned by the same session; the transient would let it drain serial
 * input or deliver ISIG for a terminal the session no longer uses. Lives here
 * because it must reach both console_tty and the pty pool. The caller has
 * already authorized the claim: TIOCSCTTY checks leadership and ownership, and
 * the implicit read-claims require the terminal to be free and the caller a
 * session leader.
 */
void tty_make_sole_ctty(struct tty *keep, int sid, int pgid)
{
    uint32_t flags;
    struct pty *p;

    if (sid == 0 || !keep) {
        return;
    }
    flags = kirq_save();
    keep->sid = sid;
    keep->fg_pgid = pgid;
    if (&console_tty != keep && console_tty.sid == sid) {
        console_tty.sid = 0;
        console_tty.fg_pgid = 0;
    }
    for (p = ptys; p; p = p->next) {
        if (&p->slave != keep && p->slave.sid == sid) {
            p->slave.sid = 0;
            p->slave.fg_pgid = 0;
        }
    }
    kirq_restore(flags);
}

struct tty *pty_slave_tty(struct fd_entry *e)
{
    if (!e || e->type != FD_TYPE_PTS || !e->priv) {
        return NULL;
    }
    return &((struct pty *)e->priv)->slave;
}

int pty_is_allocated(int index)
{
    struct pty *p = index < 0 ? NULL : pty_by_index(index);

    return p != NULL && p->master_open;
}

int pty_index_limit(void)
{
    struct pty *p;
    int limit = 0;

    for (p = ptys; p; p = p->next) {
        if (p->index + 1 > limit) {
            limit = p->index + 1;
        }
    }
    return limit;
}

struct pty *pty_get(int index)
{
    return pty_is_allocated(index) ? pty_by_index(index) : NULL;
}

int pty_slave_index(struct fd_entry *e)
{
    if (!e || e->type != FD_TYPE_PTS || !e->priv) {
        return -1;
    }
    return ((struct pty *)e->priv)->index;
}

/**
 * Disassociate any pty slave controlled by session leader `sid` on its death.
 *
 * Only a session leader (sid == pid) owns a controlling terminal. When that
 * leader exits or dies, POSIX requires its controlling terminal be released, so
 * a later session can claim the same /dev/pts/N via TIOCSCTTY instead of seeing
 * a stale, dead owner. Called beside the console release on the leader-death
 * paths (sys_exit, terminate, exec_fail). A pty has no timer-IRQ drain, so this
 * is a correctness and reuse concern, not the byte-stealing hazard the console
 * faced.
 */
void pty_release_leader(int sid, int pid)
{
    struct pty *p;

    if (sid == 0 || sid != pid) {
        return;
    }
    for (p = ptys; p; p = p->next) {
        if (p->slave.sid == sid) {
            tty_set_session(&p->slave, 0, 0);
        }
    }
}
