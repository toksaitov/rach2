/* Follow retained proof locators and require complete successful histories. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#endif
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "retention.h"

static int retained_hash(const char *hash)
{
    return hash && strlen(hash) == 64 && strspn(hash, "0123456789abcdef") == 64;
}

static struct json *ret_resolve(const struct json *ref)
{
    const char *hash = json_text(ref, "sha256");
    const struct json *locations = json_member(ref, "locations");
    int64_t size = ret_number(ref, "size_bytes");
    if (!retained_hash(hash) || size < 0 || !locations || locations->type != JSON_ARRAY ||
        !locations->count) {
        ret_fail("malformed retained blob reference");
        return NULL;
    }
    for (size_t i = 0; i < locations->count; i++) {
        const struct json *location = locations->item[i];
        if (location->type != JSON_STRING || strstr(location->string, "://") ||
            !ret_absolute(location->string))
            continue;
        struct json *actual = ret_descriptor(location->string);
        if (actual && ret_number(actual, "size_bytes") == size &&
            !strcmp(json_text(actual, "sha256"), hash))
            return actual;
        json_free(actual);
    }
    ret_fail("no retained blob location satisfies its digest");
    return NULL;
}

static int ret_references(const struct json *value, struct json *pending)
{
    if (!value)
        return -1;
    if (value->type == JSON_OBJECT && json_member(value, "sha256") &&
        json_member(value, "size_bytes") && json_member(value, "locations")) {
        struct json *actual = ret_resolve(value);
        if (!actual || ret_push(pending, actual))
            return -1;
    }
    if (value->type == JSON_OBJECT || value->type == JSON_ARRAY)
        for (size_t i = 0; i < value->count; i++)
            if (ret_references(value->item[i], pending))
                return -1;
    return 0;
}

static int ret_proof_lines(const char *path, struct json *pending)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;
    char *line = NULL, error[256];
    size_t capacity = 0;
    ssize_t size;
    int rc = 0;
    while ((size = getline(&line, &capacity, file)) >= 0) {
        struct json *value = NULL;
        if (!size || line[size - 1] != '\n' ||
            json_parse_data(line, (size_t)size, 1, &value, error, sizeof error) ||
            ret_references(value, pending))
            rc = -1;
        json_free(value);
        if (rc)
            break;
    }
    if (ferror(file))
        rc = -1;
    free(line);
    fclose(file);
    return rc ? ret_fail("incomplete or unreadable retained stream: %s", path) : 0;
}

static int ret_proof_document(const char *path, struct json *pending)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;
    unsigned char prefix[256];
    size_t n = fread(prefix, 1, sizeof prefix, file), at = 0;
    int rc = ferror(file);
    fclose(file);
    if (rc)
        return -1;
    while (at < n && strchr(" \t\r\n", prefix[at]))
        at++;
    if (at == n || (prefix[at] != '{' && prefix[at] != '['))
        return 0;
    n = strlen(path);
    if (n >= 6 && !strcmp(path + n - 6, ".jsonl"))
        return ret_proof_lines(path, pending);
    struct json *document = ret_document(path);
    rc = !document || ret_references(document, pending);
    json_free(document);
    return rc;
}

struct json *ret_closure(const struct json *roots)
{
    if (!roots || roots->type != JSON_ARRAY) {
        ret_fail("retained proof roots are not an array");
        return NULL;
    }
    struct json *pending = ret_new(JSON_ARRAY), *result = ret_new(JSON_OBJECT);
    int rc = 0;
    for (size_t i = 0; !rc && i < roots->count; i++) {
        char *path = ret_checked(roots->item[i]);
        struct json *actual = path ? ret_new(JSON_OBJECT) : NULL;
        if (actual) {
            ret_set(actual, "path", ret_string(path));
            ret_set(actual, "sha256", ret_clone(json_member(roots->item[i], "sha256")));
            ret_set(actual, "size_bytes", ret_clone(json_member(roots->item[i], "size_bytes")));
        }
        rc = !actual || ret_push(pending, actual);
        free(path);
    }
    for (size_t at = 0; !rc && at < pending->count; at++) {
        struct json *ref = pending->item[at];
        const char *path = json_text(ref, "path");
        const struct json *seen = json_member(result, path);
        if (seen) {
            if (!json_equal(seen, ref))
                rc = ret_fail("retained proof identity changed");
            continue;
        }
        rc = ret_set(result, path, ret_clone(ref)) || ret_proof_document(path, pending);
    }
    json_free(pending);
    if (rc) {
        json_free(result);
        result = NULL;
    }
    return result;
}

static int retained_phase_tail(const char *text)
{
    const char *forms[] = {" status 0 cleanup confirmed", " status 0 active 0"};
    for (size_t i = 0; i < 2; i++) {
        size_t n = strlen(forms[i]);
        if (!strncmp(text, forms[i], n) && (!text[n] || text[n] == ' '))
            return 1;
    }
    return 0;
}

static int retained_phase(const char *line, const char *run, size_t phases[2])
{
    int rc = 0;
    const char *names[] = {"preparation", "collection"};
    for (size_t i = 0; i < 2; i++) {
        char *prefix = ret_format("phase-end %s %s ", run, names[i]);
        if (!strncmp(line, prefix, strlen(prefix))) {
            char *end = NULL;
            strtoull(line + strlen(prefix), &end, 10);
            if (!end || end == line + strlen(prefix) || !retained_phase_tail(end))
                rc = -1;
            phases[i]++;
        }
        free(prefix);
    }
    return rc;
}

static int retained_result_files(const char *bundle)
{
    char *status = ret_format("%s/sequence.status", bundle),
         *failed = ret_format("%s/failed", bundle);
    char bytes[8];
    FILE *file = status ? fopen(status, "rb") : NULL;
    size_t n = file ? fread(bytes, 1, sizeof bytes, file) : 0;
    if (file)
        fclose(file);
    struct stat st;
    int rc = !file || n != 2 || memcmp(bytes, "0\n", 2) || !failed || lstat(failed, &st) ||
             !S_ISREG(st.st_mode) || st.st_size;
    free(status);
    free(failed);
    if (rc)
        return ret_fail("routine sequence or verdict did not pass");
    return 0;
}

static int retained_receipt(const char *bundle, const char *run)
{
    if (retained_result_files(bundle))
        return -1;
    int rc = 0;
    char *path = ret_format("%s/sequence.receipt", bundle);
    FILE *file = path ? fopen(path, "rb") : NULL;
    free(path);
    if (!file)
        return ret_fail("routine sequence receipt is unavailable");
    char *line = NULL, *tail[5] = {0};
    size_t capacity = 0, count = 0, phases[2] = {0}, runs = 0;
    ssize_t size;
    while ((size = getline(&line, &capacity, file)) >= 0) {
        if (!size || line[size - 1] != '\n') {
            rc = -1;
            break;
        }
        line[--size] = 0;
        free(tail[count % 5]);
        tail[count++ % 5] = strdup(line);
        if (!strncmp(line, "run ", 4)) {
            if (strcmp(line + 4, run))
                rc = -1;
            runs++;
        }
        if (retained_phase(line, run, phases))
            rc = -1;
    }
    if (ferror(file) || runs != 1 || phases[0] != 1 || phases[1] != 1 || count < 5)
        rc = -1;
    const char *expected[] = {"error 0", "cancelled 0", "status 0", "active 0",
                              "cleanup confirmed"};
    for (size_t i = 0; i < 5; i++) {
        if (count < 5 || !tail[(count + i) % 5] || strcmp(tail[(count + i) % 5], expected[i]))
            rc = -1;
        free(tail[(count + i) % 5]);
    }
    free(line);
    fclose(file);
    return rc ? ret_fail("sequence cleanup or run identity is unconfirmed") : 0;
}

static int retained_record(const struct json *record, struct json *ids, struct json *sequence,
                           struct json *plans, struct json *runs)
{
    const char *id = json_text(record, "record_id"), *stream = json_text(record, "stream_id");
    const char *run = json_text(record, "run_id"), *kind = json_text(record, "kind");
    int64_t at = ret_number(record, "sequence");
    if (!id || !stream || !run || !kind || ret_has(ids, id) || at < 0)
        return ret_fail("duplicate or malformed retained record identity");
    int64_t previous = ret_number(sequence, stream);
    if (at != (previous < 0 ? 0 : previous))
        return ret_fail("retained stream sequence gap");
    ret_set(sequence, stream, ret_integer(at + 1));
    ret_add_name(ids, id);
    if (!strcmp(kind, "plan"))
        return ret_add_name(plans, run);
    if (!strcmp(kind, "run")) {
        const struct json *result = json_member(json_member(record, "data"), "result");
        const char *cleanup = json_text(result, "cleanup"), *outcome = json_text(result, "outcome");
        if (ret_has(runs, run) || !cleanup || strcmp(cleanup, "confirmed") || !outcome ||
            strcmp(outcome, "pass"))
            return ret_fail("failed, duplicate or uncertain terminal run");
        return ret_add_name(runs, run);
    }
    return 0;
}

static int retained_stream(const char *path, struct json *ids, struct json *plans,
                           struct json *runs)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;
    struct json *sequence = ret_new(JSON_OBJECT);
    char *line = NULL, error[256];
    size_t capacity = 0;
    ssize_t size;
    int rc = 0;
    while ((size = getline(&line, &capacity, file)) >= 0) {
        struct json *record = NULL;
        if (!size || line[size - 1] != '\n' ||
            json_parse(line, (size_t)size, 1, &record, error, sizeof error) ||
            retained_record(record, ids, sequence, plans, runs))
            rc = -1;
        json_free(record);
        if (rc)
            break;
    }
    if (ferror(file))
        rc = -1;
    free(line);
    json_free(sequence);
    fclose(file);
    return rc;
}

int ret_terminal(const char *bundle, const char *run)
{
    if (retained_receipt(bundle, run))
        return -1;
    char *history = ret_format("%s/history", bundle);
    DIR *dir = history ? opendir(history) : NULL;
    struct json *ids = ret_new(JSON_ARRAY), *plans = ret_new(JSON_ARRAY),
                *runs = ret_new(JSON_ARRAY);
    struct dirent *entry;
    size_t streams = 0;
    int rc = !dir;
    while (!rc && (entry = ret_next(dir, &rc))) {
        size_t n = strlen(entry->d_name);
        if (n < 6 || strcmp(entry->d_name + n - 6, ".jsonl"))
            continue;
        char *path = ret_format("%s/%s", history, entry->d_name);
        rc = !path || retained_stream(path, ids, plans, runs);
        free(path);
        streams++;
    }
    if (dir)
        closedir(dir);
    if (!streams || runs->count != 1 || plans->count != 1 || !ret_has(runs, run) ||
        !ret_has(plans, run))
        rc = -1;
    json_free(ids);
    json_free(plans);
    json_free(runs);
    free(history);
    return rc ? ret_fail("retained history lacks its complete successful terminal run") : 0;
}
