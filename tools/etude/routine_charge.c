/* Verify a whole-PC lease and its post-sequence measured completion. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "routine_charge.h"
#include "etude/observe.h"
#include "history_json.h"
#include "sha256.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include "win/files.h"
#endif

#define ROUTINE_BYTES (64U * 1024U * 1024U)

struct proof {
    struct json *grant, *lease, *upstream, *completion, *route;
    struct json *run_document;
    const struct json *nodes;
    const char *run, *commit;
};
static char *proof_directory;
static int same(const char *a, const char *b)
{
    return a && b && !strcmp(a, b);
}
static int number(const struct json *v, const char *key, uint64_t *out)
{
    const struct json *n = json_member(v, key);
    if (!n || n->type != JSON_INTEGER || n->integer < 0)
        return 0;
    *out = (uint64_t)n->integer;
    return 1;
}
static int format(const struct json *v, const char *name)
{
    uint64_t version;
    return same(json_text(v, "format"), name) && number(v, "schema_version", &version) &&
           version == 1;
}
static int absolute(const char *path)
{
    return path && (path[0] == '/' ||
                    (strlen(path) >= 3 &&
                     ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
                     path[1] == ':' && (path[2] == '/' || path[2] == '\\')) ||
                    (path[0] == '\\' && path[1] == '\\'));
}
static void digest(const char *bytes, size_t size, char hex[65])
{
    struct sha256 h;
    unsigned char hash[32];
    sha256_init(&h);
    sha256_update(&h, (const unsigned char *)bytes, size);
    sha256_final(&h, hash);
    sha256_hex(hash, hex);
}
int etude_routine_proof_directory(const char *path)
{
    struct stat st;
    if (path && (!absolute(path) || lstat(path, &st) || !S_ISDIR(st.st_mode)))
        return -1;
    char *copy = path ? strdup(path) : NULL;
    if (path && !copy)
        return -1;
    free(proof_directory);
    proof_directory = copy;
    return 0;
}
static char *read_blob_file(const char *path, const char *expected, uint64_t length, size_t *size)
{
    struct stat st;
    if (lstat(path, &st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != length)
        return NULL;
    FILE *file = fopen(path, "rb");
    char *bytes = malloc((size_t)length + 1);
    int ok = file && bytes;
    if (ok) {
        *size = fread(bytes, 1, (size_t)length + 1, file);
        ok = *size == length && !ferror(file);
    }
    if (file && fclose(file))
        ok = 0;
    char actual[65];
    if (ok) {
        digest(bytes, *size, actual);
        ok = !strcmp(expected, actual);
        bytes[*size] = 0;
    }
    if (!ok) {
        free(bytes);
        return NULL;
    }
    return bytes;
}
char *etude_routine_bytes(const struct json *ref, size_t *size)
{
    const struct json *locations = json_member(ref, "locations");
    const char *expected = json_text(ref, "sha256");
    uint64_t length;
    if (!locations || locations->type != JSON_ARRAY || locations->count != 1 ||
        locations->item[0]->type != JSON_STRING || !absolute(locations->item[0]->string) ||
        !expected || strlen(expected) != 64 || strspn(expected, "0123456789abcdef") != 64 ||
        !number(ref, "size_bytes", &length) || length > ROUTINE_BYTES)
        return NULL;
    char path[4096];
    if (etude_routine_location(ref, path, sizeof(path)))
        return NULL;
    return read_blob_file(path, expected, length, size);
}
int etude_routine_location(const struct json *ref, char *path, size_t size)
{
    const char *hash = json_text(ref, "sha256");
    const struct json *locations = json_member(ref, "locations");
    if (!hash || strlen(hash) != 64 || strspn(hash, "0123456789abcdef") != 64 || !locations ||
        locations->type != JSON_ARRAY || locations->count != 1 ||
        locations->item[0]->type != JSON_STRING || !absolute(locations->item[0]->string))
        return -1;
    int n = proof_directory ? snprintf(path, size, "%s/%s", proof_directory, hash)
                            : snprintf(path, size, "%s", locations->item[0]->string);
    return n < 0 || (size_t)n >= size ? -1 : 0;
}

static struct json *document(const struct json *ref)
{
    size_t size = 0;
    char *bytes = etude_routine_bytes(ref, &size), error[160];
    struct json *doc = NULL;
    if (bytes)
        json_parse(bytes, size, 1, &doc, error, sizeof(error));
    free(bytes);
    return doc;
}
char *etude_routine_blob(const char *sha256, uint64_t size, const char *location, size_t *out)
{
    struct json hash = {.type = JSON_STRING, .name = "sha256", .string = (char *)sha256};
    struct json length = {.type = JSON_INTEGER, .name = "size_bytes", .integer = (long long)size};
    struct json path = {.type = JSON_STRING, .string = (char *)location};
    struct json *one[] = {&path};
    struct json paths = {.type = JSON_ARRAY, .name = "locations", .item = one, .count = 1};
    struct json *items[] = {&hash, &length, &paths};
    struct json ref = {.type = JSON_OBJECT, .item = items, .count = 3};
    return etude_routine_bytes(&ref, out);
}
struct json *etude_routine_document(const char *sha256, uint64_t size, const char *location)
{
    size_t length = 0;
    char *bytes = etude_routine_blob(sha256, size, location, &length), error[160];
    struct json *doc = NULL;
    if (bytes)
        json_parse(bytes, length, 1, &doc, error, sizeof(error));
    free(bytes);
    return doc;
}
static int reference(const struct json *a, const struct json *b)
{
    uint64_t x, y;
    char first[4096], second[4096];
    return !etude_routine_location(a, first, sizeof(first)) &&
           !etude_routine_location(b, second, sizeof(second)) &&
           same(json_text(a, "sha256"), json_text(b, "sha256")) && number(a, "size_bytes", &x) &&
           number(b, "size_bytes", &y) && x <= ROUTINE_BYTES && x == y;
}
static int identity(const struct json *a, const struct json *b)
{
    const char *pid = json_text(a, "pid"), *creation = json_text(a, "creation");
    return pid && *pid && *pid != '0' && strlen(pid) <= 20 && creation && *creation &&
           *creation != '0' && strlen(creation) <= 20 && strspn(pid, "0123456789") == strlen(pid) &&
           strspn(creation, "0123456789") == strlen(creation) && same(pid, json_text(b, "pid")) &&
           same(creation, json_text(b, "creation"));
}
static int clock_identity(const struct json *a, const struct json *b)
{
    return eh_identity(json_text(a, "machine_id")) && eh_identity(json_text(a, "boot_id")) &&
           same(json_text(a, "machine_id"), json_text(b, "machine_id")) &&
           same(json_text(a, "boot_id"), json_text(b, "boot_id"));
}
static int reading(const struct json *v, uint64_t *value, uint64_t *resolution)
{
    const char *utc = json_text(v, "utc");
    return eh_utc(utc) && number(v, "monotonic_us", value) &&
           number(v, "resolution_us", resolution) && *resolution;
}
static int same_event(const struct json *a, const struct json *b)
{
    return same(json_text(a, "run_id"), json_text(b, "run_id")) &&
           same(json_text(a, "commit"), json_text(b, "commit")) &&
           reference(json_member(a, "route"), json_member(b, "route"));
}
static int receipt_valid(const struct json *receipt, const char *commit)
{
    const char *record = json_text(receipt, "record_sha256");
    uint64_t start, end, error;
    return format(receipt, "etude-preparation-receipt") &&
           same(json_text(receipt, "commit"), commit) &&
           eh_identity(json_text(receipt, "run_id")) && clock_identity(receipt, receipt) &&
           record && strlen(record) == 64 && strspn(record, "0123456789abcdef") == 64 &&
           same(json_text(receipt, "outcome"), "pass") &&
           same(json_text(receipt, "cleanup"), "confirmed") &&
           reading(json_member(receipt, "start"), &start, &error) &&
           reading(json_member(receipt, "end"), &end, &error) && start <= end;
}
static int verified_bytes(const struct json *ref)
{
    size_t size = 0;
    char *bytes = etude_routine_bytes(ref, &size);
    int ok = bytes != NULL;
    free(bytes);
    return ok;
}
static int empty_ledger(const struct json *ref)
{
    size_t size = 0;
    char *bytes = etude_routine_bytes(ref, &size);
    int ok =
        bytes && size == strlen("etude-ledger 1\n") && !memcmp(bytes, "etude-ledger 1\n", size);
    free(bytes);
    return ok;
}
static int site_header(const char *bytes, size_t size)
{
    return bytes && size >= 13 && !memchr(bytes, 0, size) &&
           (!memcmp(bytes, "etude-site 1\n", 13) || !memcmp(bytes, "etude-site 2\n", 13));
}
static int disabled_policy(const struct json *before, const struct json *after)
{
    size_t a_size = 0, b_size = 0;
    char *a = etude_routine_bytes(before, &a_size), *b = etude_routine_bytes(after, &b_size);
    const char *old = a ? strstr(a, "\nendpoint ") : NULL;
    const char *disabled = b ? strstr(b, "\nendpoint disabled\n") : NULL;
    const char *end = old ? strchr(old + 1, '\n') : NULL;
    size_t prefix = old ? (size_t)(old - a) : 0;
    int ok = site_header(a, a_size) && site_header(b, b_size) && old && end && disabled &&
             !strstr(end, "\nendpoint ") && !strstr(disabled + 1, "\nendpoint ") &&
             (size_t)(disabled - b) == prefix && !memcmp(a, b, prefix) &&
             a_size - (size_t)(end - a) == b_size - prefix - strlen("\nendpoint disabled") &&
             !memcmp(end, disabled + strlen("\nendpoint disabled"), a_size - (size_t)(end - a));
    free(a);
    free(b);
    return ok;
}
static int endpoints(const struct json *lease)
{
    const struct json *list = json_member(lease, "endpoints");
    unsigned mask = 0;
    if (!list || list->type != JSON_ARRAY || list->count != 2)
        return 0;
    for (size_t i = 0; i < list->count; i++) {
        const struct json *e = list->item[i], *disabled = json_member(e, "normal_disabled");
        const char *name = json_text(e, "name");
        unsigned bit = same(name, "linux") ? 1 : same(name, "windows") ? 2 : 0;
        if (!bit || (mask & bit) || !disabled || disabled->type != JSON_BOOLEAN ||
            !disabled->integer || !empty_ledger(json_member(e, "ledger_empty")) ||
            !disabled_policy(json_member(e, "policy_before"), json_member(e, "policy_disabled")))
            return 0;
        mask |= bit;
    }
    return mask == 3;
}
static int field(const char *bytes, const char *key, char *out, size_t capacity)
{
    size_t key_size = strlen(key), matches = 0;
    for (const char *line = strchr(bytes, '\n'); line && *++line;) {
        const char *end = strchr(line, '\n');
        if (!end)
            return 0;
        if ((size_t)(end - line) > key_size && !memcmp(line, key, key_size) &&
            line[key_size] == ' ') {
            size_t n = (size_t)(end - line) - key_size - 1;
            if (!n || n >= capacity)
                return 0;
            memcpy(out, line + key_size + 1, n);
            out[n] = 0;
            matches++;
        }
        line = end;
    }
    return matches == 1;
}
static int text_header(const char *bytes, size_t size, const char *header, size_t fields)
{
    size_t lines = 0;
    if (!bytes || !size || bytes[size - 1] != '\n' || memchr(bytes, 0, size) ||
        size < strlen(header) || memcmp(bytes, header, strlen(header)))
        return 0;
    for (size_t i = 0; i < size; i++)
        lines += bytes[i] == '\n';
    return lines == fields + 1;
}
static int authority_binding(const struct proof *p, const struct json *authority)
{
    static const char *const paths[] = {"ledger",        "site",           "gate",
                                        "state",         "archive",        "program",
                                        "launcher_body", "physical_block", "route"};
    size_t size = 0;
    char *bytes = etude_routine_bytes(json_member(authority, "binding"), &size), value[4096];
    int ok = text_header(bytes, size, "etude-authority 1\n", 10);
    for (size_t i = 0; ok && i < sizeof(paths) / sizeof(paths[0]); i++)
        ok = field(bytes, paths[i], value, sizeof(value)) && absolute(value);
    ok = ok && field(bytes, "operator", value, sizeof(value)) &&
         ((!strncmp(value, "S-1-", 4) && strspn(value + 4, "0123456789-") == strlen(value + 4)) ||
          strspn(value, "0123456789") == strlen(value));
    const struct json *gate = json_member(authority, "gate");
    const struct json *paths_ref = json_member(json_member(p->grant, "route"), "locations");
    ok = ok && paths_ref && paths_ref->count == 1 && field(bytes, "route", value, sizeof(value)) &&
         same(value, paths_ref->item[0]->string) && field(bytes, "gate", value, sizeof(value)) &&
         same(value, json_text(gate, "path"));
    const struct json *id = json_member(gate, "identity");
    const char *device = json_text(id, "device"), *inode = json_text(id, "inode");
    ok = ok && device && *device && inode && *inode &&
         strspn(device, "0123456789") == strlen(device) &&
         strspn(inode, "0123456789") == strlen(inode);
    free(bytes);
    return ok;
}
static int snapshot_policy(const struct proof *p, const struct json *ref)
{
    const struct json *entries = json_member(p->lease, "endpoints");
    for (size_t i = 0; entries && i < entries->count; i++) {
        const struct json *e = entries->item[i];
        if (same(json_text(e, "name"), "windows"))
            return reference(ref, json_member(e, "policy_before")) && verified_bytes(ref);
    }
    return 0;
}
static int authority_state(const struct proof *p, const struct json *authority)
{
    size_t size = 0;
    char *bytes = etude_routine_bytes(json_member(authority, "state"), &size), value[4096];
    int ok = text_header(bytes, size, "etude-authority-state 1\n", 7) &&
             field(bytes, "run", value, sizeof(value)) && same(value, p->run) &&
             field(bytes, "commit", value, sizeof(value)) && same(value, p->commit) &&
             field(bytes, "phase", value, sizeof(value)) && same(value, "blocked") &&
             field(bytes, "owner_pid", value, sizeof(value)) &&
             same(value, json_text(json_member(p->grant, "owner"), "pid")) &&
             field(bytes, "owner_creation", value, sizeof(value)) &&
             same(value, json_text(json_member(p->grant, "owner"), "creation")) &&
             field(bytes, "nonce", value, sizeof(value)) && strlen(value) == 64 &&
             strspn(value, "0123456789abcdef") == 64 &&
             field(bytes, "snapshot", value, sizeof(value)) && absolute(value);
    const struct json *snapshot = json_member(authority, "snapshot");
    const struct json *locations = json_member(snapshot, "locations");
    ok = ok && locations && locations->type == JSON_ARRAY && locations->count == 1 &&
         locations->item[0]->type == JSON_STRING && same(value, locations->item[0]->string) &&
         snapshot_policy(p, snapshot);
    free(bytes);
    return ok;
}
static int lease(const struct proof *p)
{
    const struct json *result = json_member(p->lease, "result");
    const struct json *exclusive = json_member(result, "exclusive");
    const struct json *authority = json_member(p->lease, "authority");
    uint64_t start, error, grant, grant_error;
    return format(p->lease, "etude-routine-lease") && same_event(p->grant, p->lease) &&
           identity(json_member(p->grant, "owner"), json_member(p->lease, "owner")) &&
           clock_identity(p->grant, p->lease) &&
           reading(json_member(p->grant, "start"), &start, &error) &&
           reading(json_member(p->lease, "grant"), &grant, &grant_error) && start == grant &&
           error == grant_error &&
           json_equal(json_member(p->grant, "start"), json_member(p->lease, "grant")) &&
           same(json_text(result, "cleanup_before_grant"), "confirmed") && exclusive &&
           exclusive->type == JSON_BOOLEAN && exclusive->integer &&
           authority_binding(p, authority) && authority_state(p, authority) && endpoints(p->lease);
}
static int producer_cleanup(const struct json *ref, const struct json *receipt)
{
    struct json *doc = document(ref);
    const struct json *result = json_member(doc, "result");
    uint64_t end, value, exit_end, exit_value, cleanup, output, first, last, start, stop, error;
    int ok = format(doc, "etude-authority-child") && clock_identity(doc, receipt) &&
             identity(json_member(doc, "owner"), json_member(doc, "owner")) &&
             identity(json_member(doc, "process"), json_member(doc, "process")) &&
             identity(json_member(doc, "keeper"), json_member(doc, "keeper")) &&
             number(result, "end", &end) && !end && number(result, "value", &value) && !value &&
             number(result, "exit_end", &exit_end) && !exit_end &&
             number(result, "exit_value", &exit_value) && !exit_value &&
             number(result, "cleanup", &cleanup) && cleanup == 1 &&
             number(result, "output_error", &output) && !output &&
             reading(json_member(doc, "start"), &first, &error) &&
             reading(json_member(doc, "end"), &last, &error) &&
             reading(json_member(receipt, "start"), &start, &error) &&
             reading(json_member(receipt, "end"), &stop, &error) && first <= start && stop <= last;
    json_free(doc);
    return ok;
}
static int admission_pass(const struct json *ref);
static int producer(const struct proof *p)
{
    const struct json *event = p->upstream, *refs = json_member(event, "producer");
    struct json *receipt = document(json_member(refs, "receipt"));
    const char *mode = json_text(event, "mode");
    uint64_t start, end, error, grant;
    char expected[300];
    snprintf(expected, sizeof(expected), "%s:producer", p->run);
    const struct json *result = json_member(event, "result");
    const struct json *interval = json_member(event, "owned_interval");
    int ok = format(event, "etude-routine-upstream") && same_event(p->grant, event) &&
             same(json_text(result, "outcome"), "pass") &&
             same(json_text(result, "cleanup"), "confirmed") &&
             clock_identity(p->grant, interval) &&
             reading(json_member(interval, "start"), &start, &error) &&
             reading(json_member(interval, "end"), &end, &error) && end >= start &&
             reading(json_member(p->grant, "start"), &grant, &error) && grant <= start &&
             receipt_valid(receipt, p->commit) && same(mode, "produced") &&
             same(json_text(receipt, "run_id"), expected);
    static const char *const names[] = {"record", "admission", "cleanup"};
    for (size_t i = 0; ok && i < sizeof(names) / sizeof(names[0]); i++)
        ok = verified_bytes(json_member(refs, names[i]));
    ok =
        ok && admission_pass(json_member(refs, "admission")) &&
        producer_cleanup(json_member(refs, "cleanup"), receipt) &&
        same(json_text(receipt, "record_sha256"), json_text(json_member(refs, "record"), "sha256"));
    struct json *metadata = document(json_member(refs, "metadata"));
    const char *record = json_text(metadata, "record");
    char hash[65];
    int record_matches = 0;
    if (record) {
        digest(record, strlen(record), hash);
        record_matches = same(hash, json_text(receipt, "record_sha256"));
    }
    ok = ok && format(metadata, "etude-preparation-producer") &&
         same(json_text(metadata, "commit"), p->commit) &&
         json_equal(json_member(metadata, "receipt"), receipt) && record_matches;
    json_free(metadata);
    json_free(receipt);
    return ok;
}
static int import_mode(const struct proof *p)
{
    return same(json_text(p->route, "mode"), "windows-import");
}
static void close_proof(struct proof *p)
{
    json_free(p->grant);
    json_free(p->lease);
    json_free(p->upstream);
    json_free(p->completion);
    json_free(p->route);
    json_free(p->run_document);
}
static int open_proof(struct proof *p, const struct json *grant, const struct json *upstream)
{
    p->grant = document(grant);
    p->lease = document(json_member(p->grant, "lease"));
    p->route = document(json_member(p->grant, "route"));
    if (upstream && upstream->type != JSON_NULL)
        p->upstream = document(upstream);
    const char *completion = json_text(p->grant, "completion_path");
    return format(p->grant, "etude-routine-grant") && format(p->route, "etude-routine-route") &&
           same(json_text(p->grant, "run_id"), p->run) &&
           same(json_text(p->grant, "commit"), p->commit) && absolute(completion) &&
           same(json_text(p->grant, "phase"), "physical-capacity-granted") && lease(p) &&
           (import_mode(p)
                ? reference(json_member(p->upstream, "routine_grant"), grant) && producer(p)
                : same(json_text(p->route, "mode"), "wsl-native") &&
                      (!upstream || upstream->type == JSON_NULL));
}
int etude_routine_context(const struct json *grant, const struct json *upstream, const char *run_id,
                          const char *commit)
{
    struct proof p = {0};
    p.run = run_id;
    p.commit = commit;
    int ok = open_proof(&p, grant, upstream);
    struct stat st;
    const char *path = json_text(p.grant, "completion_path");
    ok = ok && lstat(path, &st) != 0 && errno == ENOENT;
    close_proof(&p);
    return ok ? 0 : -1;
}

static struct json *completion_document(const char *path)
{
    struct stat st;
    struct json *doc = NULL;
    char error[160];
    FILE *file = NULL;
    char *bytes = NULL;
    if (!absolute(path) || lstat(path, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > 1048576)
        return NULL;
    file = fopen(path, "rb");
    bytes = malloc((size_t)st.st_size + 1);
    if (file && bytes) {
        size_t n = fread(bytes, 1, (size_t)st.st_size + 1, file);
        if (!ferror(file) && n == (size_t)st.st_size)
            json_parse(bytes, n, 1, &doc, error, sizeof(error));
    }
    if (file && fclose(file)) {
        json_free(doc);
        doc = NULL;
    }
    free(bytes);
    return doc;
}
int etude_routine_work_us(const char *path, const char *purpose, uint64_t *work_us)
{
    if (!same(purpose, "push") && !same(purpose, "ci") && !same(purpose, "release") &&
        !same(purpose, "phase_first_run"))
        return -1;
    struct json *doc = completion_document(path);
    const struct json *cls = json_member(json_member(doc, "classes"), purpose);
    uint64_t version, goal, growth, ceiling, reserve;
    int ok = number(doc, "schema_version", &version) && version == 1 &&
             same(json_text(doc, "policy"), "budget-v1") && number(cls, "goal_us", &goal) &&
             number(cls, "growth_us", &growth) && number(cls, "ceiling_us", &ceiling) &&
             number(cls, "cleanup_reserve_us", &reserve) && goal && goal <= ceiling &&
             growth <= ceiling - goal && reserve < ceiling;
    if (ok)
        *work_us = ceiling - reserve;
    json_free(doc);
    return ok ? 0 : -1;
}
static int result_pass(const struct json *result)
{
    uint64_t value, active, exit_value;
    uint64_t output_error;
    return same(json_text(result, "end"), "exited") && number(result, "value", &value) && !value &&
           same(json_text(result, "exit_end"), "exited") &&
           number(result, "exit_value", &exit_value) && !exit_value &&
           number(result, "active", &active) && !active &&
           same(json_text(result, "cleanup"), "confirmed") &&
           number(result, "output_error", &output_error) && !output_error;
}
static int admission_pass(const struct json *ref)
{
    size_t size = 0;
    char *bytes = etude_routine_bytes(ref, &size);
    int ok = bytes && size == strlen("exited 0\n") && !memcmp(bytes, "exited 0\n", size);
    free(bytes);
    return ok;
}
struct phase_window {
    unsigned bit;
    uint64_t start, end;
};
static int local_phases(const struct proof *p, const struct phase_window times[3])
{
    const struct json *receipt = json_member(p->nodes, "preparation");
    const struct json *time = json_member(json_member(p->run_document, "data"), "time");
    uint64_t start, end, error, run_end;
    return receipt_valid(receipt, p->commit) && same(json_text(receipt, "run_id"), p->run) &&
           same(json_text(receipt, "commit"), p->commit) &&
           same(json_text(receipt, "outcome"), "pass") &&
           same(json_text(receipt, "cleanup"), "confirmed") &&
           clock_identity(json_member(time, "clock"), receipt) &&
           reading(json_member(receipt, "start"), &start, &error) &&
           reading(json_member(receipt, "end"), &end, &error) && times[1].start <= start &&
           start <= end && end <= times[1].end && number(time, "end_us", &run_end) &&
           times[2].start <= run_end && run_end <= times[2].end;
}
static int consumer_producer(const struct proof *p)
{
    if (!import_mode(p))
        return 1;
    const struct json *receipt = json_member(p->nodes, "preparation");
    const struct json *conditioning = json_member(receipt, "conditioning");
    struct json *proof = document(json_member(conditioning, "provenance"));
    const struct json *upstream = json_member(p->upstream, "producer");
    struct json *original = document(json_member(upstream, "receipt"));
    int ok = same(json_text(conditioning, "backend"), "import") &&
             format(proof, "etude-preparation-import-provenance") &&
             same(json_text(proof, "commit"), p->commit) &&
             same(json_text(proof, "producer_sha256"),
                  json_text(json_member(upstream, "metadata"), "sha256")) &&
             json_equal(json_member(proof, "ordinary_production"), original);
    json_free(proof);
    json_free(original);
    return ok;
}
static int producer_phase(const struct proof *p, const struct json *phase, const struct json *raw)
{
    const struct json *source = json_member(p->upstream, "producer");
    struct json *receipt = document(json_member(source, "receipt"));
    int ok = import_mode(p) &&
             reference(json_member(phase, "receipt"), json_member(source, "cleanup")) &&
             producer_cleanup(json_member(phase, "receipt"), receipt) &&
             clock_identity(phase, receipt) && format(raw, "etude-authority-child");
    json_free(receipt);
    return ok;
}
static int phase_receipt(const struct proof *p, const struct json *phase)
{
    struct json *raw = document(json_member(phase, "receipt"));
    const struct json *owner = json_member(raw, "owner");
    int is_producer = same(json_text(phase, "phase"), "producer");
    int ok = identity(json_member(raw, "process"), json_member(phase, "child")) &&
             identity(owner, owner) && clock_identity(raw, phase) &&
             json_equal(json_member(raw, "start"), json_member(phase, "start")) &&
             json_equal(json_member(raw, "end"), json_member(phase, "end"));
    if (is_producer)
        ok = ok && producer_phase(p, phase, raw);
    else
        ok = ok && format(raw, "etude-routine-child") && result_pass(json_member(raw, "result")) &&
             json_equal(json_member(raw, "result"), json_member(phase, "result")) &&
             (!import_mode(p) || identity(owner, json_member(p->grant, "owner")));
    json_free(raw);
    return ok;
}
static int phase_clock(const struct proof *p, const struct json *doc,
                       const struct phase_window *window)
{
    if (window->bit == 1)
        return import_mode(p); /* The raw producer receipt checks its Linux clock. */
    if (!import_mode(p))
        return clock_identity(doc, json_member(p->nodes, "preparation"));
    uint64_t first, last, error;
    return clock_identity(doc, p->grant) &&
           reading(json_member(p->grant, "start"), &first, &error) &&
           reading(json_member(p->completion, "end"), &last, &error) && first <= window->start &&
           window->end <= last;
}
static int phase(const struct proof *p, const struct json *ref, struct phase_window *window)
{
    struct json *doc = document(ref);
    const char *name = json_text(doc, "phase");
    window->bit = same(name, "producer")      ? 1
                  : same(name, "preparation") ? 2
                  : same(name, "collection")  ? 4
                                              : 0;
    uint64_t error;
    int ok = window->bit && format(doc, "etude-routine-phase") && same_event(doc, p->grant) &&
             reference(json_member(doc, "lease"), json_member(p->grant, "lease")) &&
             identity(json_member(doc, "owner"), json_member(p->grant, "owner")) &&
             reading(json_member(doc, "start"), &window->start, &error) &&
             reading(json_member(doc, "end"), &window->end, &error) &&
             window->end >= window->start && result_pass(json_member(doc, "result")) &&
             admission_pass(json_member(doc, "admission")) && phase_receipt(p, doc);
    ok = ok && phase_clock(p, doc, window);
    if (window->bit == 4)
        ok = ok && identity(json_member(doc, "child"), json_member(p->completion, "collector"));
    else
        ok = ok && identity(json_member(doc, "child"), json_member(doc, "child"));
    json_free(doc);
    return ok;
}
static int phases(const struct proof *p)
{
    const struct json *list = json_member(p->completion, "phase_cleanup");
    unsigned mask = 0;
    struct phase_window times[3] = {0};
    size_t expected = import_mode(p) ? 3 : 2;
    if (!list || list->type != JSON_ARRAY || list->count != expected)
        return 0;
    for (size_t i = 0; i < list->count; i++) {
        struct phase_window window = {0};
        if (!phase(p, list->item[i], &window) || (mask & window.bit))
            return 0;
        unsigned at = window.bit == 1 ? 0 : window.bit == 2 ? 1 : 2;
        times[at] = window;
        mask |= window.bit;
    }
    if (mask != (import_mode(p) ? 7U : 6U) || times[1].end > times[2].start ||
        !local_phases(p, times))
        return 0;
    uint64_t end, error, producer_end;
    if (import_mode(p))
        return reading(json_member(json_member(p->upstream, "owned_interval"), "end"),
                       &producer_end, &error) &&
               producer_end <= times[1].start &&
               reading(json_member(p->completion, "preparation_end"), &end, &error) &&
               times[1].end <= end && end <= times[2].start;
    return 1;
}
static int collector(const struct proof *p)
{
    const struct json *data = json_member(p->run_document, "data");
    struct json *load = document(json_member(data, "load")), *header = NULL;
    size_t size = 0;
    char *bytes = etude_routine_bytes(json_member(json_member(load, "runtime"), "trace"), &size);
    const char *newline = bytes ? memchr(bytes, '\n', size) : NULL;
    char error[160];
    if (newline)
        json_parse(bytes, (size_t)(newline - bytes), 1, &header, error, sizeof(error));
    const struct json *id = json_member(p->completion, "collector");
    uint64_t pid;
    int ok = same(json_text(header, "kind"), "start") &&
             same(json_text(header, "method"), "native-load-v1") &&
             same(json_text(header, "run_id"), p->run) &&
             clock_identity(header, json_member(json_member(data, "time"), "clock")) &&
             number(header, "root_pid", &pid) && json_text(id, "pid") &&
             pid == strtoull(json_text(id, "pid"), NULL, 10) &&
             same(json_text(header, "root_creation"), json_text(id, "creation"));
    free(bytes);
    json_free(header);
    json_free(load);
    return ok;
}
static int final_ledgers(const struct proof *p)
{
    const struct json *list = json_member(p->completion, "ledgers");
    unsigned mask = 0;
    if (!list || list->type != JSON_ARRAY || list->count != 2)
        return 0;
    for (size_t i = 0; i < list->count; i++) {
        const char *name = json_text(list->item[i], "endpoint");
        unsigned bit = same(name, "linux") ? 1 : same(name, "windows") ? 2 : 0;
        if (!bit || (mask & bit) || !empty_ledger(json_member(list->item[i], "claims")))
            return 0;
        mask |= bit;
    }
    return mask == 3;
}
static int observers(const struct proof *p, struct etude_routine_cost *out)
{
    const struct json *refs = json_member(p->completion, "observers");
    uint64_t start = etude_observe_utc(json_text(json_member(p->grant, "start"), "utc"));
    uint64_t end = etude_observe_utc(json_text(json_member(p->completion, "end"), "utc"));
    uint64_t prep =
        etude_observe_utc(json_text(json_member(p->completion, "preparation_end"), "utc"));
    int physical = 0;
    if (!refs || refs->type != JSON_ARRAY || !refs->count || refs->count > 8)
        return 0;
    for (size_t i = 0; i < refs->count; i++) {
        size_t size = 0;
        char *bytes = etude_routine_bytes(refs->item[i], &size), error[160];
        const char *newline = bytes ? memchr(bytes, '\n', size) : NULL;
        struct json *header = NULL, *summary = NULL;
        if (newline)
            json_parse(bytes, (size_t)(newline - bytes), 1, &header, error, sizeof(error));
        uint64_t pid;
        int match = same(json_text(header, "platform"), "windows-native-counters-v1") &&
                    clock_identity(header, p->grant) && number(header, "root_pid", &pid) &&
                    pid == strtoull(json_text(json_member(p->grant, "owner"), "pid"), NULL, 10) &&
                    same(json_text(header, "root_creation"),
                         json_text(json_member(p->grant, "owner"), "creation"));
        char *text =
            header ? etude_observe_read(bytes, size, p->run, match ? start : 0, match ? end : 0)
                   : NULL;
        if (text)
            json_parse(text, strlen(text), 1, &summary, error, sizeof(error));
        const struct json *known = json_member(summary, "comparison_available");
        if (match && summary) {
            physical++;
            out->load_available = known && known->type == JSON_BOOLEAN && known->integer;
            char *prefix = etude_observe_read(bytes, size, p->run, start, prep);
            struct json *prepared = NULL;
            if (prefix)
                json_parse(prefix, strlen(prefix), 1, &prepared, error, sizeof(error));
            const struct json *available = json_member(prepared, "comparison_available");
            out->preparation_load_available =
                available && available->type == JSON_BOOLEAN && available->integer;
            json_free(prepared);
            free(prefix);
        }
        int ok = summary != NULL;
        free(bytes);
        free(text);
        json_free(header);
        json_free(summary);
        if (!ok)
            return 0;
    }
    return physical == 1;
}
static int history_record(const char *line, size_t size, const unsigned char *expected)
{
    if (!expected)
        return 1;
    char actual[65], hex[65];
    digest(line, size, actual);
    sha256_hex(expected, hex);
    return !strcmp(actual, hex);
}
static int history(struct proof *p, const struct etude_routine_input *in)
{
    size_t size = 0;
    const struct json *ref = json_member(p->completion, "history");
    char *bytes = etude_routine_bytes(ref, &size), error[160];
    int plan = 0, run = 0;
    size_t at = 0;
    for (; bytes && at < size;) {
        const char *line = bytes + at, *newline = memchr(line, '\n', size - at);
        if (!newline)
            break;
        size_t n = (size_t)(newline - line) + 1;
        struct json *doc = NULL;
        if (json_parse(line, n, 1, &doc, error, sizeof(error)))
            break;
        if (same(json_text(doc, "run_id"), p->run)) {
            const char *kind = json_text(doc, "kind");
            if (same(kind, "plan"))
                plan += same(json_text(json_member(doc, "data"), "commit"), p->commit) &&
                        history_record(line, n - 1, in->plan_digest);
            if (same(kind, "run")) {
                run += history_record(line, n - 1, in->run_digest);
                json_free(p->run_document);
                p->run_document = doc;
                doc = NULL;
            }
        }
        json_free(doc);
        at += n;
    }
    int complete = bytes && at == size;
    free(bytes);
    if (in->history_path) {
        uint64_t length;
        if (!number(ref, "size_bytes", &length))
            return 0;
        char *actual = read_blob_file(in->history_path, json_text(ref, "sha256"), length, &size);
        int ok = actual != NULL;
        free(actual);
        if (!ok)
            return 0;
    }
    return complete && plan == 1 && run == 1;
}
static int terminal(struct proof *p, const struct etude_routine_input *in,
                    struct etude_routine_cost *out)
{
    const struct json *c = p->completion, *result = json_member(c, "result");
    uint64_t start, prep, end, a, b, ignored;
    if (!format(c, "etude-routine-completion") || !same_event(c, p->grant) ||
        !reference(json_member(c, "routine_grant"), json_member(in->nodes, "routine_grant")) ||
        !reference(json_member(c, "lease"), json_member(p->grant, "lease")) ||
        !identity(json_member(c, "owner"), json_member(p->grant, "owner")) ||
        !clock_identity(c, p->grant) || !same(json_text(result, "outcome"), "pass") ||
        !same(json_text(result, "cleanup"), "confirmed") ||
        !reading(json_member(p->grant, "start"), &start, &a) ||
        !reading(json_member(c, "preparation_end"), &prep, &ignored) ||
        !reading(json_member(c, "end"), &end, &b) || start > prep || prep > end ||
        !history(p, in) || !phases(p) || !consumer_producer(p) || !collector(p) ||
        !final_ledgers(p) || !observers(p, out))
        return 0;
    out->charged_us = end - start;
    out->preparation_us = prep - start;
    out->error_us = a + b;
    return 1;
}
int etude_routine_charge(const struct etude_routine_input *in, struct etude_routine_cost *out)
{
    const struct json *grant = json_member(in->nodes, "routine_grant");
    const struct json *upstream = json_member(in->nodes, "upstream_preparation");
    memset(out, 0, sizeof(*out));
    if ((!grant || grant->type == JSON_NULL) && (!upstream || upstream->type == JSON_NULL))
        return in->completion_path ? -1 : 0;
    const char *hint = json_text(in->nodes, "routine_proofs");
    int own_resolver = !proof_directory && hint;
    if (own_resolver && etude_routine_proof_directory(hint))
        return -1;
    struct proof p = {0};
    p.run = in->run_id;
    p.commit = in->commit;
    p.nodes = in->nodes;
    int ok = open_proof(&p, grant, upstream);
    const char *path = json_text(p.grant, "completion_path");
    ok = ok && (!in->completion_path || proof_directory || same(path, in->completion_path));
    p.completion =
        ok ? completion_document(in->completion_path ? in->completion_path : path) : NULL;
    ok = ok && same(path, json_text(p.completion, "completion_path"));
    ok = ok && terminal(&p, in, out);
    close_proof(&p);
    if (own_resolver)
        etude_routine_proof_directory(NULL);
    return ok ? 1 : -1;
}
int etude_routine_plan(const struct json *plan, const struct etude_routine_input *input,
                       struct etude_routine_cost *cost)
{
    const struct json *refs = json_member(plan, "artifacts");
    for (size_t i = 0; refs && i < refs->count; i++) {
        struct json *doc = document(refs->item[i]);
        int rc = 0;
        if (format(doc, "etude-preparation-nodes")) {
            struct etude_routine_input in = *input;
            in.nodes = doc;
            rc = etude_routine_charge(&in, cost);
        }
        json_free(doc);
        if (rc)
            return rc;
    }
    return input->completion_path ? -1 : 0;
}
