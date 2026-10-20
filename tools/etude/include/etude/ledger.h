/*
 * ledger.h: the durable claims of one machine's resources. A worker records
 * a claim before it runs anything, registers each process of the claim
 * before the process runs its program, and ends the claim when the cleanup
 * is confirmed. A claim whose worker has gone is released only when its
 * processes are proved to have ended. tools/etude/Readme.md describes it.
 */
#ifndef ETUDE_LEDGER_H
#define ETUDE_LEDGER_H

#include <stddef.h>
#include <stdint.h>

#include "etude/exec.h"

/* The ledger of one machine, open in one process. */
struct etude_ledger;

/* One resource of a claim, or one limit of the machine's. */
struct etude_ledger_res {
    const char *name; /* letters, digits, `.`, `_`, `:` and `-` */
    uint64_t amount;
};

/* What a claim is, as a reading of the ledger judges it. */
enum etude_claim_status {
    ETUDE_CLAIM_OWNED,    /* its worker runs and answers for it */
    ETUDE_CLAIM_LIVE,     /* its worker has gone and a process of it runs */
    ETUDE_CLAIM_UNCERTAIN /* nothing proves that its processes have ended */
};

/* One claim, as etude_ledger_list gives it; valid during the visit. */
struct etude_ledger_claim {
    const char *id;
    const char *system; /* the operating system that recorded it */
    struct etude_pid owner;
    const char *state; /* as recorded: reserved, running or uncertain */
    enum etude_claim_status status;
    const char *why; /* what keeps a claim that is not owned, or "" */
    const struct etude_ledger_res *res;
    size_t nres;
    size_t nproc; /* the processes registered */
};

/**
 * Open the ledger in directory `dir`, made if it is missing.
 *
 * The handle is used from one thread. A process opens one handle on a
 * ledger, since on POSIX systems the lock of a second handle in the same
 * process does not exclude the first.
 *
 * @return the ledger, or NULL with errno set.
 */
struct etude_ledger *etude_ledger_open(const char *dir);

/** Close the ledger; its claims stay. */
void etude_ledger_close(struct etude_ledger *l);

/**
 * Record claim `id` of the resources `res`, owned by this process, before
 * anything of it runs.
 *
 * The ledger first releases the claims that recovery proves free. With
 * `cap`, it refuses a claim that would take any resource the limits name
 * past its limit, counting every claim it keeps, whatever its status.
 *
 * @return 0 once the claim is durable; -1 with errno EEXIST for an identity
 *         the ledger has, EAGAIN for a claim past a limit, EINVAL for a bad
 *         identity or name, EBADMSG for a ledger that does not parse, or an
 *         error of the system.
 */
int etude_ledger_reserve(struct etude_ledger *l, const char *id, const struct etude_ledger_res *res,
                         size_t nres, const struct etude_ledger_res *cap, size_t ncap);

/**
 * Register the keeper and the process of attempt `attempt_id`, whose claim
 * has the attempt's identity, before the process runs its program.
 *
 * Its type is etude_register_fn, with the ledger as `ctx`, so that the
 * executor calls it between making the process and releasing it. Only the
 * claim's owner registers.
 *
 * @return 0 once the processes are durable in the claim, or an error number:
 *         ENOENT for no such claim, EPERM for a claim of another owner, or
 *         one of etude_ledger_reserve's.
 */
int etude_ledger_register(void *ledger, const char *attempt_id, const struct etude_pid *keeper,
                          const struct etude_pid *process);

/**
 * Register a descendant that attempt `attempt_id` reported, which may outlive
 * the attempt's process.
 *
 * Its type is etude_child_fn, with the ledger as `ctx`.
 *
 * @return 0, or an error number as etude_ledger_register gives.
 */
int etude_ledger_child(void *ledger, const char *attempt_id, const struct etude_pid *child);

/**
 * Write into `buf` the path of the file where the keeper of claim `id`
 * records its cleanup, the `verdict` of the attempt's etude_exec_spec.
 *
 * @return 0, or -1 with errno ERANGE when `size` is too small, or ENOMEM.
 */
int etude_ledger_verdict_path(const struct etude_ledger *l, const char *id, char *buf, size_t size);

/**
 * End claim `id` of this process: release it when `cleanup` is confirmed,
 * and keep it as uncertain when it is not.
 *
 * @return 0; or -1 with errno ENOENT for no such claim, EPERM for a claim of
 *         another owner, EINVAL for a cleanup still pending, or one of
 *         etude_ledger_reserve's.
 */
int etude_ledger_finish(struct etude_ledger *l, const char *id, enum etude_cleanup cleanup);

/**
 * Release every claim that recovery proves free, and give their count in
 * `*freed` when it is not NULL.
 *
 * A claim is free when it was recorded in an earlier boot of the machine,
 * or when its owner has ended and either it registered no process, or each
 * of its processes has ended, its process group has no member, and its
 * keeper confirmed the cleanup. A claim of another operating system, and
 * one that anything leaves in doubt, is kept.
 *
 * @return 0, or -1 with errno set as etude_ledger_reserve gives.
 */
int etude_ledger_recover(struct etude_ledger *l, size_t *freed);

/**
 * Recover, then give each claim kept to `visit`, which returns nonzero to
 * stop.
 *
 * @return 0, the nonzero value `visit` returned, or -1 with errno set.
 */
int etude_ledger_list(struct etude_ledger *l,
                      int (*visit)(const struct etude_ledger_claim *c, void *ctx), void *ctx);

/**
 * Release claim `id`, which is not owned, once a person has established
 * that nothing of it runs.
 *
 * @return 0; or -1 with errno ENOENT for no such claim, EBUSY for a claim
 *         whose owner runs, or one of etude_ledger_reserve's.
 */
int etude_ledger_drop(struct etude_ledger *l, const char *id);

#endif
