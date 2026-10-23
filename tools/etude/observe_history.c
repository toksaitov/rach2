/* Observation files become history blobs only after their final sample. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include "win/files.h"
#define observation_sync _commit
#else
#include <poll.h>
#define observation_sync fsync
#endif
#include "observe_history.h"
#include "compare.h"
#include "json.h"

static struct json *descriptor(const struct etude_measurement_context *context, const char *key)
{
    const char *text = etude_context_text(context, key);
    struct json *value = NULL;
    char error[128];
    if (text && json_parse(text, strlen(text), 1, &value, error, sizeof error))
        return NULL;
    return value;
}

static int request(const struct json *v, const char *run)
{
    const char *path = json_text(v, "stop_request");
    if (!path || !run)
        return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return -1;
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        return -1;
    }
    int rc = fprintf(f, "%s\n", run) < 0 || fflush(f) || observation_sync(fd);
    if (fclose(f))
        rc = 1;
    return rc ? -1 : 0;
}

void etude_observation_cancel(const struct etude_measurement_context *context, const char *run)
{
    const char *keys[] = {"outer_observation", "physical_observation"};
    if (!run || !*run)
        run = etude_context_run_id(context);
    for (size_t i = 0; i < 2; i++) {
        struct json *v = descriptor(context, keys[i]);
        if (v && v->type != JSON_NULL)
            request(v, run);
        json_free(v);
    }
}

static int ended(const char *path)
{
    char tail[1025];
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    int ok = !fseek(f, 0, SEEK_END);
    long size = ok ? ftell(f) : -1;
    if (size < 0 || (unsigned long)size > ETUDE_OBSERVE_BYTES ||
        fseek(f, size > 1024 ? size - 1024 : 0, SEEK_SET)) {
        fclose(f);
        return 0;
    }
    size_t n = fread(tail, 1, sizeof tail - 1, f);
    ok = !ferror(f);
    fclose(f);
    tail[n] = 0;
    return ok && n && tail[n - 1] == '\n' && strstr(tail, "\"kind\":\"end\"");
}

static void pause_sample(void)
{
#ifdef _WIN32
    Sleep(100);
#else
    poll(NULL, 0, 100);
#endif
}

static int keep_trace(const char *store, const char *path, const char *run, uint64_t start,
                      uint64_t end, const char *summary, struct cmp_text *out)
{
    char *bytes = NULL, *verified = NULL, location[8192], hash[65];
    size_t length = 0;
    int rc = -1;
    if (!cmp_read_file(path, ETUDE_OBSERVE_BYTES, &bytes, &length))
        verified = etude_observe_import(bytes, length, run, start, end);
    if (verified && !cmp_store(store, bytes, length, location, sizeof location, hash)) {
        struct jbuf quoted = {0};
        struct json s = {0};
        s.type = JSON_STRING;
        s.string = location;
        json_canonical(&s, &quoted);
        cmp_printf(out,
                   "{\"trace\":{\"sha256\":\"%s\",\"size_bytes\":%lu,"
                   "\"locations\":[%s]},\"summary\":%s}",
                   hash, (unsigned long)length, quoted.data, summary ? summary : verified);
        jbuf_free(&quoted);
        rc = out->failed ? -1 : 0;
    } else
        cmp_printf(out, "null");
    free(bytes);
    free(verified);
    return rc;
}

int etude_observation_finish(const struct etude_observation_run *r, char **json)
{
    struct cmp_text pieces[3] = {{0}, {0}, {0}}, out = {0};
    const char *keys[] = {"outer_observation", "physical_observation"};
    struct json *v[2] = {descriptor(r->context, keys[0]), descriptor(r->context, keys[1])};
    uint64_t start = etude_observe_utc(r->start.utc);
    uint64_t runtime_start = etude_observe_utc(r->runtime_start.utc);
    uint64_t end = etude_observe_utc(r->end.utc);
    int rc = 0, supplied[2] = {0, 0}, available[3] = {0, 0, 0};
    *json = NULL;
    for (size_t i = 0; i < 2; i++) {
        supplied[i] = v[i] && v[i]->type != JSON_NULL;
        if (supplied[i] && request(v[i], r->run_id))
            rc = -1;
    }
    int stopped = r->runtime ? etude_observe_stop(r->runtime) : -1;
    char *summary = r->runtime ? etude_observe_summary(r->runtime, &r->owned_cpu) : NULL;
    if (!stopped && summary && start && runtime_start && end &&
        !keep_trace(r->store, r->path, r->run_id, runtime_start, end, summary, &pieces[0]))
        available[0] = 1;
    else {
        if (!pieces[0].length)
            cmp_printf(&pieces[0], "null");
        rc = -1;
    }
    free(summary);
    for (size_t i = 0; i < 2; i++) {
        const char *path = json_text(v[i], "trace");
        if (!supplied[i])
            cmp_printf(&pieces[i + 1], "null");
        else {
            for (unsigned wait = 0; path && !ended(path) && wait < 100; wait++)
                pause_sample();
            if (path && !keep_trace(r->store, path, r->run_id, start, end, NULL, &pieces[i + 1]))
                available[i + 1] = 1;
            else {
                if (!pieces[i + 1].length)
                    cmp_printf(&pieces[i + 1], "null");
                rc = -1;
            }
        }
    }
    for (size_t i = 0; i < 3; i++) {
        if (pieces[i].failed || !pieces[i].data) {
            for (size_t j = 0; j < 3; j++)
                free(pieces[j].data);
            for (size_t j = 0; j < 2; j++)
                json_free(v[j]);
            return -1;
        }
    }
    int eligible = r->cleanup_confirmed && etude_context_eligible(r->context);
    int complete =
        !rc && eligible && available[0] && available[1] && (!r->physical_required || available[2]);
    cmp_printf(&out,
               "{\"schema_version\":1,\"method\":\"native-load-v1\","
               "\"comparison_available\":%s,\"context_eligible\":%s,"
               "\"runtime_start_utc_us\":%llu,\"runtime\":%s,\"outer\":%s,"
               "\"physical\":%s,\"physical_required\":%s,\"preparation\":%s}\n",
               complete ? "true" : "false", eligible ? "true" : "false",
               (unsigned long long)runtime_start, pieces[0].data, pieces[1].data, pieces[2].data,
               r->physical_required ? "true" : "false",
               etude_context_text(r->context, "preparation")
                   ? etude_context_text(r->context, "preparation")
                   : "null");
    for (size_t i = 0; i < 3; i++)
        free(pieces[i].data);
    for (size_t i = 0; i < 2; i++)
        json_free(v[i]);
    if (out.failed) {
        free(out.data);
        return -1;
    }
    *json = out.data;
    return rc;
}
