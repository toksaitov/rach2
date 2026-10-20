/*
 * exec.h: the executor of host processes. It starts a case's program as a
 * process registered before it runs, captures its output, cancels it, ends
 * the descendants it leaves, and gives its end and the processor time and
 * memory of its tree. tools/etude/Readme.md describes the POSIX adapter.
 */
#ifndef ETUDE_EXEC_H
#define ETUDE_EXEC_H

#include <stddef.h>
#include <stdint.h>

#include "etude/contract.h"

/* A process by its identity, so that a reused PID is told apart. */
struct etude_pid {
    long pid;
    uint64_t start;  /* when it started, in the platform's units: Linux's
                        clock ticks since boot, macOS's microseconds since
                        the epoch, Windows's 100 ns units since 1601 */
    int start_known; /* 0 when the platform gives no start time */
};

/* How far a measurement can be trusted, as the history's `quality` says. */
enum etude_quality {
    ETUDE_QUALITY_UNAVAILABLE, /* no value; `reason` says why */
    ETUDE_QUALITY_EXACT,       /* complete accounting at the method's resolution */
    ETUDE_QUALITY_LOWER_BOUND  /* some of the cost was not observed */
};

/* One measurement of a case's resources. */
struct etude_measure {
    enum etude_quality quality;
    uint64_t value;
    const char *method; /* the method's identity, or the reason none applied */
};

/* What is known of the cleanup of a case's processes. */
enum etude_cleanup {
    ETUDE_CLEANUP_PENDING,    /* the case has not been cleaned up yet */
    ETUDE_CLEANUP_CONFIRMED,  /* every process of the case is known to have ended */
    ETUDE_CLEANUP_UNCONFIRMED /* some may still run, and their claims stay */
};

/**
 * The registration of a case's process, called after the process exists and
 * before it executes its program.
 *
 * `keeper` is the executor's own process that supervises the case, and
 * `process` the case's. The function returns 0 to let the program run, or an
 * error number, which ends the process unrun and becomes the error of
 * etude_exec_start.
 */
typedef int (*etude_register_fn)(void *ctx, const char *attempt_id, const struct etude_pid *keeper,
                                 const struct etude_pid *process);

/**
 * The registration of a descendant that a case reported in a `child` record
 * and the executor found running, called before the executor ends it.
 *
 * @return 0, or an error number, after which the attempt's cleanup is
 *         unconfirmed, since the descendant is in no record.
 */
typedef int (*etude_child_fn)(void *ctx, const char *attempt_id, const struct etude_pid *child);

/* What the file `verdict` holds when a keeper confirmed its cleanup. */
#define ETUDE_VERDICT_CONFIRMED "confirmed\n"

/* What it holds when the keeper could not confirm the cleanup. */
#define ETUDE_VERDICT_UNCONFIRMED "unconfirmed\n"

/* One attempt to run, as the plan fixed it. */
struct etude_exec_spec {
    const char *attempt_id;
    char *const *argv;      /* argv[0] is the program's path */
    char *const *envp;      /* the whole environment */
    const char *cwd;        /* NULL keeps the controller's */
    const char *stdin_path; /* NULL gives /dev/null */
    size_t output_max;      /* bytes kept of each of standard output and error */
    uint64_t grace_us;      /* from the termination signal to the kill */
    etude_register_fn reg;  /* NULL registers nothing */
    etude_child_fn child;   /* NULL registers no descendant */
    void *reg_ctx;          /* the context of `reg` and `child` */
    int posix_only;         /* nonzero: use no interface beyond POSIX, as on a
                               system without a subreaper, as the tests do on Linux */
    const char *verdict;    /* where a keeper whose controller has gone records
                               its cleanup, or NULL; Windows, which has no
                               keeper, writes none */
};

/* One output stream of a case. */
struct etude_output {
    char *data; /* the first `kept` bytes, followed by a NUL */
    size_t kept;
    uint64_t total; /* bytes the stream carried, kept or not */
    int closed;     /* nonzero once the stream reached its end */
};

/* What the executor observed of one attempt. */
struct etude_exec_result {
    const char *attempt_id;
    struct etude_pid keeper;
    struct etude_pid process;
    enum etude_end end;      /* EXITED, SIGNALED, STOPPED after a cancellation,
                                or LOST when the keeper ended unexpectedly */
    int value;               /* the exit status or signal; -1 for STOPPED and LOST */
    enum etude_end exit_end; /* how the process itself ended: EXITED,
                                SIGNALED, or LOST when that was not seen */
    int exit_value;
    struct etude_output out;
    struct etude_output err;
    unsigned children;               /* `child` records naming a running descendant */
    unsigned foreign;                /* `child` records naming another process */
    struct etude_measure cpu_us;     /* processor time of the whole tree */
    struct etude_measure peak_bytes; /* resident memory at the tree's peak */
    enum etude_cleanup cleanup;
    uint64_t started_us;  /* monotonic times: the start was asked, */
    uint64_t ended_us;    /* the end of the process was seen, */
    uint64_t finished_us; /* and the cleanup was done */
};

/* The executor: the processes of the cases it runs at once. */
struct etude_exec;

/* One attempt that the executor runs. */
struct etude_proc;

/**
 * Make an executor with no processes.
 *
 * The executor sets SIGPIPE ignored in the controller, so that the end of a
 * keeper cannot end the controller; its cases start with every signal at its
 * default. It is used from one thread.
 *
 * @return the executor, or NULL when memory ran out.
 */
struct etude_exec *etude_exec_new(void);

/**
 * Cancel every case still running, wait until each is cleaned up, and free
 * the executor and every attempt.
 */
void etude_exec_free(struct etude_exec *ex);

/**
 * Start an attempt.
 *
 * The case's process is made in a session of its own, with its standard
 * input from `stdin_path` and its output to pipes the executor reads, no
 * other descriptor, an empty signal mask and every signal at its default. It
 * executes its program only after `reg` has accepted it.
 *
 * @return 0 with the attempt in `*p`, or -1 with errno set: an error of the
 *         system, the error `reg` returned, or the error of the program's
 *         exec, after which nothing of the attempt is left.
 */
int etude_exec_start(struct etude_exec *ex, const struct etude_exec_spec *spec,
                     struct etude_proc **p);

/**
 * Serve every running attempt until one finishes, new output arrives, or
 * `timeout_ms` milliseconds pass; -1 sets no time.
 *
 * An attempt finishes when its process has ended, the executor has ended
 * the descendants it knows of, and the cleanup is confirmed or found
 * uncertain. A caller that watches the output, as a watchdog does, reads
 * the results of the running attempts after each return.
 *
 * @return the attempt that finished, each once, or NULL when output arrived,
 *         the time passed, or no attempt is running.
 */
struct etude_proc *etude_exec_wait(struct etude_exec *ex, int timeout_ms);

/* A descriptor of the caller's that etude_exec_watch waits on beside the
 * attempts' own: a file descriptor or a socket on POSIX systems. */
struct etude_watch {
    int fd;
    int events;  /* ETUDE_WATCH_IN, ETUDE_WATCH_OUT or both */
    int revents; /* what was found ready, with ETUDE_WATCH_END for a
                    closed or failed descriptor */
};

#define ETUDE_WATCH_IN  1
#define ETUDE_WATCH_OUT 2
#define ETUDE_WATCH_END 4

/**
 * Wait as etude_exec_wait does, and also for one of the `n` descriptors of
 * `w` to be ready, which sets its `revents` and returns NULL.
 *
 * Each call serves the attempts at least once, so that a `timeout_ms` of 0
 * reads what is ready and returns at once. Only the POSIX executor has it,
 * for the QEMU launcher, which does not yet run on Windows.
 */
struct etude_proc *etude_exec_watch(struct etude_exec *ex, int timeout_ms, struct etude_watch *w,
                                    size_t n);

/**
 * Cancel an attempt: its process group and the descendants it reported get
 * SIGTERM and SIGCONT, and SIGKILL after the grace period. Its end becomes
 * ETUDE_END_STOPPED, unless the process had already ended.
 */
void etude_exec_cancel(struct etude_proc *p);

/** Give what was observed of an attempt so far. */
const struct etude_exec_result *etude_proc_result(const struct etude_proc *p);

/** Free an attempt that etude_exec_wait has returned. */
void etude_proc_free(struct etude_proc *p);

/**
 * Read the state of process `pid`.
 *
 * @return 1 when it runs, with its identity in `*id`; 0 when it has ended
 *         or is a zombie; -1 when the platform cannot tell.
 */
int etude_process_state(long pid, struct etude_pid *id);

#endif
