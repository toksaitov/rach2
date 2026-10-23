/* Process deltas are lower bounds; disappearance is never measured as zero. */
#include <stdlib.h>
#include <string.h>
#include "observe_scope.h"
#include "json.h"

static int order(const void *a, const void *b)
{
    long x = ((const struct load_process *)a)->id.pid;
    long y = ((const struct load_process *)b)->id.pid;
    return (x > y) - (x < y);
}

static struct load_process *find(struct load_process *p, size_t n, long pid)
{
    struct load_process key;
    memset(&key, 0, sizeof key);
    key.id.pid = pid;
    return bsearch(&key, p, n, sizeof *p, order);
}

static int same(const struct etude_pid *a, const struct etude_pid *b)
{
    return a->pid == b->pid && a->start_known && b->start_known && a->start == b->start;
}

/* Owned identities remain owned after reparenting. A missing parent does not
 * prove that a new process is outside the collection. */
static void scopes(struct load_account *a, const struct load_snapshot *s, struct load_process *p)
{
    for (size_t i = 0; i < s->count; i++) {
        struct load_process *old = find(a->previous, a->count, p[i].id.pid);
        if (same(&a->root, &p[i].id) || (old && same(&old->id, &p[i].id) && old->scope == 1))
            p[i].scope = 1;
        else if (old && same(&old->id, &p[i].id))
            p[i].scope = old->scope;
        else if (!a->count && p[i].parent <= 1 && !p[i].scope)
            p[i].scope = 2;
    }
    for (unsigned depth = 0; depth < 128; depth++) {
        int changed = 0;
        for (size_t i = 0; i < s->count; i++) {
            if (p[i].scope)
                continue;
            struct load_process *parent = find(p, s->count, p[i].parent);
            if (parent && parent->scope && parent->id.start <= p[i].id.start) {
                p[i].scope = parent->scope;
                changed = 1;
            }
        }
        if (!changed)
            break;
    }
}

void etude_load_account(struct load_account *a, struct load_snapshot *s, struct load_process *p)
{
    qsort(p, s->count, sizeof *p, order);
    scopes(a, s, p);
    for (size_t i = 0; i < s->count; i++) {
        struct load_process *old = find(a->previous, a->count, p[i].id.pid);
        if (!p[i].scope)
            a->unclassified++;
        if (!old || !same(&old->id, &p[i].id))
            continue;
        if (p[i].cpu_us < old->cpu_us) {
            a->resets++;
            continue;
        }
        uint64_t delta = p[i].cpu_us - old->cpu_us;
        uint64_t *total = NULL;
        if (p[i].scope == old->scope) {
            if (p[i].scope == 2)
                total = &a->outside_us;
            else if (p[i].scope == 3)
                total = &a->virtualizer_us;
        }
        if (total) {
            if (delta > (uint64_t)JSON_INTEGER_MAX - *total)
                s->overflow = 1;
            else
                *total += delta;
        }
    }
    for (size_t i = 0; i < a->count; i++) {
        struct load_process *now = find(p, s->count, a->previous[i].id.pid);
        if (!now || !same(&now->id, &a->previous[i].id))
            a->lost++;
    }
    memcpy(a->previous, p, s->count * sizeof *p);
    a->count = s->count;
}
