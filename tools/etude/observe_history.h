/* Keep complete observation traces with the run that they measured. */
#ifndef ETUDE_OBSERVE_HISTORY_H
#define ETUDE_OBSERVE_HISTORY_H
#include "measurement_context.h"
#include "etude/observe.h"

struct etude_observation_run {
    const struct etude_measurement_context *context;
    struct etude_observer *runtime;
    const char *path, *store, *run_id;
    struct etude_reading start, runtime_start, end;
    struct etude_measurement owned_cpu;
    int physical_required, cleanup_confirmed;
};

/* Stop observers after owned cleanup and retain their verified, bounded traces.
 * The returned JSON is allocated even for incomplete observation evidence. */
int etude_observation_finish(const struct etude_observation_run *run, char **json);
/* Request an outer observer's end on an early refusal as well. */
void etude_observation_cancel(const struct etude_measurement_context *context,
                              const char *run_id);
#endif
