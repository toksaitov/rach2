/* tooling_cache.h: serialize cache references and retire unused machines. */
#ifndef ETUDE_TOOLING_CACHE_H
#define ETUDE_TOOLING_CACHE_H
#include <stdint.h>
#include "etude/tooling.h"
/* Give the provisioned shared POSIX cache, or build/tooling. */
const char *tooling_cache_default(void);
/* Serialize one recipe; cancellation or an unsafe mutex returns -1. */
int tooling_cache_prepare_enter(const struct etude_tooling_site *site, const char *key,
                                intptr_t *guard);
int tooling_cache_enter(const struct etude_tooling_site *site, intptr_t *guard);
void tooling_cache_leave(intptr_t guard);
/* Remove completed machines that no key of `keep` names, once keep[0], the
 * machine just made or reused, is complete; `keep` ends with NULL. */
void tooling_cache_prune(const struct etude_tooling_site *site, const char *const *keep);
#endif
