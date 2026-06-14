#ifndef _PTY_H_
#define _PTY_H_

#include "../lib/kint.h"
#include "../proc/wait_queue.h"
#include "tty.h"

/*
 * Pseudo-terminals (Phase 19): the mechanism a network login (dropbear) uses to
 * give a remote user a real terminal. A pty is a pair of endpoints:
 *
 *   - the SLAVE is a full tty (a struct tty with the N_TTY line discipline from
 *     Phase 16): a process opens /dev/pts/N, makes it its controlling terminal,
 *     and reads cooked lines / writes output exactly as it would the console;
 *   - the MASTER is what the login server drives: it opens /dev/ptmx, writes the
 *     bytes the remote user typed (which the slave discipline cooks and echoes)
 *     and reads back the slave's output (and that echo).
 *
 * Data flow:
 *   master write(b) -> tty_input_char(&slave, b)   (discipline cooks + echoes)
 *   slave  read     <- the cooked line
 *   slave  write(b) -> tty_write(&slave)           (OPOST/ONLCR output)
 *   master read     <- the slave output + the echo (the m_buf ring below)
 * Both the slave's echo and its output leave through the slave's SINK, which
 * pushes into m_buf; the master reads m_buf. The slave has no polled input source
 * (pump == NULL) -- it is fed by master writes, not a serial port -- so the
 * Phase-18 timer-IRQ console drain never touches a pty.
 *
 * Each endpoint is an fd riding the Phase-12 file_ops layer (ops + priv = the
 * struct pty); read/write block on wait queues, and an endpoint is torn down only
 * when its last descriptor closes. The slave reuses every Phase-16/17 tty service
 * (termios, winsize, ISIG, sessions/job-control) on its struct tty.
 */

#define PTY_MAX      16   /* pty pairs (one per /dev/pts/N, N in 0..15) */
#define PTY_BUF_SIZE 4096 /* slave->master ring; power of two for the mask */

struct pty {
    int in_use;
    int index;                  /* N in /dev/pts/N (the pool slot) */
    struct tty slave;           /* the slave end: a full tty + N_TTY discipline */

    unsigned char m_buf[PTY_BUF_SIZE]; /* slave output + echo, read by the master */
    uint32_t m_head;            /* bytes consumed by the master read end */
    uint32_t m_tail;            /* bytes produced by the slave sink */
    wait_queue_t m_read_wq;     /* master readers blocked for slave output */

    int master_open;            /* the master descriptor is open */
    int slave_opens;            /* count of open slave descriptions (distinct OFDs) */
    int locked;                 /* TIOCSPTLCK: 1 until unlockpt() writes 0 */
};

/* Allocate the pty pool from kernel memory; called once from init. */
void pty_init_pool(void);

/*
 * open("/dev/ptmx"): allocate a free pty pair and install the MASTER endpoint on a
 * fresh descriptor (locked until unlockpt). Returns the master fd (>= 0), or
 * -errno (-ENFILE if the pool is full, -EMFILE if the fd table is full).
 */
int pty_open_master(int flags);

/*
 * open("/dev/pts/N"): install the SLAVE endpoint of pty N on a fresh descriptor.
 * Returns the slave fd (>= 0), or -errno (-ENXIO if N is unallocated, -EIO if the
 * pair is still locked, -EMFILE if the fd table is full).
 */
int pty_open_slave(int index, int flags);

/* The slave tty behind a FD_TYPE_PTS descriptor (for the shared tty ioctls), or
 * NULL if e is not a pty slave. */
struct tty *pty_slave_tty(struct fd_entry *e);

/* The slave index N (in /dev/pts/N) behind a FD_TYPE_PTS descriptor, or -1 if e is
 * not a pty slave -- so /proc/<pid>/fd readlink and fstat report the right name. */
int pty_slave_index(struct fd_entry *e);

/* Whether slot i is an allocated pty pair (for /dev/pts stat/getdents). */
int pty_is_allocated(int index);

/* Disassociate any pty slave controlled by the dying session leader (sid == pid),
 * called beside the console release on the leader-death paths so a stale, dead
 * owner never blocks a later TIOCSCTTY on the same /dev/pts/N. */
void pty_release_leader(int sid, int pid);

#endif /* _PTY_H_ */
