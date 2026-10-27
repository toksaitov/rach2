/* baseline.c: the choice of a comparison's baseline, and its verdict. */
#include "compare.h"
#include "../../utils/sha256.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Leave a sample out of the comparison, with its reason. */
static void leave_out(struct cmp_result *r, struct cmp_sample *s, int candidate, const char *format,
                      ...)
{
    struct cmp_exclusion *e = &r->excluded[r->excluded_count++];
    va_list arguments;
    e->sample = s;
    e->candidate = candidate;
    va_start(arguments, format);
    vsnprintf(e->reason, sizeof(e->reason), format, arguments);
    va_end(arguments);
}

/** Return the time that places a sample: its end, or its start without one. */
static const char *placed(const struct cmp_sample *s)
{
    return s->end[0] ? s->end : s->start;
}

/**
 * Take the eligible candidates of the group, all with the timing method of
 * the first; return that first one, by whose key the group is compared.
 */
static struct cmp_sample *choose_candidates(struct cmp_result *r)
{
    struct cmp_sample *reference = NULL;
    size_t i;
    for (i = 0; i < r->candidate_count; i++) {
        struct cmp_sample *s = r->candidates[i];
        if (s->reason[0]) {
            leave_out(r, s, 1, "%s", s->reason);
            continue;
        }
        if (!reference)
            reference = s;
        else if (cmp_key_differs(&s->key, &reference->key, CMP_PLAN, CMP_FIELDS, NULL, 0)) {
            leave_out(r, s, 1, "its timing method differs from that of %s", reference->attempt->id);
            continue;
        }
        r->eligible[r->eligible_count++] = s;
    }
    return reference;
}

/** Find the samples of one case; return their number and the first's index. */
static size_t case_range(const struct cmp_history *h, const char *case_id, size_t *first)
{
    size_t low = 0, high = h->cased, end;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (strcmp(case_id, h->by_case[middle]->key.value[0]) <= 0)
            high = middle;
        else
            low = middle + 1;
    }
    for (end = low; end < h->cased && !strcmp(h->by_case[end]->key.value[0], case_id); end++)
        ;
    *first = low;
    return end - low;
}

/** Return whether a comparison record decided a case of this subject. */
static int decided(const struct cmp_record *c, const char *subject)
{
    const char *verdict = cmp_text(c->data, "verdict");
    return !strcmp(cmp_text(c->data, "purpose"), "same_site_regression") &&
           !strcmp(cmp_text(c->data, "scope"), "case") &&
           !strcmp(cmp_text(c->data, "subject_id"), subject) &&
           (!strcmp(verdict, "regression") || !strcmp(verdict, "improved") ||
            !strcmp(verdict, "within_noise"));
}

/**
 * Return whether every sample a comparison accepted is of another run than
 * the candidates' and placed before `start`, when the candidates start.
 */
static int precedes(const struct cmp_history *h, const struct cmp_record *c,
                    const struct cmp_sample *reference, const char *start)
{
    const struct eh_json *id = eh_json_get(eh_json_get(c->data, "baseline"), "samples")->child;
    for (; id; id = id->next) {
        const struct cmp_record *a = cmp_find(h, id->string);
        if (a && (!strcmp(a->run, reference->attempt->run) ||
                  (a->sample && strcmp(placed(a->sample), start) > 0)))
            return 0;
    }
    return 1;
}

/**
 * Find the newest comparison whose accepted baseline is of the candidate's
 * series, and the newest whose baseline has the candidate's whole key, of
 * those whose baseline precedes the candidates' `start`.
 */
static void find_accepted(const struct cmp_history *h, const struct cmp_sample *reference,
                          const char *start, const struct cmp_record **series,
                          const struct cmp_record **match)
{
    size_t i;
    *series = *match = NULL;
    for (i = h->comparison_count; i-- > 0;) {
        const struct cmp_record *c = h->comparisons[i], *first;
        const struct eh_json *samples;
        if (!decided(c, reference->key.value[0]) || !precedes(h, c, reference, start))
            continue;
        samples = eh_json_get(eh_json_get(c->data, "baseline"), "samples");
        first = cmp_find(h, samples->child->string);
        if (!first || !first->sample ||
            cmp_key_differs(&first->sample->key, &reference->key, 0, CMP_SERIES, NULL, 0))
            continue;
        if (!*series)
            *series = c;
        if (!cmp_key_differs(&first->sample->key, &reference->key, 0, CMP_FIELDS, NULL, 0)) {
            *match = c;
            return;
        }
    }
}

/** Return the latest placing time of the samples a comparison accepted. */
static void newest_sample(const struct cmp_history *h, const struct cmp_record *c,
                          char out[CMP_UTC])
{
    const struct eh_json *id = eh_json_get(eh_json_get(c->data, "baseline"), "samples")->child;
    out[0] = 0;
    for (; id; id = id->next) {
        const struct cmp_record *a = cmp_find(h, id->string);
        if (a && a->sample && strcmp(placed(a->sample), out) > 0)
            strcpy(out, placed(a->sample));
    }
}

/**
 * Take the baseline that an earlier comparison accepted, when each of its
 * samples is still in the history, eligible and of the candidate's key.
 */
static int use_accepted(const struct cmp_history *h, const struct cmp_record *c,
                        const struct cmp_sample *reference, struct cmp_result *r, size_t room)
{
    const struct eh_json *id = eh_json_get(eh_json_get(c->data, "baseline"), "samples")->child;
    const char *baseline_id = cmp_text(c->data, "baseline_id");
    for (; id; id = id->next) {
        const struct cmp_record *a = cmp_find(h, id->string);
        const char *why = NULL;
        if (!a || !a->sample)
            why = "is not an attempt in the history";
        else if (a->sample->reason[0])
            why = a->sample->reason;
        else if (cmp_key_differs(&a->sample->key, &reference->key, 0, CMP_FIELDS, NULL, 0) ||
                 r->baseline_count == room)
            why = "differs from the candidate";
        if (why) {
            snprintf(
                r->origin, sizeof(r->origin),
                "the baseline %s that comparison %s accepted cannot be used: its sample %s: %s; ",
                baseline_id, c->id, id->string, why);
            r->baseline_count = 0;
            return 0;
        }
        r->baseline[r->baseline_count++] = a->sample;
    }
    snprintf(r->baseline_id, sizeof(r->baseline_id), "%s", baseline_id);
    snprintf(r->origin, sizeof(r->origin), "accepted by comparison %s", c->id);
    return 1;
}

/** Return the earliest start of the eligible candidates. */
static void candidate_start(const struct cmp_result *r, char out[CMP_UTC])
{
    size_t i;
    strcpy(out, r->eligible[0]->start);
    for (i = 1; i < r->eligible_count; i++)
        if (strcmp(r->eligible[i]->start, out) < 0)
            strcpy(out, r->eligible[i]->start);
}

/**
 * Take as the baseline every eligible sample of the candidate's key, from
 * another run, placed after `after` and ending before the candidates start;
 * the other samples of the series in that window are left out with reasons.
 */
static void derive(const struct cmp_history *h, const struct cmp_sample *reference,
                   const char *after, struct cmp_result *r, size_t first, size_t count)
{
    char start[CMP_UTC], names[400];
    size_t i;
    candidate_start(r, start);
    for (i = first; i < first + count; i++) {
        struct cmp_sample *s = h->by_case[i];
        int differ;
        if (cmp_key_differs(&s->key, &reference->key, 0, CMP_SERIES, NULL, 0) ||
            !strcmp(s->attempt->run, reference->attempt->run) || strcmp(placed(s), start) > 0)
            continue;
        if (strcmp(placed(s), after) <= 0) {
            r->earlier++;
            continue;
        }
        differ =
            cmp_key_differs(&s->key, &reference->key, CMP_SERIES, CMP_FIELDS, names, sizeof(names));
        if (s->reason[0])
            leave_out(r, s, 0, "%s", s->reason);
        else if (differ)
            leave_out(r, s, 0, "its %s differ%s from the candidate's", names,
                      differ == 1 ? "s" : "");
        else
            r->baseline[r->baseline_count++] = s;
    }
}

/** Name a new baseline by its policy and samples, so that the name is fixed. */
static void name_baseline(struct cmp_result *r)
{
    struct sha256 sha;
    unsigned char digest[32];
    char hex[65];
    size_t i;
    sha256_init(&sha);
    sha256_update(&sha, (const unsigned char *)"median-mad-v1", 13);
    for (i = 0; i < r->baseline_count; i++) {
        sha256_update(&sha, (const unsigned char *)"\n", 1);
        sha256_update(&sha, (const unsigned char *)r->baseline[i]->attempt->id,
                      strlen(r->baseline[i]->attempt->id));
    }
    sha256_final(&sha, digest);
    sha256_hex(digest, hex);
    hex[32] = 0;
    snprintf(r->baseline_id, sizeof(r->baseline_id), "median-mad-v1:%s", hex);
}

/** Take the statistics of samples, with their largest error and interval. */
static int stats_of(struct cmp_sample **samples, size_t count, struct cmp_stats *stats)
{
    uint64_t *values = malloc((count + 1) * sizeof(*values));
    size_t i;
    int status;
    if (!values)
        return -1;
    memset(stats, 0, sizeof(*stats));
    stats->quality = "exact";
    for (i = 0; i < count; i++) {
        const struct cmp_sample *s = samples[i];
        values[i] = s->elapsed_us;
        if (s->error_us > stats->error_us)
            stats->error_us = s->error_us;
        if (s->interval_us > stats->interval_us)
            stats->interval_us = s->interval_us;
        if (!strcmp(s->quality, "sampled"))
            stats->quality = "sampled";
        stats->method = s->key.value[CMP_PLAN];
    }
    status = cmp_statistics(values, count, stats);
    free(values);
    return status;
}

/** Make the verdict not_comparable when only another key's samples precede. */
static int differing_key(struct cmp_result *r)
{
    size_t i;
    for (i = r->excluded_count; i-- > 0;) {
        const struct cmp_exclusion *e = &r->excluded[i];
        if (e->candidate || e->sample->reason[0])
            continue;
        r->verdict = "not_comparable";
        snprintf(r->reason, sizeof(r->reason),
                 "no earlier sample has the candidate's key: the newest of the series, %s, is "
                 "left out since %s, and this candidate starts a new baseline",
                 e->sample->attempt->id, e->reason);
        return 1;
    }
    return 0;
}

/** Explain a decisive verdict by the medians and the bands. */
static void explain(struct cmp_result *r)
{
    char base[32], cand[32], difference[32], band[32];
    uint64_t b = r->base.median4, c = r->cand.median4;
    uint64_t d4 = b > c ? b - c : c - b;
    cmp_seconds(r->base.median_us, base);
    cmp_seconds(r->cand.median_us, cand);
    cmp_seconds((d4 + 3) / 4, difference);
    cmp_seconds(r->base.band_us + r->cand.band_us, band);
    if (!strcmp(r->verdict, "within_noise"))
        snprintf(r->reason, sizeof(r->reason),
                 "the candidate's median, %s, differs from the baseline's, %s, by %s, within "
                 "their combined noise band of %s",
                 cand, base, difference, band);
    else
        snprintf(r->reason, sizeof(r->reason),
                 "the candidate's median, %s, is %s %s than the baseline's, %s, beyond their "
                 "combined noise band of %s",
                 cand, difference, c > b ? "slower" : "faster", base, band);
}

/** Take the statistics of both sides and judge them. */
static int finish(struct cmp_result *r)
{
    if (stats_of(r->eligible, r->eligible_count, &r->cand))
        return -1;
    if (r->baseline_count == 0 && differing_key(r))
        return 0;
    if (r->baseline_count < CMP_MINIMUM) {
        r->verdict = "insufficient_history";
        r->base.count = r->baseline_count;
        snprintf(r->reason, sizeof(r->reason),
                 "the series has %lu eligible baseline sample%s, and median-mad-v1 requires %d",
                 (unsigned long)r->baseline_count, r->baseline_count == 1 ? "" : "s", CMP_MINIMUM);
        return 0;
    }
    if (!r->baseline_id[0])
        name_baseline(r);
    if (stats_of(r->baseline, r->baseline_count, &r->base))
        return -1;
    r->verdict = cmp_verdict(&r->base, &r->cand);
    explain(r);
    return 0;
}

/**
 * Compare a group of a run's items that share a key with the history of
 * their series: the same case, architecture, domain, machine and accelerator.
 *
 * The baseline is the one the newest earlier comparison of the candidate's
 * whole key accepted, of those whose samples are of other runs and precede
 * the candidates, unless options->new_baseline asks for a new one. A new
 * baseline has the eligible samples of that key since the newest sample of
 * the series' last accepted baseline, or since the start of the history. The
 * verdict is not_comparable when no candidate is eligible, or when no sample
 * of the key is but an earlier sample of another key is; insufficient_history
 * below CMP_MINIMUM samples; and otherwise the one cmp_verdict gives.
 * Return -1 when memory runs out.
 */
int cmp_compare(const struct cmp_history *h, const struct cmp_options *options,
                struct cmp_sample **candidates, size_t count, struct cmp_result *r)
{
    const struct cmp_record *series, *match;
    struct cmp_sample *reference;
    size_t first = 0, room = 0;
    char after[CMP_UTC] = "", start[CMP_UTC];
    memset(r, 0, sizeof(*r));
    r->candidates = candidates;
    r->candidate_count = count;
    r->eligible = malloc((count + 1) * sizeof(*r->eligible));
    if (!r->eligible)
        return -1;
    if (candidates[0]->key.value[0])
        room = case_range(h, candidates[0]->key.value[0], &first);
    r->excluded = malloc((count + room + 1) * sizeof(*r->excluded));
    r->baseline = malloc((room + 1) * sizeof(*r->baseline));
    if (!r->excluded || !r->baseline)
        return -1;
    reference = choose_candidates(r);
    if (!reference) {
        r->verdict = "not_comparable";
        snprintf(r->reason, sizeof(r->reason), "no candidate is eligible: %s",
                 r->excluded[0].reason);
        return 0;
    }
    candidate_start(r, start);
    find_accepted(h, reference, start, &series, &match);
    if (!match || options->new_baseline || !use_accepted(h, match, reference, r, room)) {
        size_t used = strlen(r->origin);
        if (series)
            newest_sample(h, series, after);
        snprintf(r->origin + used, sizeof(r->origin) - used, "%s",
                 series ? "new, from the eligible samples since the newest sample of the "
                          "series' last accepted baseline"
                        : "new, from the eligible samples of the series before the candidate");
        derive(h, reference, after, r, first, room);
    }
    return finish(r);
}

/** Free what cmp_compare allocated. */
void cmp_result_free(struct cmp_result *r)
{
    free(r->eligible);
    free(r->excluded);
    free(r->baseline);
    memset(r, 0, sizeof(*r));
}
