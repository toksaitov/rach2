/*
 * cost.h: the measured cost of a planned case, its expected time and its
 * processor reservation, from the compatible samples of its history.
 */
#ifndef ETUDE_COST_H
#define ETUDE_COST_H
#include "compare.h"
#include "json.h"
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

/* The processors a light guest claims, whatever its count, in thousandths. */
#define ETUDE_CPU_LIGHT_MILLIS 2000

int etude_cost_of(const struct cmp_history *h, const struct cmp_key *key, uint64_t initial_us,
                  uint64_t declared_cpu, uint64_t ceiling_cpu, struct etude_cost *cost);

/**
 * Give the processor claim of a guest of `processors` processors whose
 * processor class is `cpu_class`: `bound` claims every processor, a thousand
 * thousandths each, and `light` claims ETUDE_CPU_LIGHT_MILLIS, or every
 * processor of a smaller guest.
 *
 * @return 0 with `*millis` set; -1 with errno EINVAL for another class or
 *         no processor.
 */
int etude_cpu_class(const char *cpu_class, uint64_t processors, uint64_t *millis);

/**
 * Put the processor claim of case `c`'s class in place of the reservation
 * that history gave `cost`, when the case's `resources` name a `cpu_class`;
 * the claim sums the class reservation of each QEMU profile. A class fixes
 * the claim, since a guest that runs a process on each CPU uses them all
 * whatever an earlier run measured, and a performance run needs them free.
 *
 * @return 1 when the class set the claim, 0 when the case has none, and -1
 *         with errno EINVAL for a class without a QEMU profile.
 */
int etude_cost_class(const struct json *c, struct etude_cost *cost);
#endif
