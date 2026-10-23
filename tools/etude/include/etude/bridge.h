/* bridge.h: POSIX host and port locks kept for a durable etude claim. */
#ifndef ETUDE_BRIDGE_H
#define ETUDE_BRIDGE_H

struct etude_bridge;

/* One claim's compatibility locks, acquired before its execution starts. */
struct etude_bridge_spec {
    const char *ledger; /* absolute path of the shared ledger */
    const char *id;
    const char *dir; /* absolute path of the old runner's lock directory */
    int alone;
    int legacy;       /* take a legacy port block as well as the host lock */
    unsigned wait_ms; /* acquisition bound, zero for one try */
};

/**
 * Take the old runner's host lock for reserved claim `id` in `ledger`.
 *
 * The caller owns the claim and has registered no execution yet. `alone`
 * takes the host exclusively; `legacy` also takes one of its ten port
 * blocks. All accounts use the same absolute `dir`, normally /tmp.
 * `wait_ms` bounds acquisition, with zero for one try. No execution may
 * start before this returns. Each claim identity is used only once.
 *
 * A separate process holds the locks until ledger recovery proves the
 * claim free, including after the caller dies. An unreadable ledger or
 * uncertain cleanup keeps the locks. Do not kill that process to recover
 * capacity: establish cleanup and release the claim through the ledger.
 *
 * @return the holder, or NULL with errno set; EAGAIN for occupied locks
 *         with no wait, ETIMEDOUT after the bound, EPERM for a claim the
 *         caller does not own, and EINVAL for one already executing.
 */
struct etude_bridge *etude_bridge_start(const struct etude_bridge_spec *spec);

/* Keep collection locks until ID and all PREFIX: claims are gone. */
struct etude_bridge *etude_bridge_collection_start(const struct etude_bridge_spec *spec,
                                                   const char *prefix);

/** Give the legacy port offset, or -1 when no block was requested. */
int etude_bridge_offset(const struct etude_bridge *b);

/** Give the lock holder's PID for diagnostics. */
long etude_bridge_pid(const struct etude_bridge *b);

/** Close the parent's handle while the holder keeps an uncertain claim. */
void etude_bridge_detach(struct etude_bridge *b);

/**
 * Reap the holder and free `b` after the claim has been released.
 * This waits while the ledger still reserves the claim, or is unreadable.
 */
void etude_bridge_join(struct etude_bridge *b);

/**
 * Say whether a native TCP or UDP port avoids all legacy port blocks.
 * Legacy tables, including both NICs, use ports below 25000 after offsets.
 * Native allocators choose and bind ports from 25000 through 65535 while
 * holding their ordinary etude reservation.
 */
int etude_bridge_native_port(unsigned port);

#endif
