/*
 * watchdog.c: the per-case watchdog in plain C99: the bounds fixed from a
 * case's history, the progress read from its report records, and the
 * deadlines that follow from them. It reads no clock; the caller gives the
 * time.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "etude/report.h"
#include "etude/watchdog.h"

/* The record that reports progress, up to its witness. */
#define PROGRESS_PREFIX ETUDE_REPORT_PREFIX ETUDE_REPORT_PROGRESS " "

/* The largest sample etude_bound_fix takes: its spread, worked in eighths
 * of a microsecond and scaled by 44478, must fit in 64 bits. */
#define SAMPLE_MAX (UINT64_MAX / (8u * 44478u))

/* The state of one witness. */
struct witness_state {
    uint64_t units;
    uint64_t announced_us; /* a quiet witness's work began; 0 for none */
};

struct etude_watchdog {
    const struct etude_watchdog_spec *spec;
    struct witness_state *witness;
    struct etude_progress p;
    char line[ETUDE_REPORT_LINE_MAX]; /* a line being read, without newline */
    size_t line_len;
    int overlong; /* the line is longer than a record can be */
    enum etude_expiry expiry;
    uint64_t expiry_bound;
    struct etude_progress at_expiry;
};

/** Order two samples for qsort. */
static int ascending(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

    return x < y ? -1 : x > y;
}

/** Give twice the median of `n` sorted values, which is exact. */
static uint64_t median2(const uint64_t *v, size_t n)
{
    return n % 2 ? 2 * v[n / 2] : v[n / 2 - 1] + v[n / 2];
}

/**
 * Give 3 * 1.4826 times the median absolute deviation of `n` sorted
 * samples, rounded upward: the deviations are taken in quarters of a
 * microsecond, where the median is exact, and their median in eighths.
 */
static int spread_of(const uint64_t *v, size_t n, uint64_t *out)
{
    uint64_t *dev = malloc(n * sizeof(*dev)), m4 = 2 * median2(v, n), mad8;
    size_t i;

    if (dev == NULL)
        return ENOMEM;
    for (i = 0; i < n; i++)
        dev[i] = 4 * v[i] > m4 ? 4 * v[i] - m4 : m4 - 4 * v[i];
    qsort(dev, n, sizeof(*dev), ascending);
    mad8 = median2(dev, n);
    free(dev);
    *out = (mad8 * 44478u + 79999u) / 80000u;
    return 0;
}

int etude_bound_fix(const struct etude_bound *b, const uint64_t *samples, size_t n,
                    struct etude_bound_fixed *out)
{
    uint64_t *v, value;
    int e;

    memset(out, 0, sizeof(*out));
    if (b->floor_us == 0 || b->floor_us > b->initial_us || b->initial_us > b->ceiling_us)
        return EINVAL;
    value = b->ceiling_us;
    if (n >= ETUDE_BOUND_SAMPLES) {
        v = malloc(n * sizeof(*v));
        if (v == NULL)
            return ENOMEM;
        memcpy(v, samples, n * sizeof(*v));
        qsort(v, n, sizeof(*v), ascending);
        out->largest_us = v[n - 1];
        e = out->largest_us > SAMPLE_MAX ? EOVERFLOW : spread_of(v, n, &out->spread_us);
        free(v);
        if (e != 0)
            return e;
        out->samples = n;
        value = 2 * (out->largest_us + out->spread_us);
    }
    if (value < b->floor_us) {
        value = b->floor_us;
        out->clamped = -1;
    } else if (value > b->ceiling_us) {
        value = b->ceiling_us;
        out->clamped = 1;
    }
    out->us = value;
    return 0;
}

struct etude_watchdog *etude_watchdog_new(const struct etude_watchdog_spec *spec,
                                          enum etude_stage stage, uint64_t now_us)
{
    struct etude_watchdog *w = calloc(1, sizeof(*w));

    if (w == NULL)
        return NULL;
    w->witness = calloc(spec->witness_count + 1, sizeof(*w->witness));
    if (w->witness == NULL) {
        free(w);
        return NULL;
    }
    w->spec = spec;
    w->p.stage = stage;
    w->p.started_us = w->p.stage_started_us = w->p.last_us = now_us;
    return w;
}

void etude_watchdog_free(struct etude_watchdog *w)
{
    if (w == NULL)
        return;
    free(w->witness);
    free(w);
}

/** End the gap of execution that runs at `now_us`, keeping the longest. */
static void close_gap(struct etude_watchdog *w, uint64_t now_us)
{
    uint64_t gap = now_us > w->p.last_us ? now_us - w->p.last_us : 0;

    if (w->p.stage == ETUDE_STAGE_EXECUTE && gap > w->p.max_gap_us)
        w->p.max_gap_us = gap;
}

void etude_watchdog_stage(struct etude_watchdog *w, enum etude_stage stage, uint64_t now_us)
{
    if (stage == w->p.stage)
        return;
    close_gap(w, now_us);
    w->p.stage = stage;
    w->p.stage_started_us = now_us;
    if (stage == ETUDE_STAGE_EXECUTE)
        w->p.last_us = now_us;
}

/** Give the index of witness `id`, of `len` bytes, or -1. */
static long witness_of(const struct etude_watchdog *w, const char *id, size_t len)
{
    size_t i;

    for (i = 0; i < w->spec->witness_count; i++) {
        const char *name = w->spec->witnesses[i].witness_id;

        if (strlen(name) == len && memcmp(name, id, len) == 0)
            return (long)i;
    }
    return -1;
}

/** Read the units of a record, at most 18 digits and nothing after them. */
static int units_of(const char *s, uint64_t *v)
{
    int n = 0;

    *v = 0;
    for (; *s >= '0' && *s <= '9'; s++, n++) {
        if (n == 18)
            return -1;
        *v = *v * 10 + (uint64_t)(*s - '0');
    }
    return n > 0 && *s == '\0' ? 0 : -1;
}

/** Act on the progress record whose fields after the verb are `fields`. */
static void progress_record(struct etude_watchdog *w, const char *fields, uint64_t now_us)
{
    const char *space = strchr(fields, ' ');
    const struct etude_witness_bound *wb;
    struct witness_state *ws;
    uint64_t units;
    long i;

    if (space == NULL || units_of(space + 1, &units) != 0 ||
        (i = witness_of(w, fields, (size_t)(space - fields))) < 0) {
        w->p.refused++;
        return;
    }
    wb = &w->spec->witnesses[i];
    ws = &w->witness[i];
    if (units < ws->units || (wb->limit != 0 && units > wb->limit)) {
        w->p.refused++;
    } else if (units == ws->units) {
        w->p.repeats++;
        if (wb->kind == ETUDE_WITNESS_QUIET && ws->announced_us == 0)
            ws->announced_us = now_us;
    } else {
        close_gap(w, now_us);
        ws->units = units;
        ws->announced_us = 0;
        w->p.advances++;
        w->p.last_us = now_us;
        w->p.last_witness = wb->witness_id;
        w->p.last_units = units;
        w->p.bytes_since = 0;
    }
}

/** Act on one whole line of output, without its newline. */
static void feed_line(struct etude_watchdog *w, uint64_t now_us)
{
    size_t len = w->line_len, pre = sizeof(PROGRESS_PREFIX) - 1;

    if (len > 0 && w->line[len - 1] == '\r')
        len--;
    w->line[len] = '\0';
    if (len >= pre && memcmp(w->line, PROGRESS_PREFIX, pre) == 0)
        progress_record(w, w->line + pre, now_us);
}

void etude_watchdog_feed(struct etude_watchdog *w, const char *buf, size_t n, uint64_t now_us)
{
    size_t i;

    w->p.bytes += n;
    w->p.bytes_since += n;
    for (i = 0; i < n; i++) {
        if (buf[i] != '\n') {
            if (w->line_len + 1 < sizeof(w->line))
                w->line[w->line_len++] = buf[i];
            else
                w->overlong = 1;
            continue;
        }
        if (!w->overlong)
            feed_line(w, now_us);
        w->line_len = 0;
        w->overlong = 0;
    }
    w->p.quiet = NULL;
    w->p.quiet_us = 0;
    for (i = 0; i < w->spec->witness_count; i++) {
        if (w->witness[i].announced_us != 0 && w->witness[i].announced_us >= w->p.quiet_us) {
            w->p.quiet = w->spec->witnesses[i].witness_id;
            w->p.quiet_us = w->witness[i].announced_us;
        }
    }
}

/**
 * Give the deadline of the stage the attempt is in, 0 for none, and the
 * expiry and bound it would be.
 */
static uint64_t stage_deadline(const struct etude_watchdog *w, enum etude_expiry *e,
                               uint64_t *bound)
{
    uint64_t end = 0, us;
    size_t i;

    if (w->p.stage != ETUDE_STAGE_EXECUTE) {
        us = (size_t)w->p.stage < ETUDE_WATCH_STAGES ? w->spec->stage_us[w->p.stage] : 0;
        *e = ETUDE_EXPIRY_STAGE;
        *bound = us;
        return us == 0 ? 0 : w->p.stage_started_us + us;
    }
    for (i = 0; i < w->spec->witness_count; i++) {
        if (w->witness[i].announced_us == 0)
            continue;
        us = w->witness[i].announced_us + w->spec->witnesses[i].bound_us;
        if (us > end) {
            end = us;
            *bound = w->spec->witnesses[i].bound_us;
        }
    }
    if (end != 0) {
        *e = ETUDE_EXPIRY_QUIET;
        return end;
    }
    *e = ETUDE_EXPIRY_GAP;
    *bound = w->spec->gap_us;
    return w->p.last_us + w->spec->gap_us;
}

/** Give the earliest deadline, with its expiry and bound. */
static uint64_t next_deadline(const struct etude_watchdog *w, enum etude_expiry *e, uint64_t *bound)
{
    uint64_t whole = w->p.started_us + w->spec->case_us, stage;

    stage = stage_deadline(w, e, bound);
    if (stage != 0 && stage < whole)
        return stage;
    *e = ETUDE_EXPIRY_CASE;
    *bound = w->spec->case_us;
    return whole;
}

enum etude_expiry etude_watchdog_check(struct etude_watchdog *w, uint64_t now_us)
{
    enum etude_expiry e;
    uint64_t bound;

    if (w->expiry != ETUDE_EXPIRY_NONE)
        return w->expiry;
    if (now_us < next_deadline(w, &e, &bound))
        return ETUDE_EXPIRY_NONE;
    w->expiry = e;
    w->expiry_bound = bound;
    w->at_expiry = w->p;
    w->at_expiry.max_gap_us = etude_watchdog_max_gap(w, now_us);
    return e;
}

uint64_t etude_watchdog_deadline(const struct etude_watchdog *w)
{
    enum etude_expiry e;
    uint64_t bound;

    return next_deadline(w, &e, &bound);
}

uint64_t etude_watchdog_bound(const struct etude_watchdog *w)
{
    enum etude_expiry e;
    uint64_t bound;

    if (w->expiry != ETUDE_EXPIRY_NONE)
        return w->expiry_bound;
    next_deadline(w, &e, &bound);
    return bound;
}

enum etude_expiry etude_watchdog_due(const struct etude_watchdog *w)
{
    enum etude_expiry e;
    uint64_t bound;

    if (w->expiry != ETUDE_EXPIRY_NONE)
        return w->expiry;
    next_deadline(w, &e, &bound);
    return e;
}

const struct etude_progress *etude_watchdog_progress(const struct etude_watchdog *w)
{
    return &w->p;
}

const struct etude_progress *etude_watchdog_expired(const struct etude_watchdog *w)
{
    return w->expiry != ETUDE_EXPIRY_NONE ? &w->at_expiry : NULL;
}

uint64_t etude_watchdog_max_gap(const struct etude_watchdog *w, uint64_t now_us)
{
    uint64_t open = now_us > w->p.last_us ? now_us - w->p.last_us : 0;

    if (w->p.stage == ETUDE_STAGE_EXECUTE && open > w->p.max_gap_us)
        return open;
    return w->p.max_gap_us;
}

const char *etude_expiry_name(enum etude_expiry e)
{
    static const char *const names[] = {"none", "case", "gap", "quiet", "stage"};

    return (size_t)e < sizeof(names) / sizeof(names[0]) ? names[e] : "unknown";
}
