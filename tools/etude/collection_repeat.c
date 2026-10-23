/* Share the existing seed policy with the public collection allocator. */
#include "collection_repeat.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int number(const char *text, uint64_t *value)
{
    char *end;
    if (!text || *text < '0' || *text > '9')
        return -1;
    errno = 0;
    *value = strtoull(text, &end, 10);
    return errno || *end || *value > ETUDE_REPEAT_SEED_MAX ? -1 : 0;
}

static int load(struct etude_collection_repeat *r, const char *text)
{
    const char *colon = strchr(text, ':');
    uint64_t amount;
    if (!colon || r->nloads == ETUDE_COLLECTION_LOADS || number(colon + 1, &amount) || !amount)
        return -1;
    enum etude_load_kind kind;
    if (colon - text == 3 && !strncmp(text, "cpu", 3) && amount <= 1024)
        kind = ETUDE_LOAD_CPU;
    else if (colon - text == 6 && !strncmp(text, "memory", 6))
        kind = ETUDE_LOAD_MEMORY;
    else if (colon - text == 4 && !strncmp(text, "disk", 4))
        kind = ETUDE_LOAD_DISK;
    else
        return -1;
    size_t at = r->nloads++;
    snprintf(r->names[at], sizeof(r->names[at]), "load-%lu", (unsigned long)at);
    r->loads[at].load_id = r->names[at];
    r->loads[at].kind = kind;
    r->loads[at].amount = amount;
    return 0;
}

int etude_collection_repeat_option(struct etude_collection_repeat *r, int option,
                                  const char *text)
{
    uint64_t value;
    if (option == 'L')
        return load(r, text);
    if (number(text, &value))
        return -1;
    if (option == 'S' && !r->seeded) {
        r->seed = value;
        r->seeded = 1;
        return 0;
    }
    if (option == 'R' && !r->count && value && value <= 1000) {
        r->count = (unsigned)value;
        return 0;
    }
    return -1;
}

int etude_collection_repeat_valid(const struct etude_collection_repeat *r)
{
    if (r->nloads > ETUDE_COLLECTION_LOADS || r->count > 1000 ||
        r->seed > ETUDE_REPEAT_SEED_MAX || ((!r->seeded) && (r->count || r->nloads)))
        return 0;
    for (size_t i = 0; i < r->nloads; i++) {
        const struct etude_load *l = &r->loads[i];
        if (!l->load_id || !*l->load_id || strlen(l->load_id) > 63 ||
            strspn(l->load_id, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != strlen(l->load_id) ||
            l->kind > ETUDE_LOAD_DISK || !l->amount || l->amount > ETUDE_REPEAT_SEED_MAX ||
            (l->kind == ETUDE_LOAD_CPU && l->amount > 1024))
            return 0;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(l->load_id, r->loads[j].load_id)) return 0;
    }
    return 1;
}

void etude_collection_repeat_spec(const struct etude_collection_repeat *r,
                                  const struct etude_repeat_case *cases, size_t count,
                                  unsigned width, struct etude_repeat_spec *s)
{
    memset(s, 0, sizeof(*s));
    s->seed = r->seed;
    s->width = width ? width : 1;
    s->cases = cases;
    s->ncases = count;
    s->loads = r->loads;
    s->nloads = r->nloads;
    s->load_percent = r->nloads ? 100 : 0;
}
