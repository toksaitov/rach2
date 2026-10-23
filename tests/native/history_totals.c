/* Aggregate sequential native measurements without counting a tree twice. */
#include "history.h"
#include <stdlib.h>
#include <string.h>
int nc_totals(struct nc_history *h, struct cmp_text *stage, struct cmp_text *family,
               struct cmp_text *concurrency)
{
    struct cmp_text s = {0}, f = {0}, c = {0};
    uint64_t totals[ETUDE_WATCH_STAGES] = {0}, elapsed = 0;
    for (size_t n = 0; n < h->count; n++) {
        const struct nc_measure *m = &h->items[n];
        if (*m->record && m->end.monotonic_us >= m->start.monotonic_us) {
            uint64_t span = m->end.monotonic_us - m->start.monotonic_us;
            if (span > ETUDE_HISTORY_MAX - elapsed) return 0;
            elapsed += span;
        }
        for (size_t k = 0; k < ETUDE_WATCH_STAGES; k++) {
            if (m->stages[k] > ETUDE_HISTORY_MAX - totals[k]) return 0;
            totals[k] += m->stages[k];
        }
    }
    cmp_printf(&s, "{\"method\":\"native-stage-transitions-v1\",\"transfer_us\":%llu,\"execute_us\":%llu,"
                   "\"capture_us\":%llu,\"cleanup_us\":%llu}",
               (unsigned long long)totals[ETUDE_STAGE_TRANSFER],
               (unsigned long long)totals[ETUDE_STAGE_EXECUTE],
               (unsigned long long)totals[ETUDE_STAGE_CAPTURE],
               (unsigned long long)totals[ETUDE_STAGE_CLEANUP]);
    cmp_printf(&f, "{\"native\":{\"cases\":%lu,\"elapsed_us\":%llu}}", (unsigned long)h->count,
               (unsigned long long)elapsed);
    cmp_printf(&c, "{\"method\":\"native-serial-v1\",\"width\":1,\"active_us\":%llu,"
                   "\"observed_us\":%llu,\"cleanup_confirmed\":%s,\"resources_scope\":\"reported case trees; collector and transport overhead excluded\"}",
               (unsigned long long)elapsed, (unsigned long long)(h->end.monotonic_us - h->start.monotonic_us), h->unconfirmed ? "false" : "true");
    int ok = !s.failed && !f.failed && !c.failed && nc_blob(h, stage, s.data, s.length) &&
             nc_blob(h, family, f.data, f.length) && nc_blob(h, concurrency, c.data, c.length);
    free(s.data); free(f.data); free(c.data);
    return ok;
}
