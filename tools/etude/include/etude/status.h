/*
 * status.h: the status record of a run of etude, which a person or the
 * manager reads through tools/flow/test_status.py: the run's owner and
 * ledger, and of each attempt whether it waits for admission, works within
 * its bounds or has stalled. tools/etude/Readme.md describes the record.
 */
#ifndef ETUDE_STATUS_H
#define ETUDE_STATUS_H

#include <stddef.h>
#include <stdint.h>

#include "etude/exec.h"
#include "etude/watchdog.h"

/* The first line of a status record of this format. */
#define ETUDE_STATUS_MAGIC "etude-status 1"

/*
 * One attempt of a run, as the record gives it. Times are microseconds since
 * the epoch on the system's clock, so that a reader on the same machine can
 * compare them with its own time.
 */
struct etude_status_attempt {
    const char *attempt_id;
    const char *case_id;
    const char *architecture; /* i686, x86_64 or none */
    int waiting;              /* it waits for admission, since since_us */
    uint64_t since_us;        /* when it began to wait */
    enum etude_stage stage;
    int expired;            /* its watchdog ran out, and it is being ended */
    enum etude_expiry kind; /* the bound that runs out next, or that ran out */
    uint64_t progress_us;   /* see etude_status_watch */
    uint64_t deadline_us;   /* when that bound runs out; unused once expired */
    uint64_t bound_us;      /* the length of that bound */
};

/* A run, as the record gives it. */
struct etude_status {
    struct etude_pid owner; /* the process that writes the record */
    const char *run_id;
    const char *ledger; /* the directory of the machine's ledger */
    const char *args;   /* what the run was asked to do, on one line */
    uint64_t written_us;
    const struct etude_status_attempt *attempts;
    size_t nattempts;
};

/**
 * Describe running attempt `a` by its watchdog `w`: its stage, and the bound
 * due next or the one that ran out, as things stand or stood at the expiry.
 * Its progress time is the start of its gap in execution, the later of the
 * last progress and the start of execution, and in any other stage the start
 * of the stage. `now_us` is a time of the monotonic clock the watchdog was
 * given, and `epoch_us` the same instant on the system's clock.
 */
void etude_status_watch(struct etude_status_attempt *a, const struct etude_watchdog *w,
                        uint64_t now_us, uint64_t epoch_us);

/**
 * Write record `s` to `path`, replacing the record there whole: it is written
 * as `path.new` and renamed, so that a reader finds the old record or the
 * new one, never part of one.
 *
 * @return 0, or -1 with errno set: EINVAL for a field that is empty or would
 *         break its line, or an error of the system.
 */
int etude_status_write(const char *path, const struct etude_status *s);

/**
 * Give the time on the system's clock in microseconds since the epoch.
 *
 * @return the time, or 0 when the clock cannot be read.
 */
uint64_t etude_status_now(void);

#endif
