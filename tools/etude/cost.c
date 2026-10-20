/*
 * cost.c: the expected time and the processor reservation of a planned case,
 * from the samples of its history that were measured where it will run.
 */
#include "cost.h"
#include "etude/sched.h"
#include <stdlib.h>
#include <string.h>

/** Give the processor time of a sample's attempt, or -1 when it has none. */
static int64_t cpu_of(const struct cmp_sample *s)
{
    const struct eh_json *cpu = eh_json_get(
        eh_json_get(eh_json_get(s->attempt->data, "resources"), "tree_cpu_us"), "value");
    return cpu && cpu->type == 'n' ? cpu->number : -1;
}

/**
 * Find the cost of the case whose plan gives it `key`, from the samples of
 * `h` that the comparison finds eligible, a pass that was not disrupted nor
 * a retry, with an elapsed time, and that share the first COST_FIELDS
 * values of the key: the case, its workload, inventory and profile, the
 * machine, its system, accelerator, site profile, tools and cache state.
 *
 * The expected time is the median of their elapsed times, or `initial_us`,
 * the case's declared initial bound, when there is none. The reservation is
 * etude_sched_cpu of those with a processor time, a lower bound taken as
 * its value, from `declared_cpu` within `ceiling_cpu`.
 *
 * @return 0, or -1 when memory ran out.
 */
int etude_cost_of(const struct cmp_history *h, const struct cmp_key *key, uint64_t initial_us,
                  uint64_t declared_cpu, uint64_t ceiling_cpu, struct etude_cost *cost)
{
    uint64_t *elapsed, *cpu, *cpu_elapsed;
    struct cmp_stats stats;
    size_t i;
    int64_t c;
    int rc = -1;

    memset(cost, 0, sizeof(*cost));
    elapsed = malloc((h->sample_count + 1) * sizeof(*elapsed));
    cpu = malloc((h->sample_count + 1) * sizeof(*cpu));
    cpu_elapsed = malloc((h->sample_count + 1) * sizeof(*cpu_elapsed));
    if (!elapsed || !cpu || !cpu_elapsed)
        goto out;
    for (i = 0; i < h->sample_count; i++) {
        const struct cmp_sample *s = &h->samples[i];
        if (s->reason[0] || !s->plan || cmp_key_differs(&s->key, key, 0, COST_FIELDS, NULL, 0))
            continue;
        elapsed[cost->samples++] = s->elapsed_us;
        if ((c = cpu_of(s)) >= 0) {
            cpu[cost->cpu_samples] = (uint64_t)c;
            cpu_elapsed[cost->cpu_samples++] = s->elapsed_us;
        }
    }
    cost->elapsed_us = initial_us;
    memset(&stats, 0, sizeof(stats));
    if (cost->samples) {
        if (cmp_statistics(elapsed, cost->samples, &stats))
            goto out;
        cost->elapsed_us = stats.median_us;
    }
    cost->cpu_millis =
        etude_sched_cpu(cpu, cpu_elapsed, cost->cpu_samples, declared_cpu, ceiling_cpu);
    rc = 0;
out:
    free(elapsed);
    free(cpu);
    free(cpu_elapsed);
    return rc;
}
