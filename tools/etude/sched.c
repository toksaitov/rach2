/*
 * sched.c: a worker's schedule of jobs, both architectures in one order, and
 * their admission through the machine's ledger.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "etude/sched.h"

/** Order two values of a sample's ratio, smaller first. */
static int ascending(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

    return (x > y) - (x < y);
}

/** Give `cpu` thousandths of `elapsed`, rounded upward, or UINT64_MAX. */
static uint64_t ratio_millis(uint64_t cpu, uint64_t elapsed)
{
    uint64_t whole = cpu / elapsed, part = cpu % elapsed;

    if (whole > UINT64_MAX / 1000u || part > UINT64_MAX / 1000u)
        return UINT64_MAX;
    whole *= 1000u;
    part = (part * 1000u + elapsed - 1) / elapsed;
    return whole > UINT64_MAX - part ? UINT64_MAX : whole + part;
}

uint64_t etude_sched_cpu(const uint64_t *cpu_us, const uint64_t *elapsed_us, size_t n,
                         uint64_t declared, uint64_t ceiling)
{
    uint64_t *r, median, more;
    size_t i, m = 0;

    r = malloc((n > 0 ? n : 1) * sizeof(*r));
    if (r == NULL)
        return ceiling;
    for (i = 0; i < n; i++)
        if (elapsed_us[i] > 0)
            r[m++] = ratio_millis(cpu_us[i], elapsed_us[i]);
    if (m == 0) {
        free(r);
        return declared < ceiling ? declared : ceiling;
    }
    qsort(r, m, sizeof(*r), ascending);
    median = m % 2 ? r[m / 2]
                   : r[m / 2 - 1] / 2 + r[m / 2] / 2 + (r[m / 2 - 1] % 2 + r[m / 2] % 2 + 1) / 2;
    free(r);
    more = median / 100u * ETUDE_CPU_HEADROOM_PERCENT +
           (median % 100u * ETUDE_CPU_HEADROOM_PERCENT + 99u) / 100u;
    median = median > UINT64_MAX - more ? UINT64_MAX : median + more;
    if (median < 1)
        median = 1;
    return median < ceiling ? median : ceiling;
}

/** Compare two strings that may be NULL, NULL first. */
static int text_cmp(const char *a, const char *b)
{
    if (a == NULL || b == NULL)
        return (a != NULL) - (b != NULL);
    return strcmp(a, b);
}

/** Order two jobs as etude_sched_order says. */
static int job_cmp(const void *pa, const void *pb)
{
    const struct etude_job *a = pa, *b = pb;
    int c;

    if (a->prep != b->prep)
        return a->prep < b->prep ? -1 : 1;
    if (a->cost_us != b->cost_us)
        return a->cost_us > b->cost_us ? -1 : 1;
    if ((c = text_cmp(a->case_id, b->case_id)) != 0 || (c = text_cmp(a->arch, b->arch)) != 0)
        return c;
    return text_cmp(a->id, b->id);
}

void etude_sched_order(struct etude_job *jobs, size_t n)
{
    if (n > 1)
        qsort(jobs, n, sizeof(*jobs), job_cmp);
}

int etude_sched_admit(struct etude_ledger *l, const char *account, struct etude_job *jobs, size_t n,
                      const struct etude_ledger_res *cap, size_t ncap, size_t *admitted)
{
    size_t i, count = 0;
    int have = 0, wait;

    for (i = 0; i < n; i++)
        if (jobs[i].state == ETUDE_JOB_WAITING)
            have = 1;
    for (i = 0; i < n; i++) {
        struct etude_job *j = &jobs[i];

        if (!j->ready || (j->state != ETUDE_JOB_PENDING && j->state != ETUDE_JOB_WAITING))
            continue;
        wait = have ? j->state == ETUDE_JOB_WAITING : 1;
        if (etude_ledger_admit_for(l, j->id, account, j->res, j->nres, cap, ncap, wait,
                                   j->forecast_us) == 0) {
            if (j->state == ETUDE_JOB_WAITING)
                have = 0;
            j->state = ETUDE_JOB_ADMITTED;
            count++;
        } else if (errno == EAGAIN) {
            j->state = wait ? ETUDE_JOB_WAITING : ETUDE_JOB_PENDING;
            have |= wait;
        } else if (errno == ERANGE || errno == ENOSPC) {
            if (j->state == ETUDE_JOB_WAITING)
                have = 0;
            j->state = ETUDE_JOB_REFUSED;
            j->error = errno;
        } else {
            return -1;
        }
    }
    if (admitted != NULL)
        *admitted = count;
    return 0;
}

int etude_sched_withdraw(struct etude_ledger *l, struct etude_job *jobs, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        if (jobs[i].state == ETUDE_JOB_WAITING) {
            if (etude_ledger_withdraw(l, jobs[i].id) != 0 && errno != ENOENT)
                return -1;
            jobs[i].state = ETUDE_JOB_PENDING;
        }
    return 0;
}
