/*
 * idset.c: a set of small numbers that grows, a bit for each (idset.h).
 */

#include "idset.h"
#include "kerrno.h"
#include "kstring.h"
#include "../mm/kheap.h"

int idset_has(const struct idset *s, uint32_t id)
{
    uint32_t w;

    if (id < 32u) {
        return (s->low >> id) & 1u;
    }
    w = id / 32u - 1u;
    return w < s->words && ((s->more[w] >> (id % 32u)) & 1u);
}

int idset_add(struct idset *s, uint32_t id)
{
    uint32_t w;

    if (idset_has(s, id)) {
        return 0;
    }
    if (id < 32u) {
        s->low |= 1u << id;
        s->count++;
        return 0;
    }
    w = id / 32u - 1u;
    if (w >= s->words) {
        uint32_t words = s->words ? s->words : 1u;
        uint32_t *grown;

        while (words <= w) {
            words *= 2u;
        }
        grown = kzalloc(words * sizeof(*grown));
        if (!grown) {
            return -ENOMEM;
        }
        if (s->more) {
            k_memcpy(grown, s->more, s->words * sizeof(*grown));
            kfree(s->more);
        }
        s->more = grown;
        s->words = words;
    }
    s->more[w] |= 1u << (id % 32u);
    s->count++;
    return 0;
}

void idset_del(struct idset *s, uint32_t id)
{
    if (!idset_has(s, id)) {
        return;
    }
    if (id < 32u) {
        s->low &= ~(1u << id);
    } else {
        s->more[id / 32u - 1u] &= ~(1u << (id % 32u));
    }
    s->count--;
}

uint32_t idset_first_free(const struct idset *s)
{
    uint32_t w, bit, word;

    for (w = 0; w <= s->words; w++) {
        word = w == 0 ? s->low : s->more[w - 1u];
        if (word != 0xFFFFFFFFu) {
            for (bit = 0; (word >> bit) & 1u; bit++) {
            }
            return w * 32u + bit;
        }
    }
    return (s->words + 1u) * 32u;
}

void idset_free(struct idset *s)
{
    kfree(s->more);
    s->low = 0;
    s->more = NULL;
    s->words = 0;
    s->count = 0;
}
