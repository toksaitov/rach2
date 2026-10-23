/* Native history uses the common validated streams, blobs and bound policy. */
#ifndef NATIVE_HISTORY_H
#define NATIVE_HISTORY_H
#include "collection.h"
#include "compare.h"
#include "etude/history.h"
#include "etude/watchdog.h"
struct nc_measure {
    struct etude_reading start, end;
    struct etude_reading stage_begin[ETUDE_WATCH_STAGES], stage_end[ETUDE_WATCH_STAGES];
    struct etude_watchdog_spec spec;
    struct etude_witness_bound witnesses[16];
    size_t nwitnesses;
    struct etude_remote_observer observer;
    struct etude_watchdog *watchdog;
    uint64_t stages[ETUDE_WATCH_STAGES], stage_start;
    enum etude_stage stage;
    struct etude_bound_fixed whole, gap, transfer, capture, cleanup;
    char item[256], attempt[256], case_id[256], workload[65], profile[65], assertions[65];
    char workload_ref[2048], profile_ref[2048], assertions_ref[2048];
    char record[256];
    int status;
    struct etude_measure cpu, memory;
    char cpu_method[64], memory_method[64];
};
struct nc_history {
    const struct nc_config *conf;
    struct etude_history *writer;
    struct cmp_history prior;
    struct etude_reading start, end;
    struct nc_measure *items;
    const struct nc_case **cases;
    size_t count;
    char store[4096], schema[4096], machine[128], boot[128], version[256], commit[65];
    char plan[256], selection[2048], source[2048], runner[2048], policy[2048], environment[2048], reproduce[2048];
    struct etude_bound declared[5];
    int failed, incomplete, unconfirmed, terminal;
};
int nc_selected(int argc, char **argv, int first, const char *name);
void nc_quote(struct cmp_text *t, const char *s);
int nc_blob(struct nc_history *h, struct cmp_text *out, const char *bytes, size_t size);
int nc_file_blob(struct nc_history *h, struct cmp_text *out, const char *path);
void nc_metric(struct cmp_text *t, const struct etude_measure *m);
void nc_time(struct nc_history *h, struct cmp_text *t, const struct etude_reading *a,
             const struct etude_reading *b);
int nc_record(struct nc_history *h, const char *kind, struct cmp_text *t, char id[256]);
int nc_history_open(struct nc_history *h, const struct nc_config *conf,
                    const struct nc_inventory *inventory, int argc, char **argv, int first);
int nc_history_attempt(struct nc_history *h, size_t n, const struct etude_native_result *result,
                       int status, const char *reason);
int nc_totals(struct nc_history *h, struct cmp_text *stage, struct cmp_text *family,
               struct cmp_text *concurrency);
int nc_history_finish(struct nc_history *h);
int nc_history_close(struct nc_history *h);
int nc_bounds(struct nc_history *h);
int nc_measure_start(struct nc_history *h, struct nc_measure *m);
void nc_measure_end(struct nc_measure *m);
#endif
