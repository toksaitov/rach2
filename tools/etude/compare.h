/*
 * compare.h: the comparison of a run's cases with the earlier samples of
 * their series, by the policy median-mad-v1 of the measurement record.
 */
#ifndef ETUDE_COMPARE_H
#define ETUDE_COMPARE_H
#include "history_json.h"
#include <stddef.h>
#include <stdint.h>

#define CMP_MINIMUM 5 /* the baseline samples median-mad-v1 requires */
#define CMP_FIELDS  16
/* The leading CMP_SERIES fields name a series; a plan and its item give the
 * first CMP_PLAN, and the attempt the last. */
#define CMP_SERIES  5
#define CMP_PLAN    (CMP_FIELDS - 1)
#define CMP_UTC     27 /* a normalized UTC time and its NUL */

struct cmp_sample;

/* One record of the history, parsed. */
struct cmp_record {
    const char *id, *run, *kind;
    char utc[CMP_UTC]; /* recorded_utc, with six digits of fraction */
    unsigned char digest[32];
    size_t length, order; /* the line's bytes, and its place in the input */
    struct eh_json *json;
    const struct eh_json *data;
    struct cmp_sample *sample; /* an attempt's sample, or NULL */
};

/* The values two samples must share to be compared, in the order of names. */
struct cmp_key {
    const char *value[CMP_FIELDS];
};

/* An attempt considered as a sample. reason is empty when it is eligible. */
struct cmp_sample {
    const struct cmp_record *attempt, *plan, *run;
    const struct eh_json *item;
    struct cmp_key key;
    char start[CMP_UTC], end[CMP_UTC];          /* end is empty when unknown */
    uint64_t elapsed_us, error_us, interval_us; /* error: resolution or bound */
    const char *quality;
    char reason[400];
};

/* The records of every stream read, and what could not be read. */
struct cmp_history {
    struct cmp_record *records;
    size_t count, capacity;
    struct cmp_record **by_id;       /* sorted by record ID, replays removed */
    struct cmp_record **by_run;      /* the run records, sorted by run ID */
    struct cmp_record **comparisons; /* the comparison records, oldest first */
    struct cmp_sample *samples;      /* one for each attempt record */
    struct cmp_sample **by_case;     /* the samples with a case, by case ID */
    size_t unique, runs, comparison_count, sample_count, cased;
    char **notes;
    size_t note_count;
};

/* A sample left out of a baseline, and why. */
struct cmp_exclusion {
    struct cmp_sample *sample;
    int candidate; /* 1 for a candidate of the run compared */
    char reason[400];
};

/* The median, deviation and noise band of a set of durations. */
struct cmp_stats {
    size_t count;
    uint64_t median_us, mad_us, band_us; /* rounded upward to microseconds */
    uint64_t median4;                    /* four times the unrounded median */
    uint64_t error_us, interval_us;      /* the largest of the samples' */
    const char *method, *quality;
};

/* A comparison of one group of a run's items with its baseline. */
struct cmp_result {
    const char *verdict;
    struct cmp_sample **candidates; /* the group, eligible or not */
    struct cmp_sample **eligible;   /* the candidates compared */
    struct cmp_sample **baseline;
    struct cmp_exclusion *excluded; /* candidates and series samples left out */
    size_t candidate_count, eligible_count, baseline_count, excluded_count, earlier;
    struct cmp_stats base, cand;
    char baseline_id[128];
    char origin[512]; /* how the baseline was chosen */
    char reason[1024];
};

/* What the command was asked to do. */
struct cmp_options {
    const char *schemas, *store, *repo, *output, *stream_id, *run_id;
    int new_baseline;
    char **streams;
    int stream_count;
};

/* A growing text, kept terminated by a NUL; failed is set when memory ran
 * out. */
struct cmp_text {
    char *data;
    size_t length, capacity;
    int failed;
};

extern const char *const cmp_field_names[CMP_FIELDS];
extern const char cmp_policy[];
/* The command named at the start of the library's diagnostics, `etude
 * compare` unless the command that runs it sets another. */
extern const char *cmp_program;

int cmp_load(struct cmp_history *history, const char *schema, char **paths, int count);
void cmp_free(struct cmp_history *history);
const struct cmp_record *cmp_find(const struct cmp_history *history, const char *id);
const char *cmp_text(const struct eh_json *object, const char *name);
const char *cmp_digest(const struct eh_json *object, const char *name);
void cmp_utc(const char *text, char out[CMP_UTC]);
int cmp_key_differs(const struct cmp_key *a, const struct cmp_key *b, size_t from, size_t to,
                    char *names, size_t size);
int cmp_statistics(const uint64_t *values, size_t count, struct cmp_stats *stats);
const char *cmp_verdict(const struct cmp_stats *base, const struct cmp_stats *cand);
int cmp_compare(const struct cmp_history *history, const struct cmp_options *options,
                struct cmp_sample **candidates, size_t count, struct cmp_result *result);
void cmp_result_free(struct cmp_result *result);
void cmp_seconds(uint64_t us, char out[32]);
void cmp_printf(struct cmp_text *text, const char *format, ...);
void cmp_item_key(const struct eh_json *plan, const struct eh_json *item, struct cmp_key *key);
int cmp_read_file(const char *path, size_t limit, char **bytes, size_t *length);
int cmp_store(const char *store, const char *bytes, size_t length, char *path, size_t size,
              char hex[65]);
int cmp_report(const struct cmp_history *history, const struct cmp_options *options,
               const struct cmp_result *result, struct cmp_text *text);
int cmp_main(int argc, char **argv);
#endif
