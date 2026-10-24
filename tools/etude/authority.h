/* Coordinate ordinary admissions of an explicitly enrolled shared ledger. */
#ifndef ETUDE_AUTHORITY_H
#define ETUDE_AUTHORITY_H
#include <stddef.h>
#include <stdint.h>
#include "include/etude/exec.h"
#include "include/etude/history.h"

struct etude_ledger_res;
struct etude_authority;

/* Unenrolled/private ledgers return 0 and NULL. Enrolled ledgers retain a
 * shared policy gate until close; aliases are matched by file identity. */
int etude_authority_open(const char *ledger, struct etude_authority **authority);
int etude_authority_claim(struct etude_authority *authority, const char *id,
                          const struct etude_ledger_res *capacity, size_t count);
void etude_authority_close(struct etude_authority *authority);

/* The owner switches a policy only under its exclusive installed gate.
 * A durable blocked state remains after every uncertain exit. */
struct etude_authority_phase {
    const char *run_id, *commit, *phase, *nonce;
    struct etude_pid owner;
};
int etude_authority_freeze(const struct etude_authority_phase *phase,
                           struct etude_authority **authority);
int etude_authority_allow(struct etude_authority *authority,
                          const struct etude_authority_phase *phase);
int etude_authority_block(struct etude_authority *authority);
int etude_authority_restore(struct etude_authority *authority);
/* Reopen a previously blocked endpoint using its exact protected capability. */
int etude_authority_reopen(const struct etude_authority_phase *phase,
                           struct etude_authority **authority);
int etude_authority_main(int argc, char **argv);
int etude_authority_physical(const struct etude_authority_phase *phase, int release,
                             struct etude_reading *grant);

#endif
