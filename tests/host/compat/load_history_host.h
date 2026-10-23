/* Synthetic complete observations for tests of history consumers. The
 * counter values are fixtures, not observations of the test machine. */
#ifndef LOAD_HISTORY_HOST_H
#define LOAD_HISTORY_HOST_H
#include "compare.h"
#include "etude/observe.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int load_fixture_store(char *out, size_t size, const char *root, const char *bytes)
{
    struct sha256 hash;
    unsigned char digest[32];
    char hex[65], path[1400];
    sha256_init(&hash);
    sha256_update(&hash, (const unsigned char *)bytes, strlen(bytes));
    sha256_final(&hash, digest);
    sha256_hex(digest, hex);
    int n = snprintf(path, sizeof path, "%s/%s.load", root, hex);
    if (n < 0 || n >= (int)sizeof path)
        return -1;
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    int failed = fputs(bytes, f) < 0;
    failed |= fclose(f) != 0;
    if (failed)
        return -1;
    n = snprintf(out, size, "{\"sha256\":\"%s\",\"size_bytes\":%lu,\"locations\":[\"%s\"]}", hex,
                 (unsigned long)strlen(bytes), path);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

static int load_fixture(char *out, size_t size, const char *root, const char *run, const char *time)
{
    struct eh_json *v = eh_json_parse(time);
    uint64_t start = etude_observe_utc(cmp_text(v, "start_utc"));
    uint64_t end = etude_observe_utc(cmp_text(v, "end_utc"));
    eh_json_free(v);
    if (!start || end < start)
        return -1;
    struct cmp_text trace = {0}, load = {0};
    char ref[1800];
    cmp_printf(&trace,
               "{\"kind\":\"start\",\"schema_version\":1,\"method\":\"native-load-v1\","
               "\"platform\":\"linux-proc-counters-v1\",\"machine_id\":\"fixture\","
               "\"boot_id\":\"fixture-boot\",\"run_id\":\"%s\",\"root_pid\":1,"
               "\"root_creation\":\"1\",\"observer_pid\":2,\"observer_creation\":\"2\","
               "\"requested_interval_us\":1000000,\"process_limit\":4096}\n",
               run);
    uint64_t samples = (end - start + 999999) / 1000000 + 1;
    if (samples < 2)
        samples = 2;
    for (uint64_t i = 0; i < samples; i++)
        cmp_printf(&trace,
                   "{\"kind\":\"sample\",\"monotonic_us\":%llu,\"utc_us\":%llu,"
                   "\"system_busy_cpu_us\":%llu,\"outside_cpu_lower_us\":0,"
                   "\"shared_virtualizer_cpu_lower_us\":0,\"processes\":2,\"denied\":0,"
                   "\"lost\":0,\"unclassified\":0,\"overflow\":false,\"identities_sha256\":"
                   "\"0000000000000000000000000000000000000000000000000000000000000000\"}\n",
                   (unsigned long long)(i + 1 == samples ? end - start : i * 1000000),
                   (unsigned long long)(i + 1 == samples ? end : start + i * 1000000),
                   (unsigned long long)i);
    cmp_printf(&trace,
               "{\"kind\":\"end\",\"utc_us\":%llu,\"monotonic_us\":%llu,"
               "\"samples\":%llu,\"complete\":true,\"owner_lost\":false}\n",
               (unsigned long long)end, (unsigned long long)(end - start),
               (unsigned long long)samples);
    int rc = -1;
    if (trace.failed || load_fixture_store(ref, sizeof ref, root, trace.data))
        goto done;
    cmp_printf(&load,
               "{\"method\":\"native-load-v1\",\"comparison_available\":true,"
               "\"context_eligible\":true,\"runtime_start_utc_us\":%llu,"
               "\"runtime\":{\"trace\":%s,\"summary\":{\"comparison_available\":true}},"
               "\"outer\":{\"trace\":%s,\"summary\":{\"comparison_available\":true}},"
               "\"physical\":null,\"physical_required\":false}",
               (unsigned long long)start, ref, ref);
    if (!load.failed)
        rc = load_fixture_store(out, size, root, load.data);
done:
    free(trace.data);
    free(load.data);
    return rc;
}
#endif
