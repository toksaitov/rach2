/* measurement_context.h: verified inputs and same-run preparation timing. */
#ifndef ETUDE_MEASUREMENT_CONTEXT_H
#define ETUDE_MEASUREMENT_CONTEXT_H
#include "include/etude/history.h"
#include <stddef.h>

struct etude_measurement_context;

struct etude_context_clock {
    char machine_id[128], boot_id[128];
};

/* A receipt is valid only after preparation and owned cleanup succeeded. */
struct etude_preparation_receipt {
    char run_id[256], commit[41], record_sha256[65];
    struct etude_context_clock clock;
    struct etude_reading start, end;
    struct etude_measurement elapsed;
    int passed, cleanup_confirmed, same_run;
};

/* Use the same platform observer for preparation and runtime clocks. */
int etude_context_clock(struct etude_context_clock *clock);
/* Create once. Never replace a receipt, including one from a failed attempt. */
int etude_context_receipt_write(const char *path,
                                const struct etude_preparation_receipt *receipt);

/* Descriptors bind snapshots to actual bytes. Source inputs are relative to
 * tree; native tool inputs retain their actual absolute paths. */
int etude_context_open(const char *path, const char *tree, const char *record,
                       const char *site, const char *commit, const char *program,
                       struct etude_measurement_context **context, char *error, size_t size);
/* Return canonical JSON for purpose, source, artifacts, site_profile, tools,
 * cache_state or exemption. The context owns each immutable fragment. */
const char *etude_context_text(const struct etude_measurement_context *context, const char *key);
const char *etude_context_run_id(const struct etude_measurement_context *context);
const struct etude_preparation_receipt *
etude_context_preparation(const struct etude_measurement_context *context);
/* Missing or uncontrolled context cannot contribute a performance baseline. */
int etude_context_eligible(const struct etude_measurement_context *context);
/* Recheck the context and every bound file after execution. */
int etude_context_recheck(struct etude_measurement_context *context, char *error, size_t size);
void etude_context_close(struct etude_measurement_context *context);
#endif
