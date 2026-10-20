/*
 * cost.h: the measured cost of a planned case, its expected time and its
 * processor reservation, from the compatible samples of its history.
 */
#ifndef ETUDE_COST_H
#define ETUDE_COST_H
#include "compare.h"
#include <stddef.h>
#include <stdint.h>

/* The leading fields of a key that compatible samples share: all but the
 * load, the concurrency policy and the method of the elapsed time. */
#define COST_FIELDS 13

/* What the history gives of a case's cost. */
struct etude_cost {
    size_t samples;      /* the compatible samples */
    size_t cpu_samples;  /* those of them with a processor time */
    uint64_t elapsed_us; /* their median elapsed time, or the initial bound */
    uint64_t cpu_millis; /* the processor reservation by etude_sched_cpu */
};

int etude_cost_of(const struct cmp_history *h, const struct cmp_key *key, uint64_t initial_us,
                  uint64_t declared_cpu, uint64_t ceiling_cpu, struct etude_cost *cost);
#endif
