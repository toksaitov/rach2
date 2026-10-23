/* A complete, bounded helper trace can be imported across the WSL boundary. */
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "etude/observe.h"
#include "history_json.h"
#include "json.h"

struct trace_reader {
    struct eh_json *header;
    const char *first, *last;
    size_t first_size, last_size, count;
    uint64_t start_utc, end_utc, start_us, end_us, cpu, max_gap;
    int cpu_known, ended;
};

uint64_t etude_observe_utc(const char *utc)
{
    static const unsigned month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};

    unsigned y, m, d, h, minute, second, fraction = 0, digits = 0;
    if (!utc || !eh_utc(utc) || sscanf(utc, "%u-%u-%uT%u:%u:%u", &y, &m, &d, &h,
        &minute, &second) != 6 || y < 1970) return 0;
    const char *p = strchr(utc, '.');
    if (p) while (*++p >= '0' && *p <= '9' && digits < 6) {
        fraction = fraction * 10 + (unsigned)(*p - '0'); digits++;
    }
    while (digits++ < 6) fraction *= 10;
    uint64_t days = (uint64_t)(y - 1970) * 365 + (y - 1) / 4 - 1969 / 4 -
        (y - 1) / 100 + 1969 / 100 + (y - 1) / 400 - 1969 / 400;
    for (unsigned i = 1; i < m; i++) days += month_days[i - 1];
    if (m > 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) days++;
    return (((days + d - 1) * 24 + h) * 3600 + minute * 60 + second) * 1000000u + fraction;
}

static int number(const struct eh_json *v, const char *key, uint64_t *value)
{
    const struct eh_json *n = eh_json_get(v, key);
    if (!n || n->type != 'n' || n->number < 0 ||
        n->number > JSON_INTEGER_MAX || (double)(uint64_t)n->number != n->number) return 0;
    *value = (uint64_t)n->number;
    return 1;
}

static int text_is(const struct eh_json *v, const char *key, const char *s)
{
    const struct eh_json *n = eh_json_get(v, key);
    return n && n->type == 's' && !strcmp(n->string, s);
}

static int identity(const struct eh_json *v, const char *key)
{
    const struct eh_json *n = eh_json_get(v, key);
    if (!n || n->type != 's' || !n->string[0] ||
        n->string[strspn(n->string, "0123456789")] || strlen(n->string) > 20) return 0;
    char *end;
    errno = 0;
    return strtoull(n->string, &end, 10) != 0 && !errno && !*end;
}

static int header(const struct eh_json *v, const char *run)
{
    uint64_t version, interval, count, pid, observer;
    const struct eh_json *machine = eh_json_get(v, "machine_id");
    const struct eh_json *boot = eh_json_get(v, "boot_id");
    return text_is(v, "kind", "start") && text_is(v, "method", "native-load-v1") &&
        text_is(v, "run_id", run) && machine && machine->type == 's' &&
        machine->string[0] && boot && boot->type == 's' && boot->string[0] &&
        (text_is(v, "platform", "linux-proc-counters-v1") ||
         text_is(v, "platform", "macos-mach-libproc-v1") ||
         text_is(v, "platform", "windows-native-counters-v1")) &&
        number(v, "schema_version", &version) && version == 1 &&
        number(v, "requested_interval_us", &interval) && interval == ETUDE_OBSERVE_INTERVAL_US &&
        number(v, "process_limit", &count) && count == ETUDE_OBSERVE_PROCESSES &&
        number(v, "root_pid", &pid) && pid > 0 && identity(v, "root_creation") &&
        number(v, "observer_pid", &observer) && observer > 0 && identity(v, "observer_creation");
}

static int sample(struct trace_reader *r, const struct eh_json *v, const char *line, size_t n)
{
    uint64_t utc, mono, cpu, count, ignored;
    const struct eh_json *digest = eh_json_get(v, "identities_sha256");
    const struct eh_json *overflow = eh_json_get(v, "overflow");
    if (!digest || digest->type != 's' || strlen(digest->string) != 64 ||
        digest->string[strspn(digest->string, "0123456789abcdef")] ||
        !overflow || (overflow->type != 't' && overflow->type != 'f') ||
        !number(v, "processes", &count) || count > ETUDE_OBSERVE_PROCESSES ||
        !number(v, "denied", &ignored) || !number(v, "lost", &ignored) ||
        !number(v, "unclassified", &ignored) ||
        !number(v, "outside_cpu_lower_us", &ignored) ||
        !number(v, "shared_virtualizer_cpu_lower_us", &ignored)) return -1;
    if (!number(v, "utc_us", &utc) || !number(v, "monotonic_us", &mono)) return -1;
    int known = number(v, "system_busy_cpu_us", &cpu);
    if (!r->count) {
        r->first = line; r->first_size = n;
        r->start_utc = utc; r->start_us = mono;
        r->cpu_known = known;
    } else {
        if (mono < r->end_us || utc < r->end_utc) return -1;
        if (mono - r->end_us > r->max_gap) r->max_gap = mono - r->end_us;
        if (!known || (r->cpu_known && cpu < r->cpu)) r->cpu_known = 0;
    }
    if (known) r->cpu = cpu;
    r->last = line; r->last_size = n;
    r->end_utc = utc; r->end_us = mono; r->count++;
    return 0;
}

static int read_line(struct trace_reader *r, const char *line, size_t n, const char *run)
{
    char text[4096];
    if (n >= sizeof text || memchr(line, 0, n) || r->ended) return -1;
    memcpy(text, line, n); text[n] = 0;
    struct eh_json *v = eh_json_parse(text);
    if (!v) return -1;
    int rc = -1;
    if (!r->header && header(v, run)) {
        r->header = v;
        return 0;
    } else if (r->header && text_is(v, "kind", "sample")) rc = sample(r, v, line, n);
    else if (r->header && text_is(v, "kind", "end")) {
        uint64_t count, mono, utc;
        const struct eh_json *complete = eh_json_get(v, "complete");
        if (complete && complete->type == 't' && number(v, "samples", &count) &&
            number(v, "utc_us", &utc) && number(v, "monotonic_us", &mono) &&
            text_is(v, "kind", "end") && eh_json_get(v, "owner_lost") &&
            eh_json_get(v, "owner_lost")->type == 'f' && count == r->count && count >= 2 && utc == r->end_utc && mono == r->end_us) {
            r->ended = 1; rc = 0;
        }
    }
    eh_json_free(v);
    return rc;
}

char *etude_observe_import(const char *bytes, size_t length, const char *run,
                          uint64_t start, uint64_t end)
{
    struct trace_reader r = {0};
    struct jbuf b = {0};
    const char *p = bytes, *limit;
    if (!bytes || !run || end < start || length > ETUDE_OBSERVE_BYTES || !length || bytes[length - 1] != '\n' ||
        memchr(bytes, 0, length)) return NULL;
    limit = bytes + length;
    while (p < limit) {
        const char *nl = memchr(p, '\n', (size_t)(limit - p));
        if (!nl || read_line(&r, p, (size_t)(nl - p), run)) goto fail;
        p = nl + 1;
    }
    if (!r.ended || !r.cpu_known || r.max_gap > 5 * ETUDE_OBSERVE_INTERVAL_US ||
        r.start_utc > start || r.end_utc < end) goto fail;
    char prefix[512];
    snprintf(prefix, sizeof prefix, "{\"schema_version\":1,\"method\":\"native-load-v1\","
        "\"comparison_available\":true,\"coverage\":{\"start_utc_us\":%llu,\"end_utc_us\":%llu,"
        "\"start_us\":%llu,\"end_us\":%llu,\"samples\":%lu,\"max_gap_us\":%llu},\"first\":",
        (unsigned long long)r.start_utc, (unsigned long long)r.end_utc,
        (unsigned long long)r.start_us, (unsigned long long)r.end_us,
        (unsigned long)r.count, (unsigned long long)r.max_gap);
    jbuf_puts(&b, prefix); jbuf_put(&b, r.first, r.first_size);
    jbuf_puts(&b, ",\"last\":"); jbuf_put(&b, r.last, r.last_size); jbuf_puts(&b, "}");
    eh_json_free(r.header);
    return b.data;
fail:
    eh_json_free(r.header);
    return NULL;
}
