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
#include "../fs/vfs.h"
#include "../klog.h"
#include "../fs/file.h"
#include "../fs/krffs.h"
#include "../proc/process.h"
#include "../proc/ns.h"
#include "../mm/usercopy.h"
#include "../mm/kheap.h"
#include "../io/poll.h"
#include "../lib/kstring.h"
#include "../lib/kerrno.h"
#include "../lib/ksignal.h"
#include "../lib/kirq.h"
#include "../rtc.h"
#include "../syscall_internal.h"

/* Linux ioctls. The pty-number requests have the exact values musl uses. */
#define FIONREAD           0x541B
#define TIOCGWINSZ         0x5413
#define TIOCSWINSZ         0x5414
#define TIOCGPGRP          0x540F     /* the slave's foreground group (arg int*) */
#define TIOCGSID           0x5429     /* the slave's session (arg int*) */
#define TIOCGPTN           0x80045430 /* master: get the slave index N (arg int*) */
#define TIOCSPTLCK         0x40045431 /* master: set/clear the slave lock (arg int*, 0 = unlock) */
#define TIOCPKT_FLUSHREAD  1
#define TIOCPKT_FLUSHWRITE 2
#define TIOCPKT_STOP       4          /* the slave's output stopped */
#define TIOCPKT_START      8          /* and restarted */
#define TIOCPKT_NOSTOP     16         /* the slave's IXON with ^S and ^Q went off */
#define TIOCPKT_DOSTOP     32         /* and came on */
#define TIOCPKT            0x5420     /* master: packet mode on or off (arg int*) */
#define TIOCGPKT           0x80045438 /* master: whether packet mode is on (arg int*) */
#define TCFLSH             0x540B     /* flush the queues: arg 0 input, 1 output, 2 both */
#define TCXONC             0x540A     /* tcflow(3): arg TCOOFF, TCOON, TCIOFF or TCION */

/* O_* flags honored on open (mirror syscall.c / fd.h). */
#define O_CLOEXEC_FLAG 02000000
#define O_NOCTTY_FLAG  0400

/* The live ptys, each allocated from the heap at its master's open and
 * freed when both ends are gone. The index is the lowest one no live pty
 * has, so `/dev/pts` names stay small and are reused as Unix98 reuses them. */
static struct pty *ptys;

void pty_init_pool(void)
{
    ptys = NULL;
}

/**
 * Read the number from `v` to `end` as Linux's match_token and match_int
 * read a devpts option's: in `base`, or with `base` 0 in the base a leading
 * `0x` or `0` names, a minus only when `sign` is set, and nothing after
 * the digits; the value must fit an int. 0 with `*out` set, or -EINVAL.
 */
static int pts_number(const char *v, const char *end, int base, int sign, int32_t *out)
{
    uint32_t value = 0;
    int neg = sign && v < end && *v == '-';

    v += neg;
    if (base == 0) {
        base = 10;
        if (end - v > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
            base = 16;
            v += 2;
        } else if (v < end && v[0] == '0') {
            base = 8;
        }
    }
    if (v == end) {
        return -EINVAL;
    }
    for (; v < end; v++) {
        uint32_t d = *v >= 'a'   ? (uint32_t)(*v - 'a' + 10)
                     : *v >= 'A' ? (uint32_t)(*v - 'A' + 10)
                                 : (uint32_t)(*v - '0');

        if (d >= (uint32_t)base || value > (0x80000000u - d) / (uint32_t)base) {
            return -EINVAL;
        }
        value = value * (uint32_t)base + d;
    }
    if (value == 0x80000000u && !neg) {
        return -EINVAL;
    }
    *out = neg ? (int32_t)(0u - value) : (int32_t)value;
    return 0;
}

/** Whether the text from `key` to `end` is exactly `word`. */
static int pts_key_is(const char *key, const char *end, const char *word)
{
    while (key < end && *word && *key == *word) {
        key++;
        word++;
    }
    return key == end && *word == '\0';
}

/** Apply the devpts option named from `key` to `keyend`, whose value runs
 * from `v` to `end`, NULL when it has none, to `*o`. 0, or -EINVAL. */
static int pts_option(const char *key, const char *keyend, const char *v, const char *end,
                      struct vfs_pts_options *o)
{
    int is_uid = pts_key_is(key, keyend, "uid"), is_gid = pts_key_is(key, keyend, "gid");
    int octal = pts_key_is(key, keyend, "mode") || pts_key_is(key, keyend, "ptmxmode");
    int32_t n;
    int64_t k;

    if (!v) {
        return pts_key_is(key, keyend, "newinstance") ? 0 : -EINVAL;
    }
    if (!is_uid && !is_gid && !octal && !pts_key_is(key, keyend, "max")) {
        return -EINVAL;
    }
    if (pts_number(v, end, octal ? 8 : 0, pts_key_is(key, keyend, "max"), &n) != 0) {
        return -EINVAL;
    }
    if (is_uid || is_gid) {
        k = is_uid ? ns_uid_to_kernel(proc_userns(current_proc), (uint32_t)n)
                   : ns_gid_to_kernel(proc_userns(current_proc), (uint32_t)n);
        if (k < 0) {
            return -EINVAL;
        }
        *(is_uid ? &o->uid : &o->gid) = (uint32_t)k;
        *(is_uid ? &o->setuid : &o->setgid) = 1;
    } else if (octal) {
        *(key[0] == 'm' ? &o->mode : &o->ptmxmode) = (uint32_t)n & 07777u;
    } else if (n < 0 || (uint32_t)n > PTY_MAX_PAIRS) {
        return -EINVAL;
    } else {
        o->max = (uint32_t)n;
    }
    return 0;
}

int pty_parse_options(const char *options, struct vfs_pts_options *o)
{
    const char *p = options ? options : "";

    k_memset(o, 0, sizeof(*o));
    o->mode = 0600;     /* DEVPTS_DEFAULT_MODE */
    o->ptmxmode = 0000; /* DEVPTS_DEFAULT_PTMX_MODE */
    o->max = PTY_MAX_PAIRS;
    while (*p) {
        const char *key = p, *keyend, *v = NULL, *end;

        while (*p && *p != ',' && *p != '=') {
            p++;
        }
        keyend = p;
        if (*p == '=') {
            v = ++p;
            while (*p && *p != ',') {
                p++;
            }
        }
        end = p;
        if (*p == ',') {
            p++;
        }
        if (key == keyend && !v) {
            continue; /* strsep's empty item */
        }
        if (pts_option(key, keyend, v, end, o) != 0) {
            klog(KLOG_WARN, "devpts", "called with bogus options");
            return -EINVAL;
        }
    }
    return 0;
}

/** The live pty of the devpts instance `inst` with index `index`, or NULL.
 * The kernel's pairs are the system instance's, 0. */
static struct pty *pty_by_index(uint32_t inst, int index)
{
    struct pty *p;

    for (p = ptys; p; p = p->next) {
        if (p->index == index && p->inst == inst) {
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
    pid_nums_set(&p->slave.fg_nums, 0);
    kfree(p);
}

/** The bytes in the master ring that the master has not read. */
static uint32_t pty_m_avail(const struct pty *p)
{
    return p->m_tail - p->m_head;
}

/** The bytes the master ring can still take. */
static uint32_t pty_m_room(const struct pty *p)
{
    return PTY_BUF_SIZE - pty_m_avail(p);
}

/** The room one byte of the slave's output is given in the ring: 2 under
 * OPOST, where n_tty_output makes at most two bytes of one (ONLCR's CR LF),
 * and 1 for raw output, which is a byte for a byte. */
static uint32_t pts_growth(const struct pty *p)
{
    return (p->slave.ldisc.tio.c_oflag & OPOST) ? 2u : 1u;
}

/**
 * Push one byte from the slave's output into the master ring.
 *
 * Drops the byte when the ring is full, which echo and output never find,
 * since both stop at the slave's `room` (`pty_room`); a TCIOFF or TCION
 * character is dropped there, as Linux's pty_write drops what finds no
 * room. The caller wakes master readers
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

/** The slave `t`'s `room`: the bytes the master ring can still take. */
static unsigned int pty_room(struct tty *t)
{
    return pty_m_room((const struct pty *)t->sink_ctx);
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
 * The slave `t`'s flush, `what` a set of TTY_FLUSH_* bits (tty.h), as Linux's
 * pty_flush_buffer and n_tty_packet_mode_flush: TTY_FLUSH_OUT and
 * TTY_FLUSH_HANGUP discard the output in `m_buf`, and in packet mode
 * TTY_FLUSH_IN sets TIOCPKT_FLUSHREAD and TTY_FLUSH_OUT TIOCPKT_FLUSHWRITE for
 * the master's next read, while TTY_FLUSH_HANGUP clears every pending bit.
 * Takes interrupts off, wakes the master's readers when a status bit is set
 * and the slave's writers when output was discarded.
 */
static void pty_flush(struct tty *t, int what)
{
    struct pty *p = t->sink_ctx;
    uint32_t flags = kirq_save();

    if (what & (TTY_FLUSH_OUT | TTY_FLUSH_HANGUP)) {
        p->m_head = p->m_tail;
        wq_wake(&t->input_wq, WQ_ROOM);
    }
    if (what & TTY_FLUSH_HANGUP) {
        p->pktstatus = 0;
    } else if (p->packet) {
        if (what & TTY_FLUSH_IN) {
            p->pktstatus |= TIOCPKT_FLUSHREAD;
        }
        if (what & TTY_FLUSH_OUT) {
            p->pktstatus |= TIOCPKT_FLUSHWRITE;
        }
        wq_wake(&p->m_read_wq, WQ_DATA);
    }
    kirq_restore(flags);
}

/**
 * The slave `t`'s change of output flow, `what` a set of TTY_FLOW_* bits
 * (tty.h), as Linux's pty_stop, pty_start and pty_set_termios: in packet
 * mode TTY_FLOW_STOP sets TIOCPKT_STOP and clears TIOCPKT_START,
 * TTY_FLOW_START the reverse, and TTY_FLOW_DOSTOP and TTY_FLOW_NOSTOP set
 * their bit and clear the other, for the master's next read, whose readers
 * are woken; TTY_FLOW_DATA wakes them for bytes put in `m_buf` outside a
 * write. Takes interrupts off.
 */
static void pty_flow(struct tty *t, int what)
{
    struct pty *p = t->sink_ctx;
    uint32_t flags = kirq_save();

    if (p->packet &&
        (what & (TTY_FLOW_STOP | TTY_FLOW_START | TTY_FLOW_DOSTOP | TTY_FLOW_NOSTOP))) {
        if (what & TTY_FLOW_STOP) {
            p->pktstatus = (uint8_t)((p->pktstatus & ~TIOCPKT_START) | TIOCPKT_STOP);
        }
        if (what & TTY_FLOW_START) {
            p->pktstatus = (uint8_t)((p->pktstatus & ~TIOCPKT_STOP) | TIOCPKT_START);
        }
        if (what & TTY_FLOW_DOSTOP) {
            p->pktstatus = (uint8_t)((p->pktstatus & ~TIOCPKT_NOSTOP) | TIOCPKT_DOSTOP);
        }
        if (what & TTY_FLOW_NOSTOP) {
            p->pktstatus = (uint8_t)((p->pktstatus & ~TIOCPKT_DOSTOP) | TIOCPKT_NOSTOP);
        }
        wq_wake(&p->m_read_wq, WQ_DATA);
    } else if (what & TTY_FLOW_DATA) {
        wq_wake(&p->m_read_wq, WQ_DATA);
    }
    kirq_restore(flags);
}

/**
 * Allocate a pair of the devpts instance `inst` at the lowest free index
 * it has from `first` on, 0 for a pair of /dev/ptmx, its slave owned and
 * moded by the devpts options `o`, or NULL for the kernel's defaults, or
 * return NULL when the heap has no room
 * or a pair of /dev/ptmx would reach PTY_KERNEL_INDEX, past which its
 * slave's inode would be another node's of /dev. One queue serves the
 * master's readers, writers and pollers; the slave's `space_wq` points at
 * it, so a flush, a termios change, or a signal char or an erase the master
 * wrote wakes a master writer waiting for room.
 */
static struct pty *pty_alloc(uint32_t inst, int first, const struct vfs_pts_options *o)
{
    struct pty *p = kzalloc(sizeof(*p));
    int i;

    if (!p) {
        return NULL;
    }
    for (i = first; pty_by_index(inst, i); i++) {
    }
    if (first < PTY_KERNEL_INDEX && i >= PTY_KERNEL_INDEX) {
        kfree(p);
        return NULL;
    }
    p->index = i;
    p->inst = inst;
    p->locked = 1; /* unlockpt() (TIOCSPTLCK 0) must run before pts open */
    p->uid = current_proc ? proc_fsuid(current_proc) : 0;
    p->gid = PTY_GID;
    p->atime = p->mtime = pty_now();
    p->mode = 0620; /* crw--w----, the Unix98 slave default */
    if (o) {
        p->uid = o->setuid ? o->uid : p->uid; /* Linux's devpts_pty_new */
        p->gid = o->setgid ? o->gid : current_proc ? proc_fsgid(current_proc) : 0;
        p->mode = (uint16_t)(o->mode & 07777u);
    }
    tty_init(&p->slave, pty_slave_sink, p);
    p->slave.room = pty_room;
    p->slave.flush = pty_flush;
    p->slave.flow = pty_flow;
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

/** A master reader can stop blocking: output or packet status is ready,
 * or every slave descriptor has closed (EOF). */
static int ptm_readable(void *arg)
{
    struct pty *p = (struct pty *)arg;
    return pty_m_avail(p) != 0 || (p->packet && p->pktstatus) || p->slave_opens == 0;
}

/**
 * Read the slave's output from the master ring.
 *
 * Blocks until the ring has a byte or no slave description is open. Once
 * the last slave description has closed and the ring is drained, a read is
 * -EIO, as Linux's is after the slave hangs up; before any slave has opened
 * it reads as end of file. -EAGAIN when non-blocking, -EINTR on a signal.
 * The ring is drained into a stack buffer with interrupts off and copied to
 * user memory, which may fault, after they are restored. In packet mode
 * (TIOCPKT) the bytes follow a status byte, TIOCPKT_DATA, 0, as on Linux,
 * or a pending status byte alone, the TIOCPKT_FLUSHREAD and
 * TIOCPKT_FLUSHWRITE of the slave's flushes (`pty_flush`). Each piece taken
 * from the ring wakes a slave writer waiting for room.
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
    while (pty_m_avail(p) == 0 && !(p->packet && p->pktstatus)) {
        if (p->hung_up) {
            return -EIO;
        }
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
    if (p->packet && p->pktstatus) {
        uint32_t flags = kirq_save();
        uint8_t status = p->pktstatus;

        p->pktstatus = 0;
        kirq_restore(flags);
        return copy_to_user((void __user *)buf, &status, 1) != 0 ? -EFAULT : 1;
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

/** Whether the master can feed the slave a byte: no TCOOFF stopped it and
 * the slave's discipline can take a line. Whether a slave description is
 * open does not matter, as on Linux, where the master holds a count of the
 * slave's own and its writes fill the slave's buffer after the slave
 * closes. */
static int ptm_can_feed(const struct pty *p)
{
    return !p->m_stopped && n_tty_has_room(&p->slave.ldisc);
}

/** A master writer's wait predicate: it can feed the slave
 * (`ptm_can_feed`). */
static int ptm_has_room(void *arg)
{
    return ptm_can_feed((const struct pty *)arg);
}

/** The offset of the first signal character in `bytes` that flushes the
 * input, or -1: VINTR, VQUIT or VSUSP with ISIG on and NOFLSH off, a
 * disabled (zero) one never matching. */
static int signal_char_at(const struct n_tty *ld, const uint8_t *bytes, int n)
{
    int i;

    if (!(ld->tio.c_lflag & ISIG) || (ld->tio.c_lflag & NOFLSH)) {
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

/* A master write's place: the piece of it copied in and where it stands. */
struct ptm_feed {
    fd_entry_t *e; /* NULL for the kernel's feed */
    struct pty *p;
    const uint8_t *kbuf; /* the piece copied from the user */
    uint32_t chunk;      /* its bytes */
    uint32_t done;       /* the bytes of the write before it */
    uint32_t prior;      /* the bytes of a vector's elements before the write */
    uint32_t pushed;     /* m_tail when the master's readers were last woken */
    uint32_t ahead;      /* the bytes of the piece the look ahead has seen */
    uint32_t acted;      /* those up to the last one that stopped or restarted output */
};

/**
 * Feed the bytes of the master write `f`'s piece from `*i` up to `f->acted`,
 * the last whose look ahead stopped or restarted output, though the slave's
 * discipline has no room for a line, and move `*i` past them. A write that
 * returns has thereby taken every byte it acted on, as Linux's flip buffer
 * holds the bytes its look ahead reads; the read queue stores them, since
 * the look ahead stops where it would be full (`ptm_look_end`), unless a
 * canonical line is full, whose bytes are dropped as any byte past its end.
 */
static void ptm_take_acted(struct ptm_feed *f, uint32_t *i)
{
    while (*i < f->acted) {
        tty_input_looked(&f->p->slave, f->kbuf[*i]);
        (*i)++;
    }
}

/**
 * The end of the look ahead of the master write `f` from byte `i` of its
 * piece: the piece's end, or sooner where the bytes from `i` would pass what
 * the slave's read queue can still store beside the bytes the discipline
 * holds. Every byte up to the last that acts is fed (`ptm_take_acted`), so a
 * write never acts on, feeds and counts a byte the discipline would drop, as
 * Linux's look ahead reads only the bytes its flip buffer has taken.
 */
static uint32_t ptm_look_end(const struct ptm_feed *f, uint32_t i)
{
    unsigned int held = n_tty_held(&f->p->slave.ldisc);
    uint32_t room = held < N_TTY_READ_MAX ? N_TTY_READ_MAX - held : 0;

    return room < f->chunk - i ? i + room : f->chunk;
}

/**
 * Wait until the master write `f` can feed byte `*i` of its piece
 * (`ptm_can_feed`). While it cannot, a signal character that flushes is fed
 * at once, the bytes before it discarded and `*i` moved past it, and the
 * VSTOP and VSTART from `f->ahead` on act at once (`tty_lookahead`),
 * `f->ahead` moving to the piece's end or as far as the read queue can store
 * (`ptm_look_end`). It takes the bytes it acted on before any wait
 * (`ptm_take_acted`), while the space measured is still its own, and returns
 * that accepted prefix. Returns 0 to go on feeding, or what `ptm_write_in`
 * returns: the bytes fed with `f->prior` among them, -EAGAIN or -EINTR. A
 * feed with no descriptor, the kernel's (`pty_kernel_feed`), never waits.
 */
static int32_t ptm_wait_feed(struct ptm_feed *f, uint32_t *i)
{
    struct pty *p = f->p;

    while (!ptm_can_feed(p)) {
        int at =
            p->m_stopped ? -1 : signal_char_at(&p->slave.ldisc, f->kbuf + *i, (int)(f->chunk - *i));

        if (at >= 0) {
            p->slave.ldisc.counters.flush_bytes += (unsigned int)at;
            *i += (uint32_t)at;
            tty_input_char(&p->slave, f->kbuf[*i]);
            (*i)++;
            continue;
        }
        if (!p->m_stopped) {
            uint32_t end = ptm_look_end(f, *i);

            for (f->ahead = f->ahead > *i ? f->ahead : *i; f->ahead < end; f->ahead++) {
                if (tty_lookahead(&p->slave, f->kbuf[f->ahead], 1)) {
                    f->acted = f->ahead + 1;
                }
            }
        }
        ptm_take_acted(f, i);
        if (f->prior + f->done + *i > 0 || !f->e || fd_is_nonblock(f->e)) {
            wake_master(p, &f->pushed);
            return f->prior + f->done + *i > 0 ? (int32_t)(f->prior + f->done + *i) : -EAGAIN;
        }
        wake_master(p, &f->pushed);
        if (wq_wait_event(&p->m_read_wq, ptm_has_room, p) < 0) {
            return f->prior + f->done + *i > 0 ? (int32_t)(f->prior + f->done + *i) : -EINTR;
        }
    }
    return 0;
}

/**
 * Feed what the remote user typed to the slave's line discipline, which cooks
 * it, echoes into m_buf through the slave's sink, and wakes a slave reader
 * once a line is ready (tty_input_char).
 *
 * The discipline takes input only while a whole line more fits
 * (`n_tty_has_room`), and none while TCOOFF on the master stops it. A write
 * that fills it returns short; one that has fed nothing waits for the slave
 * to read or for TCOON, or fails with -EAGAIN when non-blocking, as Linux's
 * n_tty_write. It goes on after every slave description has closed, as on
 * Linux, until the discipline fills. A signal character among the waiting
 * bytes is fed at once and the bytes before it are discarded, since the
 * signal would discard them, unless NOFLSH keeps them; a VSTOP or VSTART
 * among them acts at once (`tty_lookahead`) and is fed later without acting
 * again, as Linux looks ahead in its flip buffer. The bytes up to the last
 * that acted are fed before any wait (`ptm_take_acted`), so that another
 * writer cannot take their space, and the write returns that prefix.
 * Master readers are woken before every return that fed bytes, a failed
 * copy included, so they see the echo. `prior` is the bytes a vector's
 * earlier elements fed, which the result counts: a write after them that
 * fills the input returns short, though it fed none, where one with nothing
 * before it would wait or fail, so that a vector returns as one write of the
 * same bytes does.
 */
static int32_t ptm_write_in(fd_entry_t *e, const char __user *buf, uint32_t count, uint32_t prior)
{
    struct ptm_feed f;
    uint8_t kbuf[256];

    if (count == 0) {
        return 0;
    }
    if (!user_range_ok((__force uintptr_t)buf, count)) {
        return -EFAULT;
    }
    f.e = e;
    f.p = (struct pty *)e->priv;
    f.kbuf = kbuf;
    f.done = 0;
    f.prior = prior;
    f.pushed = f.p->m_tail;
    while (f.done < count) {
        uint32_t i;
        int32_t rc;

        f.ahead = f.acted = 0;
        f.chunk = count - f.done;
        if (f.chunk > sizeof(kbuf)) {
            f.chunk = sizeof(kbuf);
        }
        if (copy_from_user(kbuf, (const void __user *)(buf + f.done), f.chunk) != 0) {
            wake_master(f.p, &f.pushed);
            return (prior + f.done > 0) ? (int32_t)(prior + f.done) : -EFAULT;
        }
        for (i = 0; i < f.chunk; i++) {
            rc = ptm_wait_feed(&f, &i);
            if (rc != 0) {
                return rc;
            }
            if (i >= f.chunk) {
                break;
            }
            if (i < f.ahead) {
                tty_input_looked(&f.p->slave, kbuf[i]);
            } else {
                tty_input_char(&f.p->slave, kbuf[i]);
            }
        }
        f.done += f.chunk;
    }
    wake_master(f.p, &f.pushed);
    return (int32_t)(prior + f.done);
}

/** Whether the preceding master write has released the pair. */
static int ptm_write_free(void *arg)
{
    return !((const struct pty *)arg)->m_writing;
}

/** Take the master's write lock: 0, -EAGAIN when `nonblock` while
 * another holds it, or -EINTR when a signal ends a blocking caller's wait.
 * The caller releases it with `ptm_write_unlock` after the whole write. */
static int32_t ptm_write_lock(struct pty *p, int nonblock)
{
    while (p->m_writing) {
        if (nonblock) {
            return -EAGAIN;
        }
        if (wq_wait_event(&p->m_read_wq, ptm_write_free, p) < 0) {
            return -EINTR;
        }
    }
    p->m_writing = 1;
    return 0;
}

/** Release the master's write lock and wake the next writer. */
static void ptm_write_unlock(struct pty *p)
{
    p->m_writing = 0;
    wq_wake(&p->m_read_wq, 0);
}

/** Feed one master write before another enters, including across a user
 * copy or a wait for input room, as Linux's tty_write_lock does. Return
 * the bytes fed or the lock's or transfer's error. */
static int32_t ptm_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    int32_t rc;

    if (count == 0) {
        return 0;
    }
    rc = ptm_write_lock((struct pty *)e->priv, fd_is_nonblock(e));
    if (rc == 0) {
        rc = ptm_write_in(e, buf, count, 0);
        ptm_write_unlock((struct pty *)e->priv);
    }
    return rc;
}

/** Feed the imported vector with one lock, as defined in pty.h. */
int32_t pty_master_writev(fd_entry_t *e, const struct kernel_iovec *iov, int count)
{
    int32_t rc, fed, total = 0;
    int i;

    for (i = 0; i < count && iov[i].iov_len == 0; i++) {
    }
    if (i == count) {
        return 0;
    }
    rc = ptm_write_lock((struct pty *)e->priv, fd_is_nonblock(e));
    if (rc != 0) {
        return rc;
    }
    for (; i < count; i++) {
        if (iov[i].iov_len == 0) {
            continue;
        }
        rc = ptm_write_in(e, (const char __user *)iov[i].iov_base, (uint32_t)iov[i].iov_len,
                          (uint32_t)total);
        if (rc < 0) {
            break;
        }
        fed = rc - total;
        total = rc;
        if ((unsigned long)fed < iov[i].iov_len) {
            break;
        }
    }
    ptm_write_unlock((struct pty *)e->priv);
    return total > 0 ? total : rc;
}

/** Report the master's readiness, all on m_read_wq, as Linux's n_tty_poll:
 * POLLIN and POLLRDNORM when the ring has output or every slave has closed,
 * POLLHUP once every slave has closed, POLLIN, POLLRDNORM and POLLPRI for
 * pending packet status, and POLLOUT and POLLWRNORM while the slave's
 * discipline has room for a line and no TCOOFF stopped the master. */
static short ptm_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct pty *p = (struct pty *)e->priv;
    short revents = 0;

    if (wq) {
        *wq = &p->m_read_wq;
    }
    if (pty_m_avail(p) != 0 || p->slave_opens == 0) {
        revents |= (short)(events & (POLLIN | POLLRDNORM));
    }
    if (p->packet && p->pktstatus) {
        revents |= (short)(events & (POLLIN | POLLRDNORM | POLLPRI));
    }
    if (p->slave_opens == 0) {
        revents |= POLLHUP;
    }
    if (!p->m_stopped && n_tty_has_room(&p->slave.ldisc)) {
        revents |= (short)(events & (POLLOUT | POLLWRNORM));
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

/**
 * tcflow(3) on the master, whose output is the slave's input: TCOOFF stops
 * the master's writes until TCOON, and TCIOFF and TCION write ^S and ^Q, the
 * master's VSTOP and VSTART, to the slave's input unless TCOOFF stopped it,
 * as Linux's tty_send_xchar does through pty_write. Both wait for the
 * master write lock even on a nonblocking descriptor, with -EINTR when a
 * signal ends the wait, and hold it while feeding. Another action is -EINVAL.
 */
static int32_t ptm_tcflow(struct pty *p, unsigned long action)
{
    uint32_t pushed;
    int32_t rc;

    switch (action) {
    case TTY_TCOOFF:
        p->m_stopped = 1;
        return 0;
    case TTY_TCOON:
        if (p->m_stopped) {
            p->m_stopped = 0;
            wq_wake(&p->m_read_wq, WQ_ROOM);
        }
        return 0;
    case TTY_TCIOFF:
    case TTY_TCION:
        rc = ptm_write_lock(p, 0);
        if (rc != 0) {
            return rc;
        }
        pushed = p->m_tail;
        if (!p->m_stopped) {
            tty_input_char(&p->slave, action == TTY_TCIOFF ? 023 : 021);
            wake_master(p, &pushed);
        }
        ptm_write_unlock(p);
        return 0;
    default:
        return -EINVAL;
    }
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
    if (on && !p->packet) {
        p->pktstatus = 0;
    }
    p->packet = on != 0;
    return 0;
}

/**
 * The master's ioctls: TIOCGPTN, TIOCSPTLCK, the window size (TIOCSWINSZ
 * sends SIGWINCH to the slave's foreground group when the size changes,
 * `tty_resize`), TIOCPKT and TIOCGPKT, TCFLSH, TCXONC, FIONREAD, the bytes of
 * the ring not yet read, and the slave's foreground group and session,
 * TIOCGPGRP and TIOCGSID, which Linux answers on the master without the
 * caller's controlling terminal: the group's number, which stays once the
 * group has ended, or 0 for none, and the session's, or -ENOTTY for none. A
 * bad user pointer is -EFAULT, and any other request -ENOTTY.
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
    case TIOCSWINSZ: {
        struct tty_winsize ws;

        /* Read whole before the size changes, as Linux's tiocswinsz does. */
        if (copy_from_user(&ws, (const void __user *)arg, sizeof(ws)) != 0) {
            return -EFAULT;
        }
        tty_resize(&p->slave, &ws);
        return 0;
    }
    case TIOCGPGRP: {
        int pgrp = (int)tty_fg_seen(&p->slave);

        return copy_to_user((void __user *)arg, &pgrp, sizeof(pgrp)) != 0 ? -EFAULT : 0;
    }
    case TIOCGSID: {
        int sid = (int)pid_seen((uint32_t)p->slave.sid);

        if (p->slave.sid == 0) {
            return -ENOTTY;
        }
        return copy_to_user((void __user *)arg, &sid, sizeof(sid)) != 0 ? -EFAULT : 0;
    }
    case TIOCPKT:
    case TIOCGPKT:
        return ptm_packet(p, request, arg);
    case TCFLSH:
        return ptm_flush(p, arg);
    case TCXONC:
        return ptm_tcflow(p, arg);
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
 * Mark the master of `p` closed, send SIGHUP to the slave's foreground group
 * and hang up the slave, which wakes its readers; free the pair when no
 * slave description is open.
 */
static void pty_master_gone(struct pty *p)
{
    p->master_open = 0;
    if (p->slave.fg_pgid != 0) {
        tty_signal_group(p->slave.fg_pgid, p->slave.fg_serial, SIGHUP);
    }
    tty_hangup(&p->slave);
    if (p->slave_opens == 0) {
        pty_free(p);
    }
}

/** The master's close (`pty_master_gone`). */
static void ptm_close(fd_entry_t *e)
{
    struct pty *p = (struct pty *)e->priv;

    if (!p) {
        return;
    }
    pty_master_gone(p);
}

static const struct file_ops ptm_ops = {ptm_read,  ptm_write, ptm_poll, ptm_ioctl,
                                        ptm_close, NULL,      NULL,     0};

/* the slave end */

/** Whether the slave description `e` of `p` is one a vhangup(2) since its
 * open has hung up (`tty_desc_hung`). */
static int pts_hung(const fd_entry_t *e, const struct pty *p)
{
    return e->ofd && tty_desc_hung(&p->slave, e->ofd->tty_hangups);
}

/**
 * Read slave input, draining queued bytes before the master's closing EOF.
 *
 * A read makes the slave no one's controlling terminal, as on Linux, where
 * only an open without O_NOCTTY and TIOCSCTTY do. A background reader takes
 * SIGTTIN, or EIO when SIGTTIN is blocked or ignored or its group is
 * orphaned (`tty_job_check`).
 * The wait is `tty_read`'s, which honours VMIN and VTIME and ends at the
 * master's close (`tty_hangup`), even after another reader consumed the
 * discipline's single canonical EOF marker. A raw read returns at most
 * N_TTY_READ_CHUNK bytes, and a canonical one its whole line, copied a chunk
 * at a time (`tty_read_rest`); each chunk wakes a master writer waiting for
 * room. A vhangup(2) of the description ends the read with 0, or with the
 * bytes already copied when it comes during a copy to the user.
 */
static int32_t pts_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct pty *p = (struct pty *)e->priv;
    unsigned char kbuf[N_TTY_READ_CHUNK];
    uint32_t want, done;
    uint32_t opened = e->ofd ? e->ofd->tty_hangups : p->slave.vhangups;
    int n;

    if (!(e->flags & FD_FLAG_READ)) {
        return -EBADF;
    }
    if (count == 0 || pts_hung(e, p)) {
        return 0;
    }
    want = count;
    if (want > sizeof(kbuf)) {
        want = sizeof(kbuf);
    }
    n = tty_job_check(&p->slave, SIGTTIN);
    if (n != 0) {
        return n;
    }
    n = tty_read(&p->slave, kbuf, (int)want, e->ofd && (e->ofd->status_flags & OFD_NONBLOCK),
                 opened);
    if (n < 0) {
        return n;
    }
    done = 0;
    while (n > 0) {
        wq_wake(&p->m_read_wq, WQ_ROOM);
        if (copy_to_user(buf + done, kbuf, (uint32_t)n) != 0) {
            return -EFAULT;
        }
        done += (uint32_t)n;
        p->atime = pty_now();
        want = count - done;
        if (want > sizeof(kbuf)) {
            want = sizeof(kbuf);
        }
        n = tty_read_rest(&p->slave, kbuf, (int)want, opened);
    }
    return (int32_t)done;
}

/* A slave writer's wait: its pty and the count of vhangups its description
 * was opened at, which a vhangup(2) since ends the wait, as Linux's
 * n_tty_write looks at tty_hung_up_p on every turn. */
struct pts_waiter {
    struct pty *p;
    int has_desc; /* the description keeps a count to compare */
    uint32_t opened;
};

/** Whether the description of the writer `w` has been hung up since it was
 * opened (`tty_desc_hung`). */
static int pts_waiter_hung(const struct pts_waiter *w)
{
    return w->has_desc && tty_desc_hung(&w->p->slave, w->opened);
}

/** The bytes of slave output `p`'s ring can take now: none while output is
 * stopped, and otherwise as many as fit at their largest (`pts_growth`). */
static uint32_t pts_fit(const struct pty *p)
{
    return p->slave.ldisc.stopped ? 0u : pty_m_room(p) / pts_growth(p);
}

/** A slave writer `arg`, a struct pts_waiter, can go on: output runs and the
 * ring has room for a byte of it at its largest, the master has closed, or
 * its description has been hung up. */
static int pts_has_room(void *arg)
{
    const struct pts_waiter *w = (const struct pts_waiter *)arg;

    return !w->p->master_open || pts_waiter_hung(w) || pts_fit(w->p) > 0;
}

/** A slave writer `arg`, a struct pts_waiter, may begin: no other write is
 * in progress, the master has closed, or its description has been hung up. */
static int pts_write_free(void *arg)
{
    const struct pts_waiter *w = (const struct pts_waiter *)arg;

    return !w->p->writing || !w->p->master_open || pts_waiter_hung(w);
}

/**
 * Copy `count` bytes of slave output into m_buf, through OPOST processing by
 * way of the slave's sink, a piece at a time that fits the ring at its
 * largest (`pts_growth`): a writer the ring has no room for, or whose output
 * is stopped (VSTOP or TCOOFF), waits until the master reads or output
 * restarts, as on Linux, or takes -EAGAIN, or returns what it wrote, when
 * non-blocking. A vhangup(2) of the description of the writer `w`, before a
 * piece or during its wait, ends the write with EIO or the bytes written, as
 * Linux's n_tty_write does. Master readers are woken even if a later copy
 * fails. The copy from the user may sleep on a page fault, so after it the
 * hangup and the master's close are tested again, as n_tty_write tests
 * tty_hung_up_p just before it writes, and the piece is fitted again, since
 * echo of the master's input may have filled the ring meanwhile. The echo
 * held goes out ahead of each piece as far as the ring has room
 * (`tty_write`), which then writes only what still fits; the rest of the
 * piece waits for room, as Linux's n_tty_write writes only into
 * pty_write_room, and is never dropped.
 */
static int32_t pts_write_out(fd_entry_t *e, struct pts_waiter *w, const char __user *buf,
                             uint32_t count)
{
    struct pty *p = w->p;
    uint8_t kbuf[256];
    uint32_t done = 0;
    uint32_t pushed = p->m_tail;

    while (done < count) {
        uint32_t chunk = count - done;
        uint32_t fit;

        while ((fit = pts_waiter_hung(w) ? 0u : pts_fit(p)) == 0) {
            int32_t stop = !p->master_open || pts_waiter_hung(w) ? -EIO
                           : fd_is_nonblock(e)                   ? -EAGAIN
                                                                 : 0;

            wake_master(p, &pushed);
            if (stop == 0 && wq_wait_event(&p->slave.input_wq, pts_has_room, w) < 0) {
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
        if (!p->master_open || pts_waiter_hung(w)) {
            wake_master(p, &pushed);
            return (done > 0) ? (int32_t)done : -EIO;
        }
        fit = pts_fit(p);
        if (chunk > fit) {
            chunk = fit;
        }
        done += (uint32_t)tty_write(&p->slave, kbuf, (int)chunk);
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
 * closed, or a vhangup(2) has hung up the description, the write fails with
 * EIO. With TOSTOP set, a background writer takes SIGTTOU, or EIO when its
 * group is orphaned, and writes when it blocks or ignores SIGTTOU
 * (`tty_job_check`), as on the console and on Linux.
 */
static int32_t pts_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct pty *p = (struct pty *)e->priv;
    struct pts_waiter w;
    int32_t rc;

    if (!(e->flags & FD_FLAG_WRITE)) {
        return -EBADF;
    }
    if (pts_hung(e, p)) {
        return -EIO;
    }
    if (count == 0) {
        return 0;
    }
    w.p = p;
    w.has_desc = e->ofd != NULL;
    w.opened = e->ofd ? e->ofd->tty_hangups : 0u;
    if (!p->master_open) {
        return -EIO;
    }
    if ((p->slave.ldisc.tio.c_lflag & TOSTOP) && (rc = tty_job_check(&p->slave, SIGTTOU)) != 0) {
        return rc;
    }
    while (p->writing) {
        if (!p->master_open || pts_waiter_hung(&w)) {
            return -EIO;
        }
        if (fd_is_nonblock(e)) {
            return -EAGAIN;
        }
        if (wq_wait_event(&p->slave.input_wq, pts_write_free, &w) < 0) {
            return -EINTR;
        }
    }
    p->writing = 1;
    rc = pts_write_out(e, &w, buf, count);
    p->writing = 0;
    wq_wake(&p->slave.input_wq, WQ_ROOM);
    return rc;
}

/**
 * Report the slave's readiness, POLLOUT only while output runs and the
 * master ring has room for a byte of it, and POLLHUP once the master has
 * closed.
 *
 * tty_poll parks every poller on the slave's input queue, which a master
 * close (tty_hangup) and a master read with room wake too, so a poller
 * waiting only for the hangup or for room wakes for ptm_close or the read.
 */
static short pts_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    struct pty *p = (struct pty *)e->priv;
    short revents;

    if (pts_hung(e, p)) {
        return (short)TTY_HUNG_POLL;
    }
    revents = tty_poll(&p->slave, events, wq);
    if (!p->master_open) {
        revents |= POLLHUP;
    } else if (pts_fit(p) == 0) {
        revents &= (short)~POLLOUT;
    }
    return revents;
}

/**
 * Drop one slave description. When the last one closes, the pair is hung
 * up and master readers are woken to read -EIO once the ring drains
 * (`ptm_read`), and the pair is freed if the master is closed too.
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
        p->hung_up = 1;
        wq_wake_all(&p->m_read_wq);
        if (!p->master_open) {
            pty_free(p);
        }
    }
}

static const struct file_ops pts_ops = {pts_read,  pts_write, pts_poll, NULL,
                                        pts_close, NULL,      NULL,     0};

/* open paths */

int pty_open_master(int flags)
{
    struct vfs_pts_options o;
    uint32_t inst;
    int mounted;

    if (!current_proc) {
        return -ESRCH;
    }
    mounted = vfs_devpts_options(&o, &inst);
    return pty_open_master_in(inst, mounted ? &o : NULL, flags);
}

int pty_open_master_in(uint32_t inst, const struct vfs_pts_options *o, int flags)
{
    struct pty *p;
    int fd, i;
    fd_entry_t *e;

    if (!current_proc) {
        return -ESRCH;
    }
    for (i = 0; pty_by_index(inst, i); i++) {
    }
    if (o && (uint32_t)i >= o->max) {
        return -ENOSPC; /* Linux's devpts_new_index past the instance's max */
    }
    p = pty_alloc(inst, 0, o);
    if (!p) {
        return -ENFILE;
    }
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        pty_free(p);
        return fd;
    }
    e = fd_slot(current_proc->fds, fd);
    e->type = FD_TYPE_PTM;
    e->node = NULL;
    e->sock = NULL;
    e->ops = &ptm_ops;
    e->priv = p;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    if (flags & O_CLOEXEC_FLAG) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    if (e->ofd) {
        e->ofd->status_flags |= OFD_OPEN_STATUS(flags);
    }
    p->master_open = 1;
    fd_install(current_proc->fds, fd);
    return fd;
}

/** Whether session `sid` has a controlling terminal: the console or a slave. */
static int session_has_ctty(int sid)
{
    return console_tty.sid == sid || pty_session_slave(sid) >= 0;
}

/**
 * Give the descriptor `e`, which is being opened with the open flags
 * `flags`, the slave of `p`: the pair counts one more slave description,
 * which remembers the slave's count of hangups, and a session leader
 * without a controlling terminal that opens a slave no session has, without
 * O_NOCTTY, takes it as its controlling terminal.
 */
static void pts_bind(struct pty *p, fd_entry_t *e, int flags)
{
    e->priv = p;
    if (e->ofd) {
        e->ofd->tty_hangups = p->slave.vhangups;
    }
    p->slave_opens++;
    p->hung_up = 0;
    if (!(flags & O_NOCTTY_FLAG) && p->slave.sid == 0 && current_proc->sid == current_proc->tgid &&
        !session_has_ctty((int)current_proc->sid)) {
        tty_make_sole_ctty(&p->slave, (int)current_proc->sid, (int)current_proc->pgid,
                           current_proc->pgrp_serial);
    }
}

int pty_open_slave(uint32_t inst, int index, int flags)
{
    return pty_open_pair_slave(index < 0 ? NULL : pty_by_index(inst, index), flags);
}

int pty_open_pair_slave(struct pty *p, int flags)
{
    int fd;
    fd_entry_t *e;

    if (!current_proc) {
        return -ESRCH;
    }
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
    e = fd_slot(current_proc->fds, fd);
    e->type = FD_TYPE_PTS;
    e->node = NULL;
    e->sock = NULL;
    e->ops = &pts_ops;
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
    if (e->ofd) {
        e->ofd->status_flags |= OFD_OPEN_STATUS(flags);
    }
    pts_bind(p, e, flags);
    fd_install(current_proc->fds, fd);
    return fd;
}

struct pty *pty_kernel_make(uint32_t dev, const char *node)
{
    struct pty *p = pty_alloc(0, PTY_KERNEL_INDEX, NULL);

    if (!p) {
        return NULL;
    }
    p->locked = 0;
    p->uid = 0;
    p->kdev = dev;
    p->knode = node;
    p->master_open = 1;
    return p;
}

void pty_kernel_close(struct pty *p)
{
    pty_master_gone(p);
}

uint32_t pty_kernel_feed(struct pty *p, const unsigned char *bytes, uint32_t n)
{
    struct ptm_feed f;
    uint32_t i;

    f.e = NULL;
    f.p = p;
    f.kbuf = bytes;
    f.chunk = n;
    f.done = f.prior = f.ahead = f.acted = 0;
    f.pushed = p->m_tail;
    for (i = 0; i < n; i++) {
        if (ptm_wait_feed(&f, &i) != 0) {
            return i;
        }
        if (i >= n) {
            break;
        }
        if (i < f.ahead) {
            tty_input_looked(&p->slave, bytes[i]);
        } else {
            tty_input_char(&p->slave, bytes[i]);
        }
    }
    wake_master(p, &f.pushed);
    return n;
}

uint32_t pty_kernel_take(struct pty *p, unsigned char *out, uint32_t n)
{
    uint32_t flags = kirq_save();
    uint32_t i, have = pty_m_avail(p);

    if (n > have) {
        n = have;
    }
    for (i = 0; i < n; i++) {
        out[i] = p->m_buf[p->m_head & (PTY_BUF_SIZE - 1)];
        p->m_head++;
    }
    kirq_restore(flags);
    if (n > 0) {
        wq_wake(&p->slave.input_wq, WQ_ROOM);
    }
    return n;
}

int32_t pty_kernel_slave_open(struct pty *p, struct fd_entry *e, int flags)
{
    if (!current_proc) {
        return -ESRCH;
    }
    if (!p->master_open) {
        return -ENXIO;
    }
    pts_bind(p, e, flags);
    return 0;
}

const struct file_ops *pty_slave_ops(void)
{
    return &pts_ops;
}

const char *pty_session_node(int sid)
{
    struct pty *p;

    for (p = sid != 0 ? ptys : NULL; p; p = p->next) {
        if (p->slave.sid == sid && p->knode) {
            return p->knode;
        }
    }
    return NULL;
}

int tty_controlling(const process_t *proc, const struct pidns *ns, uint32_t *dev, int *fg_pgid)
{
    struct pty *p;

    if (tty_is_ctty(&console_tty, proc)) {
        *dev = (5u << 8) | 1u;
        *fg_pgid = (int)tty_fg_seen_in(&console_tty, ns);
        return 1;
    }
    for (p = ptys; p; p = p->next) {
        if (tty_is_ctty(&p->slave, proc)) {
            uint32_t n = (uint32_t)p->index;

            *dev = p->kdev ? p->kdev : (136u << 8) | (n & 0xFFu) | ((n & ~0xFFu) << 12);
            *fg_pgid = (int)tty_fg_seen_in(&p->slave, ns);
            return 1;
        }
    }
    return 0;
}

struct tty *tty_session_tty(int sid)
{
    struct pty *p;

    if (sid == 0) {
        return NULL;
    }
    if (console_tty.sid == sid) {
        return &console_tty;
    }
    for (p = ptys; p; p = p->next) {
        if (p->slave.sid == sid) {
            return &p->slave;
        }
    }
    return NULL;
}

int pty_session_slave(int sid)
{
    struct pty *p = pty_session_pair(sid);

    return p ? p->index : -1;
}

struct pty *pty_session_pair(int sid)
{
    struct pty *p;

    if (sid == 0) {
        return NULL;
    }
    for (p = ptys; p; p = p->next) {
        if (p->slave.sid == sid) {
            return p;
        }
    }
    return NULL;
}

/**
 * The claim and the release are one step to the console's interrupt-side
 * drain, which drains only while console_tty.sid is nonzero. That drain must
 * not run between the claim of the new terminal and the release of the
 * console, or it would drain serial input or deliver a signal for a terminal
 * the session no longer uses. The function is in this file because
 * it reaches both console_tty and the list of ptys.
 */
void tty_make_sole_ctty(struct tty *keep, int sid, int pgid, uint64_t serial)
{
    uint32_t flags;
    struct pty *p;

    if (sid == 0 || !keep) {
        return;
    }
    flags = kirq_save();
    keep->sid = sid;
    keep->fg_pgid = pgid;
    keep->fg_serial = serial;
    if (&console_tty != keep && console_tty.sid == sid) {
        console_tty.sid = 0;
        console_tty.fg_pgid = 0;
        console_tty.fg_serial = 0;
    }
    for (p = ptys; p; p = p->next) {
        if (&p->slave != keep && p->slave.sid == sid) {
            p->slave.sid = 0;
            p->slave.fg_pgid = 0;
            p->slave.fg_serial = 0;
        }
    }
    kirq_restore(flags);
    pid_nums_set(&keep->fg_nums, (uint32_t)pgid);
    if (console_tty.fg_pgid == 0) {
        pid_nums_set(&console_tty.fg_nums, 0);
    }
    for (p = ptys; p; p = p->next) {
        if (p->slave.fg_pgid == 0) {
            pid_nums_set(&p->slave.fg_nums, 0);
        }
    }
}

struct tty *pty_slave_tty(struct fd_entry *e)
{
    if (!e || (e->type != FD_TYPE_PTS && e->type != FD_TYPE_DEVICE) || e->ops != &pts_ops ||
        !e->priv) {
        return NULL;
    }
    return &((struct pty *)e->priv)->slave;
}

int pty_is_allocated(uint32_t inst, int index)
{
    struct pty *p = index < 0 ? NULL : pty_by_index(inst, index);

    return p != NULL && p->master_open && !p->knode;
}

int pty_index_limit(uint32_t inst)
{
    struct pty *p;
    int limit = 0;

    for (p = ptys; p; p = p->next) {
        if (!p->knode && p->inst == inst && p->index + 1 > limit) {
            limit = p->index + 1;
        }
    }
    return limit;
}

struct pty *pty_get(uint32_t inst, int index)
{
    return pty_is_allocated(inst, index) ? pty_by_index(inst, index) : NULL;
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
 * exits, and Linux's disassociate_ctty sends the slave's foreground group
 * SIGHUP then (`tty_disassociate`). A pty has no interrupt-side drain, so
 * the release matters for a later claim of the same slave and for the
 * identity of a later session given the leader's id.
 */
void pty_release_leader(int sid, int pid)
{
    struct pty *p;

    if (sid == 0 || sid != pid) {
        return;
    }
    for (p = ptys; p; p = p->next) {
        if (p->slave.sid == sid) {
            tty_disassociate(&p->slave, 1);
        }
    }
}
