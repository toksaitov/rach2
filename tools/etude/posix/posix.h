/*
 * posix.h: the parts of the POSIX executor that its controller side, its
 * keeper and its process helpers share.
 *
 * Each attempt has a keeper: a process the controller forks, which makes the
 * case's process, holds it until the controller has registered it, reaps
 * it, ends what it leaves and measures the tree. The two talk over a pair of
 * pipes in fixed messages of fewer than PIPE_BUF bytes, so that each write
 * is whole.
 */
#ifndef ETUDE_POSIX_H
#define ETUDE_POSIX_H

#include <stdint.h>

#include "etude/exec.h"

/* The messages between the controller and a keeper. */
enum keeper_type {
    /* From the keeper. */
    KEEP_STARTED, /* a, b, c: the case's pid, start and whether it is known */
    KEEP_EXEC,    /* a: 0, or the error of the exec */
    KEEP_EXIT,    /* a: ETUDE_END_EXITED or ETUDE_END_SIGNALED; b: the value */
    KEEP_USAGE,   /* a: processor microseconds; b: 1 when exact; c: peak bytes */
    KEEP_CLEANED, /* a: 1 when every process is confirmed ended */
    KEEP_STOPPED, /* a: when the case's process was seen to stop */
    KEEP_RESUMED, /* a: when it was seen to continue, or was let go */
    /* From the controller. */
    KEEP_GO,     /* let the case's process execute its program */
    KEEP_CANCEL, /* end the case: SIGTERM and SIGCONT, SIGKILL after the grace */
    KEEP_CHILD,  /* a, b: a reported descendant's pid and start */
    KEEP_CLEAN,  /* the controller has read the output; end what is left */
    KEEP_STOP,   /* send SIGSTOP to the case's group */
    KEEP_CONT    /* send SIGCONT to the case's group */
};

struct keeper_msg {
    int32_t type;
    int32_t pad;
    int64_t a, b, c;
};

/* The descriptors and the attempt a keeper starts with. */
struct keeper_args {
    const struct etude_exec_spec *spec;
    int ctl; /* the controller's messages */
    int ev;  /* the keeper's messages */
    int in;  /* the case's standard input */
    int out; /* the write ends of the case's standard output and error */
    int err;
    int out_r; /* their read ends, which the keeper reads only once the */
    int err_r; /* controller has gone */
};

/** Run the keeper of one attempt in the process just forked; never returns. */
void keeper_main(const struct keeper_args *k);

/** Write one message; 0 on success, -1 when the reader has gone. */
int keeper_send(int fd, int type, int64_t a, int64_t b, int64_t c);

/** Read one whole message, waiting for it; -1 at the writer's end. */
int keeper_recv(int fd, struct keeper_msg *m);

/** Give the monotonic clock in microseconds. */
uint64_t posix_now_us(void);

/** Close every descriptor of the process except the `n` in `keep`. */
void posix_close_others(const int *keep, int n);

/**
 * Clear GitHub Actions' process marker in a forked cleanup supervisor.
 * Its orphan sweep must leave this process alive to confirm cleanup.
 * Overwrite the inherited bytes: unsetenv alone leaves Linux's
 * /proc/PID/environ unchanged. Call after forking any tracked case.
 */
void posix_untrack_runner(void);

/**
 * Move descriptor `fd` to one numbered 3 or above, closed on exec.
 *
 * @return the new descriptor, or -1.
 */
int posix_fd_high(int fd);

/**
 * Give the PIDs of this process's children, from Linux's record of them.
 *
 * @return the count found, at most `max`, or -1 where the system keeps no
 *         such record.
 */
int posix_children(long *pids, int max);

/** Ask to be the reaper of every orphan below this process; 0 on success. */
int posix_subreaper(void);

/**
 * Sample process `id`: the processor time of it and of the children it has
 * reaped, in microseconds, and its resident memory.
 *
 * @return 1 with the sample; 0 when it has ended or its PID is another
 *         process's; -1 where the system gives no sample.
 */
int posix_usage(const struct etude_pid *id, uint64_t *cpu_us, uint64_t *rss_bytes);

#endif
