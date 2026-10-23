/* The native collector retains the common history's exact bytes and clocks. */
#define _POSIX_C_SOURCE 200809L
#include "history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void nc_quote(struct cmp_text *t, const char *s)
{
    cmp_printf(t, "\"");
    for (; s && *s; s++) {
        unsigned c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            cmp_printf(t, "\\%c", c);
        else if (c < 32 || c > 126)
            cmp_printf(t, "\\u%04x", c);
        else
            cmp_printf(t, "%c", c);
    }
    cmp_printf(t, "\"");
}
int nc_blob(struct nc_history *h, struct cmp_text *out, const char *bytes, size_t size)
{
    char path[4096], hex[65];
    if (cmp_store(h->store, bytes, size, path, sizeof(path), hex))
        return 0;
    cmp_printf(out, "{\"sha256\":\"%s\",\"size_bytes\":%llu,\"locations\":[", hex,
               (unsigned long long)size);
    nc_quote(out, path);
    cmp_printf(out, "]}");
    return !out->failed;
}
int nc_file_blob(struct nc_history *h, struct cmp_text *out, const char *path)
{
    char *bytes = NULL;
    size_t size;
    if (cmp_read_file(path, 16 * 1024 * 1024, &bytes, &size))
        return 0;
    int ok = nc_blob(h, out, bytes, size);
    free(bytes);
    return ok;
}
void nc_metric(struct cmp_text *t, const struct etude_measure *m)
{
    if (m->quality == ETUDE_QUALITY_UNAVAILABLE || m->value > ETUDE_HISTORY_MAX) {
        cmp_printf(t, "{\"value\":null,\"reason\":");
        nc_quote(t, m->method ? m->method : "the scope was not measured");
        cmp_printf(t, "}");
    } else {
        cmp_printf(t, "{\"value\":%llu,\"method\":", (unsigned long long)m->value);
        nc_quote(t, m->method ? m->method : "native-usage-v1");
        cmp_printf(t, ",\"quality\":\"%s\",\"interval_us\":null,\"error_bound\":null}",
                   m->quality == ETUDE_QUALITY_EXACT ? "exact" : "lower_bound");
    }
}
void nc_time(struct nc_history *h, struct cmp_text *t, const struct etude_reading *a,
             const struct etude_reading *b)
{
    char metric[4096];
    if (b) {
        struct etude_measurement elapsed = etude_history_elapsed(a, b);
        if (etude_measurement_json(&elapsed, metric, sizeof(metric))) {
            t->failed = 1;
            return;
        }
    } else strcpy(metric, "{\"value\":null,\"reason\":\"native cleanup is not confirmed\"}");
    cmp_printf(t, "{\"start_utc\":\"%s\",\"end_utc\":", a->utc);
    if (b) nc_quote(t, b->utc); else cmp_printf(t, "null");
    cmp_printf(t, ",\"clock\":{\"machine_id\":"); nc_quote(t, h->machine);
    cmp_printf(t, ",\"boot_id\":"); nc_quote(t, h->boot);
    cmp_printf(t, ",\"clock_id\":\"monotonic\",\"resolution_us\":%llu},\"start_us\":%llu,\"end_us\":",
               (unsigned long long)a->resolution_us, (unsigned long long)a->monotonic_us);
    if (b) cmp_printf(t, "%llu", (unsigned long long)b->monotonic_us);
    else cmp_printf(t, "null");
    cmp_printf(t, ",\"elapsed_us\":%s}", metric);
}
int nc_record(struct nc_history *h, const char *kind, struct cmp_text *t, char id[256])
{
    struct etude_reading now;
    int ok = !t->failed && !etude_history_now(&now) &&
             !etude_history_record_id(h->writer, id, 256) &&
             !etude_history_append(h->writer, id, now.utc, kind, t->data);
    free(t->data);
    memset(t, 0, sizeof(*t));
    h->failed |= !ok;
    return ok;
}
