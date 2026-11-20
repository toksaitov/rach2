/*
 * pty.h: pseudo-terminals, the /dev/ptmx and /dev/pts/N pairs a network login
 * (dropbear) uses to give a remote user a terminal.
 *
 * A pty is a pair of ends:
 *
 *   * the slave is a full tty (a struct tty with the N_TTY line discipline): a
 *     process opens /dev/pts/N, makes it its controlling terminal, and reads
 *     cooked lines and writes output as it would on the console;
 *   * the master is the end the login server holds: it opens /dev/ptmx,
 *     writes the bytes the remote user typed (which the slave discipline cooks
 *     and echoes) and reads back the slave's output (and that echo).
 *
 * Data flow:
 *   master write(b) -> tty_input_char(&slave, b)   (discipline cooks + echoes)
 *   slave  read     <- the cooked line
 *   slave  write(b) -> tty_write(&slave)           (OPOST/ONLCR output)
 *   master read     <- the slave output + the echo (the m_buf ring below)
 * Both the slave's echo and its output leave through the slave's sink, which
 * pushes into m_buf; the master reads m_buf. The slave has no polled input
 * source (pump == NULL); it is fed by master writes, not a serial port, so the
 * console's interrupt-side drain never touches a pty.
 *
 * Each endpoint is a descriptor whose ops are its file_ops and whose priv is
 * the struct pty; read/write block on wait queues, and the pair is freed only
 * when the master and every slave description have closed. The slave reuses
 * every tty service (termios, winsize, ISIG, sessions/job-control) on its
 * struct tty.
 */
#ifndef _PTY_H_
#define _PTY_H_

#include "../lib/kint.h"
#include "../proc/wait_queue.h"
#include "tty.h"

#define PTY_BUF_SIZE 4096 /* slave->master ring; power of two for the mask */

/* The group of a new slave, `tty`, as Linux's devpts gives it with gid=5,
 * where no devpts mount is at /dev/pts. */
#define PTY_GID 5

/* Linux's NR_UNIX98_PTY_MAX: the largest devpts `max=`, and its default. */
#define PTY_MAX_PAIRS (1u << 20)

struct vfs_pts_options;

struct pty {
    struct pty *next; /* the list of live ptys */
    int index;        /* N in /dev/pts/N, unique in its instance */
    /* The devpts instance whose /dev/pts lists the slave, 0 for the
     * system's (`vfs_pts_instance`); a pair of /dev/ptmx takes that of the
     * devpts mount its master was opened through. */
    uint32_t inst;
    /* The st_dev of the slave, its instance's when the pair was made
     * (`vfs_pts_dev`), kept for the pair's life as Linux's pins the
     * instance's superblock, though its mount go. */
    uint32_t dev;
    struct tty slave; /* the slave end: a full tty + N_TTY discipline */

    unsigned char m_buf[PTY_BUF_SIZE]; /* slave output + echo, read by the master */
    uint32_t m_head;                   /* bytes consumed by the master read end */
    uint32_t m_tail;                   /* bytes produced by the slave sink */
    /* The master's readers, writers and pollers; the slave's space_wq. */
    wait_queue_t m_read_wq;

    int master_open;   /* the master descriptor is open */
    int slave_opens;   /* count of open slave descriptions (distinct OFDs) */
    int hung_up;       /* every slave description has closed since one opened */
    int locked;        /* TIOCSPTLCK: 1 until unlockpt() writes 0 */
    int packet;        /* TIOCPKT: each master read begins with a status byte */
    uint8_t pktstatus; /* pending TIOCPKT_* bits for the next packet read */
    int writing;       /* a slave write is in progress, which no other enters */
    int m_writing;     /* a master write is in progress, which no other enters */
    int m_stopped;     /* TCOOFF on the master: its writes wait for TCOON */

    /* Ownership of the slave (/dev/pts/N), the state a login server sets
     * with chown/chmod before handing the terminal to the user. Set at
     * allocation to the opener's effective uid, group PTY_GID and mode 0620;
     * stat reports it. */
    uint32_t uid;
    uint32_t gid;
    uint16_t mode; /* permission bits only; the type is S_IFCHR */
    /* The second the slave last read input and last wrote output, which
     * stat reports as its access and modification times: `w` takes a
     * session's idle time from the first. */
    uint32_t atime;
    uint32_t mtime;
    /* For a pair whose master the kernel holds (`pty_kernel_make`): the
     * slave's node in /dev, as "hvc1", and its device number, major << 8 |
     * minor. NULL and 0 for a pair of /dev/ptmx. */
    const char *knode;
    uint32_t kdev;
};

/* The first index of the pairs whose master the kernel holds, past any
 * /dev/pts/N: they are not listed there and take none of its numbers. */
#define PTY_KERNEL_INDEX 0x100000

/**
 * Parse `options`, mount(2)'s data for devpts or NULL, into `*o`, as
 * Linux's devpts parse_mount_options does: from the defaults, the
 * opener's ids, mode 0600, ptmxmode 0000 and max PTY_MAX_PAIRS, the
 * options `uid=` and `gid=`, numbers the caller's user namespace maps,
 * `mode=` and `ptmxmode=` in octal, cut to 07777, `newinstance`, which
 * every mount is, and `max=` from 0 to PTY_MAX_PAIRS. 0, or -EINVAL with
 * the record `devpts: called with bogus options` for another option or a
 * bad number, with `*o` the defaults and the options before it. A remount
 * parses from the defaults too and takes that `*o` even on -EINVAL, as
 * Linux's devpts_remount does (`pts_remount` in vfs.c).
 */
int pty_parse_options(const char *options, struct vfs_pts_options *o);

/** Empty the list of live ptys; called once at boot. Each pair is allocated
 * from the heap when its master opens. */
void pty_init_pool(void);

/**
 * open("/dev/ptmx"): `pty_open_master_near` of /dev/pts under the
 * caller's root, for an open that knows no place of its ptmx.
 */
int pty_open_master(int flags);

struct vfs_ref;

/**
 * An open of a ptmx device: `pty_open_master_in` in the instance of the
 * devpts mount at `path`, walked from `start` (`vfs_resolve`), the `pts`
 * beside the device the open reached, as Linux's devpts_acquire looks for
 * it (path_pts), with its options; without one, in the system's instance
 * with none.
 */
int pty_open_master_near(const struct vfs_ref *start, const char *path, int flags);

/**
 * Allocate a pty pair in the devpts instance `inst`, at the lowest index
 * it has free, and install the master end on a fresh descriptor (locked
 * until unlockpt), as an open of /dev/ptmx or of the instance's
 * /dev/pts/ptmx does. The slave takes the owner, group and mode of the
 * devpts options `o`, or with NULL the opener's user, group PTY_GID and
 * mode 0620. Returns the master fd (>= 0), or -errno (-ESRCH with no
 * current process, -ENOSPC when the lowest free index is not below `o`'s
 * `max`, -ENFILE if the heap has no room for a pair or PTY_KERNEL_INDEX
 * pairs of the instance are open, -EMFILE if the fd table is full, -ENOMEM
 * if the heap has no room for the descriptor).
 */
int pty_open_master_in(uint32_t inst, const struct vfs_pts_options *o, int flags);

/**
 * open("/dev/pts/N"): install the slave end of pty N of the devpts instance
 * `inst` on a fresh descriptor.
 * Returns the slave fd (>= 0), or -errno (-ESRCH with no current process,
 * -ENXIO if no pty N has its master open, -EIO if the pair is still locked,
 * -EMFILE if the fd table is full, -ENOMEM if the heap has no room for the
 * descriptor). The descriptor has only the access of its open mode, also
 * when /dev/tty resolves to the slave. As on Linux, a session leader
 * without a controlling terminal that opens a slave no session has, without
 * O_NOCTTY, makes it its controlling terminal, with its own group in the
 * foreground; BusyBox's telnetd relies on this, where an ssh server uses
 * TIOCSCTTY.
 */
int pty_open_slave(uint32_t inst, int index, int flags);

/** `pty_open_slave` of the pair `p`, NULL for none, -ENXIO. */
int pty_open_pair_slave(struct pty *p, int flags);

/*
 * A terminal whose master end the kernel holds, as a console driver holds
 * the far end of Linux's hvc terminals: the slave is a pair's slave in
 * every way but its name, a node of /dev the holder registers (devfs.h)
 * with `pty_slave_ops` and an open that calls `pty_kernel_slave_open`, and
 * the holder moves the bytes itself. Each wake of the master's queue,
 * `m_read_wq`, with WQ_DATA says the slave has output or echo to take and
 * with WQ_ROOM that its discipline has room for input again. Every call
 * runs under the big lock in process context.
 */

struct file_ops;

/** Make a pair whose master the kernel holds, unlocked, its slave named
 * `node` in /dev, a string the caller keeps while the pair lives, with the
 * device number `dev`, major << 8 | minor, which /proc reports for a
 * process the slave controls. NULL when the heap has no room. */
struct pty *pty_kernel_make(uint32_t dev, const char *node);

/** The holder lets go of `p` as a master's close does: the slave's
 * foreground group takes SIGHUP, the slave is hung up, and the pair is
 * freed once no slave description is open. The holder does not use `p`
 * after the call. */
void pty_kernel_close(struct pty *p);

/**
 * Feed the `n` bytes at `bytes` to the slave's discipline as a master's
 * write does, without waiting: the bytes it has room for, with a signal
 * character and a VSTOP or VSTART among those that wait acting at once.
 * Returns the bytes taken; the rest are fed again once the master's queue
 * is woken for room.
 */
uint32_t pty_kernel_feed(struct pty *p, const unsigned char *bytes, uint32_t n);

/** Take up to `n` bytes of the slave's output and echo into `out`, and wake
 * a slave writer that waits for room. Returns the bytes taken. */
uint32_t pty_kernel_take(struct pty *p, unsigned char *out, uint32_t n);

/** The open of the slave's node: bind the slave of `p` to the descriptor
 * `e`, being opened with the flags `flags`, as `pty_open_slave` binds one.
 * 0, -ESRCH with no current process, or -ENXIO once the holder let go. */
int32_t pty_kernel_slave_open(struct pty *p, struct fd_entry *e, int flags);

/** The slave's file operations, for the node of a kernel-held pair. */
const struct file_ops *pty_slave_ops(void);

/** The node in /dev of the kernel-held slave that is session `sid`'s
 * controlling terminal, or NULL: /dev/tty opens it by that name. */
const char *pty_session_node(int sid);

struct kernel_iovec;

/** Write an imported vector to the held master descriptor `e`, with the
 * master's write lock held across every element and user copy. `iov` has
 * `count` kernel-owned elements, range-checked and capped at USER_RW_MAX
 * in total by `iov_import`. Returns the bytes fed before a short write or
 * fault, or a negative errno when none were fed, as the master's write
 * does. No bytes returns 0 without taking the lock. May sleep. */
int32_t pty_master_writev(struct fd_entry *e, const struct kernel_iovec *iov, int count);

/** Index of the pty whose slave is session `sid`'s controlling terminal, or
 * -1 (always for `sid` 0). */
int pty_session_slave(int sid);

/** The pair whose slave is session `sid`'s controlling terminal, or NULL
 * (always for `sid` 0). /dev/tty opens the caller's controlling pty
 * through this (`pty_open_pair_slave`), in whichever devpts instance it
 * is. */
struct pty *pty_session_pair(int sid);

/**
 * Make `keep` the sole controlling terminal of session `sid`, with `pgid` of
 * the lifetime `serial` (`pgrp_serial` in process.h) in the foreground, and
 * release every other terminal the session controls (the
 * console and any other pty slave), in one interrupts-off region. Does
 * nothing when `sid` is 0 or `keep` is NULL. The caller has authorized the
 * claim: TIOCSCTTY checks leadership and ownership, and the implicit claims
 * by a read or an open require the terminal to be free and the caller a
 * session leader.
 */
void tty_make_sole_ctty(struct tty *keep, int sid, int pgid, uint64_t serial);

/** The slave tty behind a FD_TYPE_PTS descriptor, or behind a device's
 * whose node is a kernel-held slave (for the shared tty ioctls), or NULL if
 * e is not a pty slave. */
struct tty *pty_slave_tty(struct fd_entry *e);

/** The slave index N (in /dev/pts/N) behind a FD_TYPE_PTS descriptor, or -1 if
 * e is not a pty slave; lets /proc/<pid>/fd readlink and fstat report the
 * right name. */
int pty_slave_index(struct fd_entry *e);

/** Whether pty `index` of the devpts instance `inst` exists and its master
 * is open (for /dev/pts stat/getdents). */
int pty_is_allocated(uint32_t inst, int index);

/** One past the highest index a live pty of the devpts instance `inst` has,
 * for a walk of its `/dev/pts`. */
int pty_index_limit(uint32_t inst);

/** The pty behind /dev/pts/N of the devpts instance `inst` when its master
 * is open, or NULL: the ownership fields for the stat/chown/chmod paths. */
struct pty *pty_get(uint32_t inst, int index);

/**
 * The controlling terminal of the process of `proc` (`tty_is_ctty`): 1 with
 * its device number in `*dev`, in the encoding /proc/PID/stat's tty_nr has,
 * and its foreground process group in `*fg_pgid` as the PID namespace `ns`
 * sees it (`tty_fg_seen_in`), 0 for none; 0 when the process has no
 * controlling terminal. The console is /dev/console, 5:1, and a pty's slave
 * /dev/pts/N, 136:N.
 */
int tty_controlling(const process_t *proc, const struct pidns *ns, uint32_t *dev, int *fg_pgid);

/** The controlling terminal of the session `sid`, the console or a pty's
 * slave, or NULL when the session has none (always for `sid` 0). */
struct tty *tty_session_tty(int sid);

/**
 * Release every pty slave whose session is `sid` when `sid` is `pid`, a
 * session leader that is ending, after SIGHUP to the slave's foreground
 * group (`tty_disassociate`), so a later session can claim the same
 * /dev/pts/N with TIOCSCTTY instead of finding a dead owner. The exit paths
 * call it beside the console's release.
 */
void pty_release_leader(int sid, int pid);

#endif /* _PTY_H_ */
