/* Seeded repetitions of the selected native collection. */
#ifndef ETUDE_COLLECTION_REPEAT_H
#define ETUDE_COLLECTION_REPEAT_H
#include "etude/repeat.h"
#define ETUDE_COLLECTION_LOADS 16
struct etude_collection_repeat {
    uint64_t seed;
    unsigned count;
    int seeded;
    size_t nloads;
    struct etude_load loads[ETUDE_COLLECTION_LOADS];
    char names[ETUDE_COLLECTION_LOADS][32];
};
int etude_collection_repeat_option(struct etude_collection_repeat *repeat, int option,
                                   const char *value);
int etude_collection_repeat_valid(const struct etude_collection_repeat *repeat);
void etude_collection_repeat_spec(const struct etude_collection_repeat *repeat,
                                  const struct etude_repeat_case *cases, size_t count,
                                  unsigned width, struct etude_repeat_spec *spec);
#endif
