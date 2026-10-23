/* Bounded, identity-aware deltas over consecutive process snapshots. */
#ifndef ETUDE_OBSERVE_SCOPE_H
#define ETUDE_OBSERVE_SCOPE_H
#include "observe_system.h"

struct load_account {
    struct etude_pid root;
    struct load_process *previous;
    size_t count;
    uint64_t outside_us, virtualizer_us, lost, resets, unclassified;
};

void etude_load_account(struct load_account *a, struct load_snapshot *s, struct load_process *p);

#endif
