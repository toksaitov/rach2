/*
 * sched.h: a worker's schedule of jobs on one machine. Jobs of both
 * architectures go into one order, by their preparation and their measured
 * cost, and each is admitted through the machine's ledger before it runs.
 * tools/etude/Readme.md describes it.
 */
#ifndef ETUDE_SCHED_H
#define ETUDE_SCHED_H

#include <stddef.h>
#include <stdint.h>

#include "etude/ledger.h"

/* The policy of a processor reservation from history, and its headroom. */
#define ETUDE_CPU_POLICY           "cpu-median-v1"
#define ETUDE_CPU_HEADROOM_PERCENT 25

/* Where a job of a worker's schedule is. */
enum etude_job_state {
    ETUDE_JOB_PENDING,  /* not admitted, and no request of it waits */
    ETUDE_JOB_WAITING,  /* the worker's one request waits in the ledger */
    ETUDE_JOB_ADMITTED, /* its claim is in the ledger */
    ETUDE_JOB_REFUSED   /* the machine can never hold it; `error` says why */
};

/* One job: an attempt of a case on one architecture. */
struct etude_job {
    const char *id; /* the attempt's identity, which its claim takes */
    const char *case_id;
    const char *arch; /* i686, x86_64 or none */
    unsigned prep;    /* the place of its last preparation in the order of preparation */
    int ready;        /* its preparation has finished */
    uint64_t cost_us; /* its expected time, from compatible history or its initial bound */
    const struct etude_ledger_res *res;
    size_t nres;
    enum etude_job_state state;
    int error; /* ERANGE or ENOSPC, as etude_ledger_admit gave it */
};

/**
 * Give a job's processor reservation, in thousandths of a processor, from
 * the processor time `cpu_us` and the elapsed time `elapsed_us` of the `n`
 * compatible samples of its history.
 *
 * It is the median of each sample's processor time divided by its elapsed
 * time, rounded upward, with ETUDE_CPU_HEADROOM_PERCENT more, at least 1 and
 * at most `ceiling`. A sample whose elapsed time is 0 is left out. With no
 * sample left it is `declared`, the case's first reservation, at most
 * `ceiling`; and `ceiling` when memory runs out.
 */
uint64_t etude_sched_cpu(const uint64_t *cpu_us, const uint64_t *elapsed_us, size_t n,
                         uint64_t declared, uint64_t ceiling);

/**
 * Sort the `n` jobs into the order of a schedule: by the place of their
 * preparation, so that a job whose preparation ends first goes first; then
 * the costlier first, so that a long job does not start last; then by case,
 * architecture and identity. Jobs of the two architectures share one order.
 */
void etude_sched_order(struct etude_job *jobs, size_t n);

/**
 * Ask the ledger `l` to admit each ready job of the `n` in `jobs`, which
 * etude_sched_order has sorted, for account `account` within the machine's
 * limits `cap`.
 *
 * The worker keeps one request waiting in the ledger: the job already
 * waiting, or else the first that is refused for now. The other jobs are
 * asked without waiting, and each is admitted only where the ledger finds it
 * room beside the claims and the protected request, its `cost_us` given as
 * its forecast, as etude_ledger_admit_for says. A job the machine can
 * never hold becomes ETUDE_JOB_REFUSED with the reason in `error`, and the
 * rest go on. `*admitted`, when not NULL, has the count of jobs admitted.
 * The caller runs each job that has become ETUDE_JOB_ADMITTED, ends its
 * claim with etude_ledger_finish, and calls this again when a claim of the
 * machine may have ended.
 *
 * @return 0, or -1 with errno set as etude_ledger_admit gives it for a
 *         ledger that cannot be read or written.
 */
int etude_sched_admit(struct etude_ledger *l, const char *account, struct etude_job *jobs, size_t n,
                      const struct etude_ledger_res *cap, size_t ncap, size_t *admitted);

/**
 * Withdraw the request of the job of `jobs` that waits, as a worker does
 * when it stops; the job becomes ETUDE_JOB_PENDING.
 *
 * @return 0, or -1 with errno set as etude_ledger_withdraw gives it.
 */
int etude_sched_withdraw(struct etude_ledger *l, struct etude_job *jobs, size_t n);

#endif
