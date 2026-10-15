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

/* The group of a new slave, `tty`, as Linux's devpts gives it with gid=5. */
#define PTY_GID 5

struct pty {
    struct pty *next; /* the list of live ptys */
    int in_use;
    int index;        /* N in /dev/pts/N */
    struct tty slave; /* the slave end: a full tty + N_TTY discipline */

    unsigned char m_buf[PTY_BUF_SIZE]; /* slave output + echo, read by the master */
    uint32_t m_head;                   /* bytes consumed by the master read end */
    uint32_t m_tail;                   /* bytes produced by the slave sink */
    /* The master's readers, writers and pollers; the slave's space_wq. */
    wait_queue_t m_read_wq;

    int master_open; /* the master descriptor is open */
    int slave_opens; /* count of open slave descriptions (distinct OFDs) */
    int locked;      /* TIOCSPTLCK: 1 until unlockpt() writes 0 */
    int packet;      /* TIOCPKT: each master read begins with a status byte */
    int writing;     /* a slave write is in progress, which no other enters */

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
};

/** Empty the list of live ptys; called once at boot. Each pair is allocated
 * from the heap when its master opens. */
void pty_init_pool(void);

/**
 * open("/dev/ptmx"): allocate a pty pair and install the master end on a
 * fresh descriptor (locked until unlockpt). Returns the master fd (>= 0), or
 * -errno (-ESRCH with no current process, -ENFILE if the heap has no room for
 * a pair, -EMFILE if the fd table is full, -ENOMEM if the heap has no room
 * for the descriptor).
 */
int pty_open_master(int flags);

/**
 * open("/dev/pts/N"): install the slave end of pty N on a fresh descriptor.
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
int pty_open_slave(int index, int flags);

/** Index of the pty whose slave is session `sid`'s controlling terminal, or
 * -1 (always for `sid` 0). /dev/tty opens the caller's controlling pty
 * through this. */
int pty_session_slave(int sid);

/**
 * Make `keep` the sole controlling terminal of session `sid`, with `pgid` in
 * the foreground, and release every other terminal the session controls (the
 * console and any other pty slave), in one interrupts-off region. Does
 * nothing when `sid` is 0 or `keep` is NULL. The caller has authorized the
 * claim: TIOCSCTTY checks leadership and ownership, and the implicit claims
 * by a read or an open require the terminal to be free and the caller a
 * session leader.
 */
void tty_make_sole_ctty(struct tty *keep, int sid, int pgid);

/** The slave tty behind a FD_TYPE_PTS descriptor (for the shared tty ioctls),
 * or NULL if e is not a pty slave. */
struct tty *pty_slave_tty(struct fd_entry *e);

/** The slave index N (in /dev/pts/N) behind a FD_TYPE_PTS descriptor, or -1 if
 * e is not a pty slave; lets /proc/<pid>/fd readlink and fstat report the
 * right name. */
int pty_slave_index(struct fd_entry *e);

/** Whether pty `index` exists and its master is open (for /dev/pts
 * stat/getdents). */
int pty_is_allocated(int index);

/** One past the highest index a live pty has, for a walk of `/dev/pts`. */
int pty_index_limit(void);

/** The pty behind /dev/pts/N when its master is open, or NULL: the ownership
 * fields for the stat/chown/chmod paths. */
struct pty *pty_get(int index);

/**
 * The controlling terminal of the session `sid`: 1 with its device number
 * in `*dev`, in the encoding /proc/PID/stat's tty_nr has, and its
 * foreground process group in `*fg_pgid`, 0 for none; 0 when the session
 * has no terminal. The console is /dev/console, 5:1, and a pty's slave
 * /dev/pts/N, 136:N.
 */
int tty_controlling(int sid, uint32_t *dev, int *fg_pgid);

/**
 * Release every pty slave whose session is `sid` when `sid` is `pid`, a
 * session leader that is ending, so a later session can claim the same
 * /dev/pts/N with TIOCSCTTY instead of finding a dead owner. The exit paths
 * call it beside the console's release.
 */
void pty_release_leader(int sid, int pid);

#endif /* _PTY_H_ */
