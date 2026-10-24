/* measurement_context.h: verified inputs and same-run preparation timing. */
#ifndef ETUDE_MEASUREMENT_CONTEXT_H
#define ETUDE_MEASUREMENT_CONTEXT_H
#include "include/etude/history.h"
#include <stddef.h>

struct etude_measurement_context;

struct etude_context_clock {
    char machine_id[128], boot_id[128];
};

/* Admission records its first granted reading before waiting for host locks. */
struct etude_preparation_grant {
    struct etude_context_clock clock;
    struct etude_reading start;
};
int etude_context_grant_write(const char *path, const char *claim_id,
                              const struct etude_preparation_grant *grant);
/* Require the exact claim, current machine and boot, and no future reading. */
int etude_context_grant_read(const char *path, const char *claim_id,
                             struct etude_preparation_grant *grant);

/* A receipt is valid only after preparation and owned cleanup succeeded. */
enum etude_preparation_conditioning {
    ETUDE_PREPARATION_UNKNOWN,
    ETUDE_PREPARATION_RETAINED,
    ETUDE_PREPARATION_CLEAN
};
struct etude_preparation_receipt {
    char run_id[256], commit[41], record_sha256[65];
    struct etude_context_clock clock;
    struct etude_reading start, end;
    struct etude_measurement elapsed;
    int passed, cleanup_confirmed, same_run;
    enum etude_preparation_conditioning conditioning;
    /* Empty for ordinary production; otherwise the actual import proof. */
    char import_provenance[4096];
};

/* Use the same platform observer for preparation and runtime clocks. */
int etude_context_clock(struct etude_context_clock *clock);
/* Create once. Never replace a receipt, including one from a failed attempt. */
int etude_context_receipt_write(const char *path, const struct etude_preparation_receipt *receipt);

/* The physical owner supplies the reading at its actual exclusive lease.
 * Route and lease are existing files; completion is a new terminal file. */
struct etude_routine_grant_inputs {
    const char *run_id, *commit, *route, *lease, *completion_path;
    const char *owner_pid, *owner_creation;
    struct etude_context_clock clock;
    struct etude_reading start;
};
int etude_context_routine_grant_write(const char *path,
                                      const struct etude_routine_grant_inputs *inputs);

/* Descriptors bind snapshots to actual bytes. Source inputs are relative to
 * tree; native tool inputs retain their actual absolute paths. */
struct etude_context_inputs {
    const char *tree, *record, *site, *commit, *program;
};
int etude_context_open(const char *path, const struct etude_context_inputs *inputs,
                       struct etude_measurement_context **context, char *error, size_t size);
/* Return canonical JSON for purpose, source, artifacts, site_profile, tools,
 * cache_state, exemption, outer_observation, physical_observation or
 * preparation (the complete verified receipt, or null), cache_evidence,
 * routine_grant or upstream_preparation (original blob references), or
 * routine_proofs (the native-readable resolver path or null). The
 * context owns each immutable fragment; live trace headers are pinned. */
const char *etude_context_text(const struct etude_measurement_context *context, const char *key);
const char *etude_context_run_id(const struct etude_measurement_context *context);
const struct etude_preparation_receipt *
etude_context_preparation(const struct etude_measurement_context *context);
/* Missing or uncontrolled context cannot contribute a performance baseline. */
int etude_context_eligible(const struct etude_measurement_context *context);
/* Recheck the context and every bound file after execution. */
int etude_context_recheck(const struct etude_measurement_context *context, char *error,
                          size_t size);
void etude_context_close(struct etude_measurement_context *context);
#endif
