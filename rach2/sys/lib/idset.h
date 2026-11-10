/* idset.h: a set of small numbers that grows, a bit for each. */

#ifndef _IDSET_H_
#define _IDSET_H_

#include "kint.h"

/*
 * A set of small numbers that grows: a bit for each, the numbers under 32 in
 * a word of the set itself and the others in words from the heap, which the
 * set takes as a number above them is added and keeps until it is freed. A
 * zeroed set is empty and needs no allocation; one that has had a number of
 * 32 or more added must be freed with `idset_free`. The mount namespaces'
 * ids are kept in sets of this kind (proc/ns.h).
 */
struct idset {
    uint32_t low;   /* the numbers 0 to 31 */
    uint32_t *more; /* from 32, `words` words from the heap, or NULL */
    uint32_t words;
    uint32_t count; /* the numbers in the set */
};

/** Whether `id` is in `s`. */
int idset_has(const struct idset *s, uint32_t id);

/** Add `id` to `s`: 0, or -ENOMEM when its word cannot be had, which
 * leaves `s` as it was. */
int idset_add(struct idset *s, uint32_t id);

/** Take `id` out of `s`, which keeps its words. */
void idset_del(struct idset *s, uint32_t id);

/** The lowest number not in `s`. */
uint32_t idset_first_free(const struct idset *s);

/** Empty `s` and free its words. */
void idset_free(struct idset *s);

#endif /* _IDSET_H_ */
