/* tooling_cache.h: serialize cache references and retire unused machines. */
#ifndef ETUDE_TOOLING_CACHE_H
#define ETUDE_TOOLING_CACHE_H
#include <stdint.h>
#include "etude/tooling.h"
int tooling_cache_enter(const struct etude_tooling_site *site, intptr_t *guard);
void tooling_cache_leave(intptr_t guard);
void tooling_cache_prune(const struct etude_tooling_site *site, const char *current);
#endif
