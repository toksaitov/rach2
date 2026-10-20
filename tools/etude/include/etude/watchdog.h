/*
 * watchdog.h: the per-case watchdog. It fixes a case's bounds from its
 * history, tells the case's progress apart from the rest of its output, and
 * says when a bound has run out; on POSIX systems the supervisor runs host
 * attempts and QEMU machines under their watchdogs and ends them when one
 * runs out. tools/etude/Readme.md gives the rules.
 */
#ifndef ETUDE_WATCHDOG_H
#define ETUDE_WATCHDOG_H

#include <stddef.h>
#include <stdint.h>

#include "etude/contract.h"
#include "etude/exec.h"
#include "etude/qemu.h"

/* The policy by which etude_bound_fix fixes a bound from samples. */
#define ETUDE_BOUND_POLICY "watchdog-bounds-v1"

/* The fewest samples a bound is fixed from; with fewer, the initial value. */
#define ETUDE_BOUND_SAMPLES 5

/* The stages a case's watchdog follows, indexed by enum etude_stage. */
#define ETUDE_WATCH_STAGES (ETUDE_STAGE_CLEANUP + 1)

/* A bound as a manifest declares it. */
struct etude_bound {
    uint64_t floor_us;
    uint64_t initial_us;
    uint64_t ceiling_us;
};

/* A bound the plan fixed, and how. */
struct etude_bound_fixed {
    uint64_t us;
    size_t samples;      /* the samples it came from; 0 for the initial value */
    uint64_t largest_us; /* the largest sample; 0 without samples */
    uint64_t spread_us;  /* 3 * 1.4826 times the samples' median absolute
                            deviation, rounded upward */
    int clamped;         /* -1 raised to the floor, 1 cut to the ceiling */
};

/**
 * Fix bound `b` from `n` samples of the same case, workload, profile,
 * machine and accelerator, each the time of a successful attempt or its
 * longest gap between progress records.
 *
 * With fewer than ETUDE_BOUND_SAMPLES samples the bound is the initial
 * value. Otherwise it is twice the sum of the largest sample and the
 * spread. Either is then held between the floor and the ceiling. The caller
 * gives only the samples of attempts that passed undisrupted.
 *
 * @return 0, or EINVAL for a bound whose floor is 0 or whose values are out
 *         of order, EOVERFLOW for samples too large to add, or ENOMEM.
 */
int etude_bound_fix(const struct etude_bound *b, const uint64_t *samples, size_t n,
                    struct etude_bound_fixed *out);

/* A witness of progress, with the bound the plan fixed for a quiet one. */
struct etude_witness_bound {
    const char *witness_id;
    enum etude_witness kind;
    uint64_t limit;    /* 0 for none */
    uint64_t bound_us; /* a quiet witness's; 0 for the others */
};

/* The bounds of one attempt, fixed before it starts. */
struct etude_watchdog_spec {
    uint64_t case_us; /* the whole case, from the watchdog's start */
    uint64_t gap_us;  /* the longest time without progress in execution */
    /* Each other stage's, from its start; 0 for a stage bounded by the case
     * alone. */
    uint64_t stage_us[ETUDE_WATCH_STAGES];
    const struct etude_witness_bound *witnesses;
    size_t witness_count;
};

/* Which bound ran out. */
enum etude_expiry {
    ETUDE_EXPIRY_NONE,
    ETUDE_EXPIRY_CASE,  /* the whole case's */
    ETUDE_EXPIRY_GAP,   /* no progress within the gap bound */
    ETUDE_EXPIRY_QUIET, /* a quiet witness's work did not end within its bound */
    ETUDE_EXPIRY_STAGE  /* a stage other than execution did not end */
};

/* What a watchdog has seen of its case. */
struct etude_progress {
    enum etude_stage stage;
    uint64_t started_us;       /* monotonic times: the watchdog started, */
    uint64_t stage_started_us; /* the stage began, */
    uint64_t last_us;          /* and the gap began: the last progress, or
                                  the start of execution when later */
    const char *last_witness;  /* the witness that advanced last, or NULL */
    uint64_t last_units;
    const char *quiet;    /* the quiet witness whose work runs, or NULL */
    uint64_t quiet_us;    /* when that work was announced */
    uint64_t advances;    /* records that were progress */
    uint64_t repeats;     /* records of a witness at its units, heartbeats */
    uint64_t refused;     /* records of no witness of the case, going
                             back or past a limit, or that do not parse */
    uint64_t bytes;       /* output bytes read */
    uint64_t bytes_since; /* of them, since the last progress */
    uint64_t max_gap_us;  /* the longest gap of execution so far, ended
                             gaps alone */
};

/* The watchdog of one attempt. */
struct etude_watchdog;

/**
 * Make the watchdog of an attempt that begins `stage` at `now_us`, on the
 * monotonic clock. It keeps a pointer to `spec`, which must outlive it.
 *
 * @return the watchdog, or NULL when memory ran out.
 */
struct etude_watchdog *etude_watchdog_new(const struct etude_watchdog_spec *spec,
                                          enum etude_stage stage, uint64_t now_us);

/** Free a watchdog. */
void etude_watchdog_free(struct etude_watchdog *w);

/**
 * Move the attempt to `stage`. Entering execution starts a gap, and leaving
 * it ends one.
 */
void etude_watchdog_stage(struct etude_watchdog *w, enum etude_stage stage, uint64_t now_us);

/**
 * Read `n` bytes of the case's standard output, which carries its report
 * records. Only a record `progress WITNESS UNITS` whose units advance a
 * witness of the case, within its limit, is progress. A record of a quiet
 * witness at its present units announces its work, and until the witness
 * advances its bound replaces the gap bound from that time; a repeated
 * announcement changes nothing.
 */
void etude_watchdog_feed(struct etude_watchdog *w, const char *buf, size_t n, uint64_t now_us);

/**
 * Say which bound has run out by `now_us`, the one that ran out first when
 * several have. The first expiry is kept: its copy of the progress is what
 * etude_watchdog_expired gives, and later calls return it again.
 */
enum etude_expiry etude_watchdog_check(struct etude_watchdog *w, uint64_t now_us);

/** Give the earliest time at which a bound runs out, as things stand. */
uint64_t etude_watchdog_deadline(const struct etude_watchdog *w);

/** Give the bound that etude_watchdog_check reports, or the one now due. */
uint64_t etude_watchdog_bound(const struct etude_watchdog *w);

/** Give what the watchdog has seen so far. */
const struct etude_progress *etude_watchdog_progress(const struct etude_watchdog *w);

/** Give the progress as it stood at the first expiry, or NULL. */
const struct etude_progress *etude_watchdog_expired(const struct etude_watchdog *w);

/**
 * Give the longest gap of execution, counting a gap still open at `now_us`:
 * the history's `max_progress_gap_us`, which includes the first and last.
 */
uint64_t etude_watchdog_max_gap(const struct etude_watchdog *w, uint64_t now_us);

/** Give the name of an expiry: "none", "case", "gap", "quiet" or "stage". */
const char *etude_expiry_name(enum etude_expiry e);

/* ---- The supervisor, on POSIX systems. ---- */

/* The attempts a supervisor runs at once, on a host executor and a QEMU
 * launcher of its own. */
struct etude_supervisor;

/* One attempt under its watchdog, whose spec must outlive it. */
struct etude_supervised;

/* How an attempt is reached. */
enum etude_supervised_kind {
    ETUDE_SUPERVISED_HOST,    /* a host process of the supervisor's executor */
    ETUDE_SUPERVISED_MACHINE, /* a QEMU machine of its launcher */
    ETUDE_SUPERVISED_REMOTE   /* a case on a running Rach2, over a transport
                                 the caller keeps */
};

/* How a case is asked to stop through its transport, as a `cancel` frame to
 * etude-guest asks it. */
struct etude_ask {
    /**
     * Ask the case of `attempt_id` to stop.
     *
     * @return 0 when the request was sent, or an error number.
     */
    int (*fn)(void *ctx, const char *attempt_id);
    void *ctx;
};

/* What a supervisor saved and did when a watchdog ran out, and how the
 * attempt ended. */
struct etude_supervised_result {
    const char *attempt_id;
    enum etude_supervised_kind kind;
    int finished;
    enum etude_end end; /* as observed; LOST when the end was not */
    int value;          /* the exit status or signal, or -1 */
    int incomplete;     /* a watchdog ran out or the transport was lost,
                           so the attempt cannot pass */
    enum etude_expiry expiry;
    const struct etude_progress *progress; /* as it stands */
    const struct etude_progress *expired;  /* as it stood at the expiry */
    uint64_t bound_us;                     /* the bound that ran out */
    /* Saved at the expiry: the processes the supervisor owns, the keeper and
     * the case's process or QEMU's, and a sample of the case's process, or of
     * QEMU's, with the children it has reaped. */
    struct etude_pid owned[2];
    size_t owned_count;
    struct etude_measure sample_cpu_us;
    struct etude_measure sample_rss_bytes;
    char *console_tail; /* a machine's last console lines, or NULL */
    /* A machine's state read through QMP within the capture bound: replies
     * of query-status, query-cpus-fast and `info registers -a`, or NULL. */
    char *qmp_status;
    char *qmp_cpus;
    char *qmp_registers;
    char capture_error[160]; /* what could not be saved, or "" */
    int asked;               /* the case was asked to stop */
    int ask_error;           /* the error of the ask, or 0 */
    enum etude_cleanup cleanup;
    int claim_kept; /* nonzero while the cleanup is not confirmed: the
                       attempt's claims stay reserved */
    uint64_t max_gap_us;
    uint64_t expired_us;                        /* monotonic times: the expiry was seen, */
    uint64_t finished_us;                       /* and the attempt finished */
    const struct etude_exec_result *proc;       /* a host attempt's */
    const struct etude_machine_result *machine; /* a machine's */
};

/** Make a supervisor with no attempts; NULL when memory ran out. */
struct etude_supervisor *etude_supervisor_new(void);

/** End every attempt still running, wait for each, and free all. */
void etude_supervisor_free(struct etude_supervisor *s);

/**
 * Start a host attempt under watchdog `wd`, in execution. Its standard
 * output is read for progress, as far as the executor keeps it. When `wd`
 * runs out, the case is cancelled: SIGTERM, and SIGKILL after the grace.
 *
 * @return 0 with the attempt in `*a`, or -1 with errno set as
 *         etude_exec_start sets it, or ENOMEM.
 */
int etude_supervise_host(struct etude_supervisor *s, const struct etude_exec_spec *spec,
                         const struct etude_watchdog_spec *wd, struct etude_supervised **a);

/**
 * Start a machine under watchdog `wd`, in its boot stage, until the caller
 * moves it to execution. With `console_progress` the console is read for
 * progress, as for a designated init's report. When `wd` runs out, the
 * supervisor reads QEMU's state through QMP within the capture bound, asks
 * the guest to stop through `ask` when it is not NULL and waits the grace
 * for its end, and then stops the machine: QMP's quit, and a kill after the
 * grace.
 *
 * @return 0 with the attempt in `*a`, or -1 with errno set as
 *         etude_qemu_start sets it, or ENOMEM.
 */
int etude_supervise_machine(struct etude_supervisor *s, const struct etude_machine_spec *spec,
                            const struct etude_watchdog_spec *wd, int console_progress,
                            const struct etude_ask *ask, struct etude_supervised **a);

/**
 * Watch a case that runs on a running Rach2, in execution, reached by a
 * transport the caller keeps, which feeds its output and reports its end.
 * When `wd` runs out, `ask` asks the case to stop, and the supervisor waits
 * `grace_us` for its end; an end not reported is lost, and the cleanup
 * unconfirmed.
 *
 * @return 0 with the attempt in `*a`, or -1 with errno ENOMEM.
 */
int etude_supervise_remote(struct etude_supervisor *s, const char *attempt_id,
                           const struct etude_watchdog_spec *wd, uint64_t grace_us,
                           const struct etude_ask *ask, struct etude_supervised **a);

/**
 * Serve every attempt until one finishes, output arrives, or `timeout_ms`
 * passes; -1 sets no time. Each watchdog is checked, and an expiry starts the
 * attempt's end.
 *
 * @return the attempt that finished, each once, or NULL.
 */
struct etude_supervised *etude_supervise_wait(struct etude_supervisor *s, int timeout_ms);

/** Feed `n` bytes of the case's standard output to its watchdog. */
void etude_supervised_feed(struct etude_supervised *a, const char *buf, size_t n);

/** Move the attempt to `stage`, as a machine to execution after its boot. */
void etude_supervised_stage(struct etude_supervised *a, enum etude_stage stage);

/**
 * Report the end of the case behind a machine's guest or a remote
 * transport, and its cleanup: `exit` and `cleaned` of etude-guest. A remote
 * attempt finishes with it.
 */
void etude_supervised_end(struct etude_supervised *a, enum etude_end end, int value,
                          enum etude_cleanup cleanup);

/**
 * Report that a remote attempt's transport was lost. The attempt finishes
 * at once: its end is lost unless it was reported, its cleanup is
 * unconfirmed, and its claims stay reserved.
 */
void etude_supervised_lost(struct etude_supervised *a);

/**
 * Cancel a host attempt that the caller no longer needs, such as a load
 * beside a case that has ended: SIGTERM and SIGCONT, and SIGKILL after the
 * grace. Its watchdog is not consulted again, so the attempt is not made
 * incomplete; its end is stopped unless it had ended. An attempt of another
 * kind, or one whose end has begun, is left as it is.
 */
void etude_supervised_cancel(struct etude_supervised *a);

/** Give what the supervisor observed of an attempt so far. */
const struct etude_supervised_result *etude_supervised_result(const struct etude_supervised *a);

/** Give the machine of a machine attempt, for typing and QMP; else NULL. */
struct etude_machine *etude_supervised_machine(struct etude_supervised *a);

/** Free an attempt that etude_supervise_wait has returned. */
void etude_supervised_free(struct etude_supervised *a);

#endif
