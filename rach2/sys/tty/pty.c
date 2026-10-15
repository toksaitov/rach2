/*
 * pty.c: pseudo-terminals, the /dev/ptmx and /dev/pts/N master and slave pairs
 * a network login gives a remote user as a terminal; pty.h has the data flow.
 *
 * The slave is a full struct tty (the N_TTY discipline plus sessions and job
 * control), so it reuses every terminal service; the master reads a byte ring
 * (m_buf) of the slave's output and the discipline's echo. Each endpoint is a
 * descriptor with its own file_ops.
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
#include "../rtc.h"

/* Linux ioctls. The pty-number requests have the exact values musl uses. */
#define FIONREAD   0x541B
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGPTN   0x80045430 /* master: get the slave index N (arg int*) */
#define TIOCSPTLCK 0x40045431 /* master: set/clear the slave lock (arg int*, 0 = unlock) */
#define TIOCPKT    0x5420     /* master: packet mode on or off (arg int*) */
#define TIOCGPKT   0x80045438 /* master: whether packet mode is on (arg int*) */
#define TCFLSH     0x540B     /* flush the queues: arg 0 input, 1 output, 2 both */

/* O_* flags honored on open (mirror syscall.c / fd.h). */
#define O_CLOEXEC_FLAG  02000000
#define O_NONBLOCK_FLAG 04000
#define O_NOCTTY_FLAG   0400

/* The live ptys, each allocated from the heap at its master's open and
 * freed when both ends are gone. The index is the lowest one no live pty
 * has, so `/dev/pts` names stay small and are reused as Unix98 reuses them. */
static struct pty *ptys;

void pty_init_pool(void)
{
    ptys = NULL;
}

/** The live pty with index `index`, or NULL. */
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

/** Unlink `p` from the list of live ptys and free it; no descriptor may
 * refer to it any more. */
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

/** The bytes in the master ring that the master has not read. */
static uint32_t pty_m_avail(struct pty *p)
{
    return p->m_tail - p->m_head;
}

/** The bytes the master ring can still take. */
static uint32_t pty_m_room(struct pty *p)
{
    return PTY_BUF_SIZE - pty_m_avail(p);
}

/** The room one byte of the slave's output is given in the ring: 8 under
 * OPOST, where n_tty_output makes at most two bytes of one (ONLCR's CR LF),
 * and 1 for raw output, which is a byte for a byte. */
static uint32_t pts_growth(const struct pty *p)
{
    return (p->slave.ldisc.tio.c_oflag & OPOST) ? 8u : 1u;
}

/**
 * Push one byte from the slave's output into the master ring.
 *
 * Drops the byte when the ring is full, which only echo can find, since a
 * slave writer waits for room (`pts_write`). The caller wakes master readers
 * once per write op (ptm_write echo, pts_write output), not per byte. Runs
 * in thread context, since no IRQ feeds a pty.
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

/** The wall clock's second, which a pty's times record. */
static uint32_t pty_now(void)
{
    int64_t sec, nsec;

    rtc_coarse_realtime(&sec, &nsec);
    return (uint32_t)sec;
}

/**
 * Wake the master's queue after slave output or echo: a change of data when
 * the slave's sink has put bytes in m_buf since the count `*from`, which moves
 * on to the present one, and of no kind otherwise, as input written with echo
 * off, or output the full ring dropped, gives the master nothing to read.
 */
static void wake_master(struct pty *p, uint32_t *from)
{
    wq_wake(&p->m_read_wq, p->m_tail != *from ? WQ_DATA : 0u);
    *from = p->m_tail;
}

/**
 * Allocate a pair at the lowest free index, or return NULL when the heap has
 * no room. One queue serves the master's readers, writers and pollers; the
 * slave's `space_wq` points at it, so a flush, a termios change, or a signal
 * char or an erase the master wrote wakes a master writer waiting for room.
 */
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
    p->gid = PTY_GID;
    p->atime = p->mtime = pty_now();
    p->mode = 0620; /* crw--w----, the Unix98 slave default */
    tty_init(&p->slave, pty_slave_sink, p);
    wait_queue_init(&p->m_read_wq);
    p->slave.space_wq = &p->m_read_wq;
    p->next = ptys;
    ptys = p;
    return p;
}

/** Whether `e`'s open file description is non-blocking. */
static int fd_is_nonblock(fd_entry_t *e)
{
    return e->ofd && (e->ofd->status_flags & OFD_NONBLOCK);
}

/* the master end */

/** A master reader can stop blocking: the slave produced output, or every
 * slave descriptor has closed (EOF). */
static int ptm_readable(void *arg)
{
    struct pty *p = (struct pty *)arg;
    return pty_m_avail(p) != 0 || p->slave_opens == 0;
}

/**
 * Read the slave's output from the master ring.
 *
 * Blocks until the ring has a byte or every slave description has closed,
 * which reads as end of file; -EAGAIN when non-blocking, -EINTR on a signal.
 * The ring is drained into a stack buffer with interrupts off and copied to
 * user memory, which may fault, after they are restored. In packet mode
 * (TIOCPKT) the bytes follow a status byte, TIOCPKT_DATA, 0, as on Linux,
 * which also sends the byte alone for a flush or a stop of the slave's
 * output; this driver sends only the first kind. Each piece taken from the
 * ring wakes a slave writer waiting for room.
 */
static int32_t ptm_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct pty *p = (struct pty *)e->priv;
    uint8_t kbuf[512];
    uint32_t got = 0;
    uint32_t lead = p->packet ? 1u : 0u;
    uint32_t to_read;

    if (count == 0) {
        return 0;
    }
    if (!user_range_ok((__force uintptr_t)buf, count)) {
        return -EFAULT;
    }
    while (pty_m_avail(p) == 0) {
        if (p->slave_opens == 0) {
            return 0;
        }
        if (fd_is_nonblock(e)) {
            return -EAGAIN;
        }
        if (wq_wait_event(&p->m_read_wq, ptm_readable, p) < 0) {
            return -EINTR;
        }
    }
    to_read = pty_m_avail(p);
    if (to_read > count - lead) {
        to_read = count - lead;
    }
    if (lead && copy_to_user((void __user *)buf, "", 1) != 0) {
        return -EFAULT;
    }
    while (got < to_read) {
        uint32_t chunk = to_read - got;
        uint32_t i;
        uint32_t flags;

        if (chunk > sizeof(kbuf)) {
            chunk = sizeof(kbuf);
        }
        flags = kirq_save();
        for (i = 0; i < chunk; i++) {
            kbuf[i] = p->m_buf[p->m_head & (PTY_BUF_SIZE - 1)];
            p->m_head++;
        }
        kirq_restore(flags);
        wq_wake(&p->slave.input_wq, WQ_ROOM);
        if (copy_to_user((void __user *)(buf + lead + got), kbuf, chunk) != 0) {
            return (got > 0) ? (int32_t)(lead + got) : -EFAULT;
        }
        got += chunk;
    }
    return (int32_t)(lead + got);
}

/** Whether the slave's discipline can take a line: the master's write wait. */
static int ptm_has_room(void *arg)
{
    struct pty *p = (struct pty *)arg;

    return n_tty_has_room(&p->slave.ldisc);
}

/** The offset of the first signal character in `bytes`, or -1: VINTR, VQUIT
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
 * Feed what the remote user typed to the slave's line discipline, which cooks
 * it, echoes into m_buf through the slave's sink, and wakes a slave reader
 * once a line is ready (tty_input_char).
 *
 * The discipline takes input only while a whole line more fits
 * (`n_tty_has_room`). A write that fills it returns short; one that has fed
 * nothing waits for the slave to read, or fails with -EAGAIN when
 * non-blocking. A signal character among the waiting bytes is fed at once and
 * the bytes before it are discarded, since the signal would discard them.
 * Master readers are woken before every return that fed bytes, a failed copy
 * included, so they see the echo.
 */
static int32_t ptm_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct pty *p = (struct pty *)e->priv;
    uint8_t kbuf[256];
    uint32_t done = 0;
    uint32_t pushed = p->m_tail;

    if (count == 0) {
        return 0;
    }
    if (!user_range_ok((__force uintptr_t)buf, count)) {
        return -EFAULT;
    }
    while (done < count) {
        uint32_t chunk = count - done;
        uint32_t i;

        if (chunk > sizeof(kbuf)) {
            chunk = sizeof(kbuf);
        }
        if (copy_from_user(kbuf, (const void __user *)(buf + done), chunk) != 0) {
            wake_master(p, &pushed);
            return (done > 0) ? (int32_t)done : -EFAULT;
        }
        for (i = 0; i < chunk; i++) {
            while (!n_tty_has_room(&p->slave.ldisc)) {
                int at = signal_char_at(&p->slave.ldisc, kbuf + i, (int)(chunk - i));

                if (at >= 0) {
                    p->slave.ldisc.counters.flush_bytes += (unsigned int)at;
                    i += (uint32_t)at;
                    tty_input_char(&p->slave, kbuf[i]);
                    i++;
                    continue;
                }
                wake_master(p, &pushed);
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
    wake_master(p, &pushed);
    return (int32_t)done;
}

/** Report the master's readiness, all on m_read_wq: POLLIN when the ring has
 * output or every slave has closed, POLLHUP once every slave has closed, and
 * POLLOUT while the slave's discipline has room for a line. */
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
        revents |= POLLHUP;
    }
    if (n_tty_has_room(&p->slave.ldisc)) {
        revents |= (short)(events & POLLOUT);
    }
    return revents;
}

/**
 * TCFLSH on the master, whose input is the slave's output and whose output
 * the slave's input: `which` 0 drops the first, 1 the second, 2 both, as on
 * Linux; anything else is -EINVAL.
 */
static int32_t ptm_flush(struct pty *p, unsigned long which)
{
    uint32_t flags;

    if (which > 2) {
        return -EINVAL;
    }
    if (which != 1) {
        flags = kirq_save();
        p->m_head = p->m_tail;
        kirq_restore(flags);
        wq_wake(&p->slave.input_wq, WQ_ROOM);
    }
    if (which != 0) {
        tty_flush_input(&p->slave);
    }
    return 0;
}

/** Set or read packet mode (TIOCPKT, TIOCGPKT) through the int at `arg`. */
static int32_t ptm_packet(struct pty *p, uint32_t request, uintptr_t arg)
{
    int on = p->packet;

    if (request == TIOCGPKT) {
        return copy_to_user((void __user *)arg, &on, sizeof(on)) != 0 ? -EFAULT : 0;
    }
    if (copy_from_user(&on, (const void __user *)arg, sizeof(on)) != 0) {
        return -EFAULT;
    }
    p->packet = on != 0;
    return 0;
}

/**
 * The master's ioctls: TIOCGPTN, TIOCSPTLCK, the window size (TIOCSWINSZ
 * sends SIGWINCH to the slave's foreground group), TIOCPKT and TIOCGPKT,
 * TCFLSH, and FIONREAD, the bytes of the ring not yet read. A bad user
 * pointer is -EFAULT, and any other request -ENOTTY.
 */
static int32_t ptm_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
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
        if (p->slave.fg_pgid != 0) {
            tty_signal_group(p->slave.fg_pgid, SIGWINCH);
        }
        return 0;
    case TIOCPKT:
    case TIOCGPKT:
        return ptm_packet(p, request, arg);
    case TCFLSH:
        return ptm_flush(p, arg);
    case FIONREAD: {
        int n = (int)pty_m_avail(p);
        if (copy_to_user((void __user *)arg, &n, sizeof(n)) != 0) {
            return -EFAULT;
        }
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

/**
 * Mark the master closed, send SIGHUP to the slave's foreground group and
 * hang up the slave, which wakes its readers; free the pair when no slave
 * description is open.
 */
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

/* the slave end */

/** The slave reader's wait condition: the master has closed, which stays
 * true, or the discipline has input to read. */
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
 * another reader consumes the discipline's single canonical EOF marker. A
 * read returns at most N_TTY_READ_CHUNK bytes and wakes a master writer
 * waiting for room.
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
    if (p->slave.sid == 0 && (int)current_proc->sid == (int)current_proc->tgid) {
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
        wq_wake(&p->m_read_wq, WQ_ROOM);
    }
    if (n < 0) {
        return n;
    }
    if (copy_to_user(buf, kbuf, (uint32_t)n) != 0) {
        return -EFAULT;
    }
    if (n > 0) {
        p->atime = pty_now();
    }
    return n;
}

/** A slave writer can go on: the ring has room for a byte of output at its
 * largest, or the master has closed. */
static int pts_has_room(void *arg)
{
    struct pty *p = (struct pty *)arg;

    return !p->master_open || pty_m_room(p) >= pts_growth(p);
}

/** A slave writer may begin: no other write is in progress, or the master
 * has closed. */
static int pts_write_free(void *arg)
{
    const struct pty *p = (const struct pty *)arg;

    return !p->writing || !p->master_open;
}

/**
 * Copy `count` bytes of slave output into m_buf, through OPOST processing by
 * way of the slave's sink, a piece at a time that fits the ring at its
 * largest (`pts_growth`): a writer the ring has no room for waits until the
 * master reads, as on Linux, or takes -EAGAIN, or returns what it wrote, when
 * non-blocking. Master readers are woken even if a later copy fails.
 */
static int32_t pts_write_out(fd_entry_t *e, struct pty *p, const char __user *buf, uint32_t count)
{
    uint8_t kbuf[256];
    uint32_t done = 0;
    uint32_t pushed = p->m_tail;

    while (done < count) {
        uint32_t chunk = count - done;
        uint32_t fit;

        while ((fit = pty_m_room(p) / pts_growth(p)) == 0) {
            int32_t stop = !p->master_open ? -EIO : fd_is_nonblock(e) ? -EAGAIN : 0;

            wake_master(p, &pushed);
            if (stop == 0 && wq_wait_event(&p->slave.input_wq, pts_has_room, p) < 0) {
                stop = -EINTR;
            }
            if (stop != 0) {
                return done > 0 ? (int32_t)done : stop;
            }
        }
        if (chunk > fit) {
            chunk = fit;
        }
        if (chunk > sizeof(kbuf)) {
            chunk = sizeof(kbuf);
        }
        if (copy_from_user(kbuf, (const void __user *)(buf + done), chunk) != 0) {
            wake_master(p, &pushed);
            return (done > 0) ? (int32_t)done : -EFAULT;
        }
        tty_write(&p->slave, kbuf, (int)chunk);
        done += chunk;
    }
    wake_master(p, &pushed); /* output pushed into m_buf: wake the master */
    p->mtime = pty_now();
    return (int32_t)done;
}

/**
 * Queue slave output for the master (`pts_write_out`), the whole of one
 * write before any other writer's, as Linux's `atomic_write_lock` keeps two
 * processes' lines apart: a writer that finds another write in progress
 * waits for it, or takes -EAGAIN when non-blocking. Once the master has
 * closed the write fails with EIO. With TOSTOP set, a background writer
 * takes SIGTTOU, or EIO when SIGTTOU is blocked or ignored, as on the
 * console.
 */
static int32_t pts_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct pty *p = (struct pty *)e->priv;
    int32_t rc;

    if (!(e->flags & FD_FLAG_WRITE)) {
        return -EBADF;
    }
    if (count == 0) {
        return 0;
    }
    if (!p->master_open) {
        return -EIO;
    }
    if ((p->slave.ldisc.tio.c_lflag & TOSTOP) &&
        tty_is_background(&p->slave, (int)current_proc->sid, (int)current_proc->pgid)) {
        if ((current_proc->sig_blocked & KSIG_BIT(SIGTTOU)) ||
            current_proc->sigacts[SIGTTOU].handler == KSIG_IGN) {
            return -EIO;
        }
        tty_signal_group((int)current_proc->pgid, SIGTTOU);
        return -EINTR;
    }
    while (p->writing) {
        if (!p->master_open) {
            return -EIO;
        }
        if (fd_is_nonblock(e)) {
            return -EAGAIN;
        }
        if (wq_wait_event(&p->slave.input_wq, pts_write_free, p) < 0) {
            return -EINTR;
        }
    }
    p->writing = 1;
    rc = pts_write_out(e, p, buf, count);
    p->writing = 0;
    wq_wake(&p->slave.input_wq, WQ_ROOM);
    return rc;
}

/**
 * Report the slave's readiness, POLLOUT only while the master ring has room
 * for a byte of output, and POLLHUP once the master has closed.
 *
 * tty_poll sets the wait queue only for a POLLIN query, but a master close
 * wakes that same input queue (tty_hangup), and a master read wakes it with
 * room, so a poller waiting only for the hangup or for room is parked on it
 * too; otherwise it would sleep through ptm_close or the read.
 */
static short pts_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct pty *p = (struct pty *)e->priv;
    short revents = tty_poll(&p->slave, events, wq);

    if (!*wq) {
        *wq = &p->slave.input_wq;
    }
    if (!p->master_open) {
        revents |= POLLHUP;
    } else if (pty_m_room(p) < pts_growth(p)) {
        revents &= (short)~POLLOUT;
    }
    return revents;
}

/**
 * Drop one slave description. When the last one closes, master readers are
 * woken to read end of file, and the pair is freed if the master is closed
 * too.
 */
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
        wq_wake_all(&p->m_read_wq);
        if (!p->master_open) {
            pty_free(p);
        }
    }
}

static const struct file_ops pts_ops = {pts_read, pts_write, pts_poll, NULL, pts_close};

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
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        pty_free(p);
        return fd;
    }
    e = &current_proc->fds->entries[fd];
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

/** Whether session `sid` has a controlling terminal: the console or a slave. */
static int session_has_ctty(int sid)
{
    return console_tty.sid == sid || pty_session_slave(sid) >= 0;
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
        return -ENXIO;
    }
    if (p->locked) {
        return -EIO;
    }
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        return fd;
    }
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_PTS;
    e->node = NULL;
    e->sock = NULL;
    e->ops = &pts_ops;
    e->priv = p;
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
    if (!(flags & O_NOCTTY_FLAG) && p->slave.sid == 0 && current_proc->sid == current_proc->tgid &&
        !session_has_ctty((int)current_proc->sid)) {
        tty_make_sole_ctty(&p->slave, (int)current_proc->sid, (int)current_proc->pgid);
    }
    return fd;
}

int tty_controlling(int sid, uint32_t *dev, int *fg_pgid)
{
    struct pty *p;

    if (sid == 0) {
        return 0;
    }
    if (console_tty.sid == sid) {
        *dev = (5u << 8) | 1u;
        *fg_pgid = console_tty.fg_pgid;
        return 1;
    }
    for (p = ptys; p; p = p->next) {
        if (p->slave.sid == sid) {
            uint32_t n = (uint32_t)p->index;

            *dev = (136u << 8) | (n & 0xFFu) | ((n & ~0xFFu) << 12);
            *fg_pgid = p->slave.fg_pgid;
            return 1;
        }
    }
    return 0;
}

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
 * The claim and the release are one step to the console's interrupt-side
 * drain, which drains only while console_tty.sid is nonzero. That drain must
 * not run between the claim of the new terminal and the release of the
 * console, or it would drain serial input or deliver a signal for a terminal
 * the session no longer uses. The function is in this file because
 * it reaches both console_tty and the list of ptys.
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
 * POSIX releases a session leader's controlling terminal when the leader
 * exits. A pty has no interrupt-side drain, so the release here matters only
 * for a later claim of the same slave.
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
