/* portlock.h: the host lock and the port blocks that runs on one host share. */
#ifndef ETUDE_PORTLOCK_H
#define ETUDE_PORTLOCK_H

/*
 * The protocol is the old runner's, which every checkout on a host takes
 * from the same directory, normally /tmp, whatever its account:
 *
 *   DIR/rach2-host.gate, DIR/rach2-host.lock
 *       the host lock. A run that needs the host alone write-locks the gate
 *       and then the lock, and keeps both; a shared run read-locks the gate
 *       and then the lock, and gives the gate back. A shared run that starts
 *       while one waits for the host alone so waits behind it.
 *   DIR/rach2-ports.N
 *       port block N, write-locked by the one run that forwards the ports of
 *       its table plus N * 1000.
 *
 * The locks are whole-file POSIX record locks on Linux and macOS, and
 * whole-file LockFileEx locks on Windows. The files are created mode 0666
 * and never removed.
 */

/* The highest number of port blocks; the old runner's tables have ten. */
#define ETUDE_PORTLOCK_BLOCKS 10

/* A host lock to take, or none. */
enum etude_portlock_host { ETUDE_PORTLOCK_NONE, ETUDE_PORTLOCK_SHARED, ETUDE_PORTLOCK_ALONE };

/* What a wait is for, which the pause callback is given. */
struct etude_portlock_wait {
    const char *what; /* "host" or "ports" */
    const char *path; /* the file of the host lock, or the blocks' directory */
    int exclusive;    /* the lock waited for is a write lock */
    int count;        /* the number of blocks, for a wait for ports */
    int first;        /* this is the first pause of this wait */
};

/**
 * Pause before the next try of a lock that another process holds.
 *
 * @return 0 to try again, or -1 with errno set to end the take.
 */
typedef int etude_portlock_pause_fn(void *ctx, const struct etude_portlock_wait *w);

/* What to take, and how to wait. */
struct etude_portlock_spec {
    const char *dir; /* the directory of the lock files */
    enum etude_portlock_host host;
    int blocks; /* take the lowest free of this many blocks, or none for 0 */
    etude_portlock_pause_fn *pause;
    void *ctx;
};

/* The locks taken: the gate, the host lock and the block, each open or not. */
struct etude_portlock {
#ifdef _WIN32
    void *file[3]; /* INVALID_HANDLE_VALUE for none */
#else
    int file[3]; /* -1 for none; each is close-on-exec */
#endif
    int block; /* the block taken, or -1 */
};

/**
 * Take the host lock and then a port block, as `spec` asks, into `l`.
 *
 * Each lock is tried at once, and while another process holds it, the take
 * calls `spec->pause` and tries again; a take whose pause returns -1 ends.
 * The host lock is always taken before the block, so that a run that waits
 * for the host waits holding no block. The locks last until
 * etude_portlock_release or the end of the process.
 *
 * @return 0; or -1 with errno set and nothing kept: the pause's error, EINVAL
 *         for a count of blocks outside 0 to ETUDE_PORTLOCK_BLOCKS or a lock
 *         file that is not a regular file, or the error of an open.
 */
int etude_portlock_take(struct etude_portlock *l, const struct etude_portlock_spec *spec);

/** Give back every lock of `l`. */
void etude_portlock_release(struct etude_portlock *l);

#endif
