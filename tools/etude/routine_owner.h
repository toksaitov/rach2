/* Physical route proofs added to the existing native sequence owner. */
#ifndef ETUDE_ROUTINE_OWNER_H
#define ETUDE_ROUTINE_OWNER_H
#include "measurement_context.h"
#include "authority.h"
struct etude_routine_owner {
    const char *route, *directory, *run, *commit, *purpose, *trace;
    struct etude_context_clock clock;
    struct etude_reading start, preparation_end, observation_end;
    struct etude_pid owner, collector;
    char mode[24];
    uint64_t work_us;
    int granted, direct_prepared;
};
int routine_owner_open(struct etude_routine_owner *owner, const char *route,
                        const char *directory, const char *run, const char *purpose);
int routine_owner_grant(struct etude_routine_owner *owner);
int routine_owner_expired(const struct etude_routine_owner *owner);
int routine_owner_prepared(struct etude_routine_owner *owner);
int routine_owner_direct_prepared(struct etude_routine_owner *owner);
int routine_owner_observed(struct etude_routine_owner *owner);
int routine_owner_phase(struct etude_routine_owner *owner, const char *name,
                         const struct etude_exec_result *result,
                         const struct etude_reading *start);
int routine_owner_complete(struct etude_routine_owner *owner, int status, int confirmed);
#endif
