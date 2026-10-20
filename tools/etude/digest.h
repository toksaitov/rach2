/*
 * digest.h: the digests of a manifest's case by which a plan's item names
 * its workload, its assertion inventory and its profile.
 */
#ifndef ETUDE_DIGEST_H
#define ETUDE_DIGEST_H

#include "json.h"

void workload_digest(const struct json *c, const char *architecture, char hex[65]);
void inventory_digest(const struct json *c, char hex[65]);
void profile_digest(const struct json *c, const char *placement, char hex[65]);

#endif
