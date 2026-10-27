/* Exercise the native registry only in a fresh owned fixture directory. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#endif
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "fixdir_host.c"
#include "../../../tools/etude/retention.h"
#include "../../../tools/etude/include/etude/exec.h"
#include "../../../utils/sha256.h"

static char retention_base[4096], retention_policy[4096], retention_site[4096],
    retention_issues[4096];
static char retention_root[4096], retention_self[4096];
static struct json *fixture_policy;
static int retention_failures;
static unsigned retention_case;
#define RET_CHECK(test)                                                                   \
    do {                                                                                  \
        if (!(test)) {                                                                    \
            fprintf(stderr, "retention fixture: %s:%d: %s\n", __FILE__, __LINE__, #test); \
            retention_failures++;                                                         \
        }                                                                                 \
    } while (0)

static void retention_put(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file || fputs(text, file) < 0 || fclose(file))
        exit(2);
}
static void retention_hash(const char *text, char hex[65])
{
    struct sha256 hash;
    unsigned char raw[32];
    sha256_init(&hash);
    sha256_update(&hash, (const unsigned char *)text, strlen(text));
    sha256_final(&hash, raw);
    sha256_hex(raw, hex);
}

int etude_test_retention_child(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "--retention-child"))
        return etude_retention_main(argc - 2, argv + 2, NULL);
    const char *base = strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    if (!strcmp(base, "gh")) {
        if (getenv("RETENTION_GH_FAIL"))
            return 1;
        if (argc > 1 && strstr(argv[argc - 1], "/comments?")) {
            puts("[[]]");
            return 0;
        }
        const char *path = getenv("RETENTION_GH_RESPONSE");
        FILE *file = path ? fopen(path, "rb") : NULL;
        if (!file)
            return 2;
        char bytes[4096];
        size_t n;
        while ((n = fread(bytes, 1, sizeof bytes, file)))
            fwrite(bytes, 1, n, stdout);
        int rc = ferror(file);
        fclose(file);
        return rc;
    }
    if (argc > 1 && !strcmp(argv[1], "estimate")) {
        if (argc < 9 || strcmp(argv[1], "estimate") || strcmp(argv[2], "-s") ||
            strcmp(argv[4], "forecast") || strcmp(argv[6], "release"))
            return 99;
        puts("native reference forecast");
        return getenv("RETENTION_REFERENCE_FAIL") ? 1 : 0;
    }
    return -1;
}

static int retention_call(const char *action, const char *const extra[])
{
    RET_CHECK(!ret_write(retention_policy, fixture_policy, 1));
    char *argv[32] = {"retention", "--policy",     retention_policy,
                      "--site",    retention_site, (char *)action};
    size_t count = 6;
    for (size_t i = 0; extra && extra[i]; i++)
        argv[count++] = (char *)extra[i];
    argv[count] = NULL;
    return etude_retention_main((int)count, argv, NULL);
}

static void retention_snapshot(void)
{
    char captured[64], digest[65];
    ret_stamp(captured);
    retention_hash("[]", digest);
    struct json *snapshot = ret_new(JSON_OBJECT);
    ret_set(snapshot, "format", ret_string("etude-retention-open-issues"));
    ret_set(snapshot, "schema_version", ret_integer(1));
    ret_set(snapshot, "repository", ret_string("fixture/retention"));
    ret_set(snapshot, "captured_utc", ret_string(captured));
    ret_set(snapshot, "complete", ret_boolean(1));
    ret_set(snapshot, "issue_set_sha256", ret_string(digest));
    ret_set(snapshot, "issues", ret_new(JSON_ARRAY));
    ret_set(snapshot, "unknown", ret_new(JSON_ARRAY));
    RET_CHECK(!ret_write(retention_issues, snapshot, 1));
    json_free(snapshot);
    ret_set(fixture_policy, "open_issues", ret_descriptor(retention_issues));
}

static void retention_setup(void)
{
    json_free(fixture_policy);
    snprintf(retention_root, sizeof retention_root, "%s/case-%u/evidence", retention_base,
             ++retention_case);
    char *parent = ret_format("%s/case-%u", retention_base, retention_case);
    RET_CHECK(parent && !mkdir(parent, 0700));
    free(parent);
    fixture_policy = ret_new(JSON_OBJECT);
    ret_set(fixture_policy, "format", ret_string("etude-retention-policy"));
    ret_set(fixture_policy, "schema_version", ret_integer(1));
    ret_set(fixture_policy, "root", ret_string(retention_root));
    struct json *site = ret_descriptor(retention_site);
    ret_set(fixture_policy, "site_sha256", ret_clone(json_member(site, "sha256")));
    json_free(site);
    ret_set(fixture_policy, "max_bytes", ret_integer(1000000));
    ret_set(fixture_policy, "max_age_days", ret_integer(1));
    ret_set(fixture_policy, "reserve_bytes", ret_integer(10000));
    ret_set(fixture_policy, "issue_snapshot_max_age_seconds", ret_integer(3600));
    ret_set(fixture_policy, "baselines", ret_new(JSON_ARRAY));
    ret_set(fixture_policy, "anchors", ret_new(JSON_ARRAY));
    retention_snapshot();
    RET_CHECK(!retention_call("init", NULL));
}

static char *retention_allocate(void)
{
    char *env = ret_format("%s/allocated.env", retention_base);
    const char *extra[] = {"--github-env", env, NULL};
    int rc = retention_call("allocate", extra);
    FILE *file = !rc ? fopen(env, "rb") : NULL;
    char *line = NULL, *result = NULL;
    size_t capacity = 0;
    while (file && getline(&line, &capacity, file) >= 0) {
        if (!strncmp(line, "ETUDE_RESULTS=", 14)) {
            free(result);
            line[strcspn(line, "\r\n")] = 0;
            result = strdup(line + 14);
        }
    }
    if (file)
        fclose(file);
    RET_CHECK(result != NULL);
    free(env);
    free(line);
    return result;
}

static char *retention_bundle(int cleanup, const char *payload)
{
    char *bundle = retention_allocate();
    if (!bundle)
        return NULL;
    const char *begin[] = {bundle, strrchr(bundle, '/') + 1, NULL};
    RET_CHECK(!retention_call("begin", begin));
    char *history = ret_format("%s/history", bundle);
    RET_CHECK(history && !mkdir(history, 0700));
    char *path = ret_format("%s/run.jsonl", history);
    char *text = ret_format(
        "{\"record_id\":\"p\",\"stream_id\":\"s\",\"sequence\":0,\"run_id\":\"%s\",\"kind\":"
        "\"plan\",\"data\":%s}\n"
        "{\"record_id\":\"r\",\"stream_id\":\"s\",\"sequence\":1,\"run_id\":\"%s\",\"kind\":"
        "\"run\",\"data\":{\"result\":{\"cleanup\":\"confirmed\",\"outcome\":\"pass\"}}}\n",
        begin[1], payload ? payload : "{}", begin[1]);
    retention_put(path, text);
    free(path);
    free(text);
    free(history);
    path = ret_format("%s/sequence.status", bundle);
    retention_put(path, "0\n");
    free(path);
    path = ret_format("%s/failed", bundle);
    retention_put(path, "");
    free(path);
    text = ret_format("run %s\nphase-end %s preparation 1 status 0 cleanup confirmed cpu_us 1\n"
                      "phase-end %s collection 2 status 0 cleanup confirmed cpu_us 1\nerror "
                      "0\ncancelled 0\nstatus 0\n%s",
                      begin[1], begin[1], begin[1],
                      cleanup ? "active 0\ncleanup confirmed\n" : "cleanup unknown\n");
    path = ret_format("%s/sequence.receipt", bundle);
    retention_put(path, text);
    free(path);
    free(text);
    return bundle;
}

static int retention_state(const char *bundle, const char *expected)
{
    const char *extra[] = {bundle, NULL};
    if (retention_call("seal", extra))
        return 0;
    struct retention registry;
    if (ret_registry_open(&registry, retention_policy, retention_site, NULL))
        return 0;
    char *path = NULL;
    struct json *item = NULL;
    int rc = !ret_record(&registry, bundle, &path, &item) && json_text(item, "state") &&
             !strcmp(json_text(item, "state"), expected);
    free(path);
    json_free(item);
    ret_registry_close(&registry);
    return rc;
}

static void retention_age_tree(const char *path, time_t old)
{
    struct stat st;
    RET_CHECK(!lstat(path, &st));
    if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        struct dirent *entry;
        RET_CHECK(dir != NULL);
        while (dir && (entry = readdir(dir))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;
            char *child = ret_format("%s/%s", path, entry->d_name);
            retention_age_tree(child, old);
            free(child);
        }
        if (dir)
            closedir(dir);
    }
    struct timespec times[] = {{old, 0}, {old, 0}};
    RET_CHECK(!utimensat(AT_FDCWD, path, times, 0));
}
/**
 * Opens the registry and reads the record of `bundle`, counting a failure of
 * either as the fixture's. On success the registry stays open and the caller
 * frees `*path` and `*item` and closes it; on failure nothing is left to free.
 */
static int retention_record(struct retention *registry, const char *bundle, char **path,
                            struct json **item)
{
    *path = NULL;
    *item = NULL;
    int opened = !ret_registry_open(registry, retention_policy, retention_site, NULL);
    RET_CHECK(opened);
    int found = opened && !ret_record(registry, bundle, path, item);
    RET_CHECK(!opened || found);
    if (opened && !found)
        ret_registry_close(registry);
    return found ? 0 : -1;
}
static void retention_old(const char *bundle)
{
    struct retention registry;
    char *path;
    struct json *item;
    if (retention_record(&registry, bundle, &path, &item))
        return;
    ret_set(item, "sealed_utc", ret_string("2000-01-01T00:00:00Z"));
    ret_set(item, "last_used", ret_integer(ret_now() - 3 * 86400));
    RET_CHECK(!ret_write(path, item, 1));
    free(path);
    json_free(item);
    ret_registry_close(&registry);
    retention_age_tree(bundle, (time_t)(ret_now() - 3 * 86400));
}

static void retention_pruning(void)
{
    retention_setup();
    char *ordinary = retention_bundle(1, NULL);
    RET_CHECK(ordinary && retention_state(ordinary, "sealed"));
    char *active = retention_allocate();
    retention_old(ordinary);
    char *unknown = ret_format("%s/runs/unknown", retention_root);
    RET_CHECK(!mkdir(unknown, 0700));
    char *file = ret_format("%s/evidence", unknown);
    retention_put(file, "unknown");
    free(file);
    RET_CHECK(!retention_call("prune", NULL) && !access(ordinary, F_OK));
    const char *apply[] = {"--apply", NULL};
    RET_CHECK(!retention_call("prune", apply));
    RET_CHECK(access(ordinary, F_OK) && !access(active, F_OK) && !access(unknown, F_OK));
    free(ordinary);
    free(active);
    free(unknown);
}

static void retention_uncertain(void)
{
    retention_setup();
    char *uncertain = retention_bundle(0, NULL), *changed = retention_bundle(1, NULL),
         *partial = retention_bundle(1, NULL);
    RET_CHECK(retention_state(uncertain, "protected"));
    RET_CHECK(retention_state(changed, "sealed"));
    retention_old(changed);
    char *path = ret_format("%s/new-proof", changed);
    retention_put(path, "changed");
    free(path);
    path = ret_format("%s/history/run.jsonl", partial);
    FILE *file = fopen(path, "ab");
    RET_CHECK(file && fputs("{", file) >= 0 && !fclose(file));
    free(path);
    RET_CHECK(retention_state(partial, "protected"));
    const char *apply[] = {"--apply", NULL};
    RET_CHECK(!retention_call("prune", apply));
    RET_CHECK(!access(uncertain, F_OK) && !access(changed, F_OK) && !access(partial, F_OK));
    free(uncertain);
    free(changed);
    free(partial);
}

static void retention_closures(void)
{
    char *leaf = ret_format("%s/trace", retention_base),
         *nested = ret_format("%s/nested.json", retention_base);
    retention_put(leaf, "trace");
    struct json *ref = ret_descriptor(leaf), *blob = ret_new(JSON_OBJECT),
                *locations = ret_new(JSON_ARRAY);
    ret_set(blob, "sha256", ret_clone(json_member(ref, "sha256")));
    ret_set(blob, "size_bytes", ret_clone(json_member(ref, "size_bytes")));
    ret_push(locations, ret_string(leaf));
    ret_set(blob, "locations", locations);
    struct json *document = ret_new(JSON_OBJECT);
    ret_set(document, "trace", blob);
    RET_CHECK(!ret_write(nested, document, 0));
    struct json *roots = ret_new(JSON_ARRAY);
    ret_push(roots, ret_descriptor(nested));
    struct json *proofs = ret_closure(roots);
    RET_CHECK(proofs && proofs->count == 2);
    json_free(proofs);
    retention_put(leaf, "changed");
    proofs = ret_closure(roots);
    RET_CHECK(!proofs);
    json_free(proofs);
    RET_CHECK(!unlink(leaf));
    proofs = ret_closure(roots);
    RET_CHECK(!proofs);
    json_free(proofs);
    char *kept = ret_format("%s/routine-proofs", retention_base),
         *copy = ret_format("%s/%s", kept, json_text(ref, "sha256"));
    RET_CHECK(!mkdir(kept, 0700));
    retention_put(copy, "changed");
    proofs = ret_closure(roots);
    RET_CHECK(!proofs);
    json_free(proofs);
    retention_put(copy, "trace");
    proofs = ret_closure(roots);
    RET_CHECK(proofs && proofs->count == 2);
    json_free(proofs);
    RET_CHECK(!unlink(copy) && !rmdir(kept));
    free(kept);
    free(copy);
    free(leaf);
    free(nested);
    json_free(ref);
    json_free(document);
    json_free(roots);
}

static struct json *retention_history_ref(const char *bundle)
{
    char *path = ret_format("%s/history/run.jsonl", bundle);
    struct json *ref = ret_descriptor(path);
    free(path);
    return ref;
}
static void retention_protected(void)
{
    retention_setup();
    char *sample = retention_bundle(1, NULL);
    RET_CHECK(retention_state(sample, "sealed"));
    retention_old(sample);
    struct json *roots = ret_new(JSON_ARRAY);
    ret_push(roots, retention_history_ref(sample));
    struct json *baseline = ret_new(JSON_OBJECT);
    ret_set(baseline, "format", ret_string("etude-retention-protection"));
    ret_set(baseline, "kind", ret_string("baseline"));
    ret_set(baseline, "roots", ret_clone(roots));
    char *path = ret_format("%s/baseline.json", retention_base);
    RET_CHECK(!ret_write(path, baseline, 1));
    struct json *baselines = ret_new(JSON_ARRAY);
    ret_push(baselines, ret_descriptor(path));
    ret_set(fixture_policy, "baselines", baselines);
    const char *apply[] = {"--apply", NULL};
    RET_CHECK(!retention_call("prune", apply) && !access(sample, F_OK));
    ret_set(fixture_policy, "baselines", ret_new(JSON_ARRAY));
    struct json *snapshot = ret_document(retention_issues), *issue = ret_new(JSON_OBJECT),
                *body = NULL;
    char error[128];
    const char *raw =
        "{\"number\":991,\"state\":\"open\",\"body\":\"keep \\u00e9 \\ud83d\\ude00\"}";
    RET_CHECK(!json_parse_data(raw, strlen(raw), 1, &body, error, sizeof error));
    struct jbuf python = {0};
    ret_python_json(body, &python);
    RET_CHECK(!strcmp(
        python.data,
        "{\"body\": \"keep \\u00e9 \\ud83d\\ude00\", \"number\": 991, \"state\": \"open\"}"));
    char hash[65];
    retention_hash(python.data, hash);
    jbuf_free(&python);
    ret_set(issue, "number", ret_integer(991));
    ret_set(issue, "snapshot", body);
    ret_set(issue, "snapshot_sha256", ret_string(hash));
    ret_set(issue, "evidence", ret_clone(roots));
    struct json *issues = ret_new(JSON_ARRAY);
    ret_push(issues, issue);
    ret_set(snapshot, "issues", issues);
    RET_CHECK(!ret_write(retention_issues, snapshot, 1));
    ret_set(fixture_policy, "open_issues", ret_descriptor(retention_issues));
    RET_CHECK(!retention_call("prune", apply) && !access(sample, F_OK));
    free(sample);
    free(path);
    json_free(roots);
    json_free(baseline);
    json_free(snapshot);
}

static void retention_limits(void)
{
    retention_setup();
    char *sample = retention_bundle(1, NULL);
    RET_CHECK(retention_state(sample, "sealed"));
    retention_old(sample);
    struct json *stale = ret_clone(json_member(fixture_policy, "open_issues"));
    ret_set(stale, "sha256",
            ret_string("0000000000000000000000000000000000000000000000000000000000000000"));
    ret_set(fixture_policy, "open_issues", stale);
    char *fresh = retention_allocate();
    RET_CHECK(fresh && !access(sample, F_OK));
    free(fresh);
    ret_set(fixture_policy, "max_bytes", ret_integer(10000));
    RET_CHECK(retention_call("allocate", NULL) != 0 && !access(sample, F_OK));
    free(sample);
    retention_setup();
    sample = retention_bundle(1, NULL);
    RET_CHECK(retention_state(sample, "sealed"));
    const char *apply[] = {"--apply", NULL};
    RET_CHECK(!retention_call("prune", apply) && !access(sample, F_OK));
    char *large = ret_format("%s/unregistered-large-object", retention_root);
    int fd = open(large, O_WRONLY | O_CREAT | O_EXCL, 0600);
    RET_CHECK(fd >= 0 && !ftruncate(fd, 1000000));
    if (fd >= 0)
        close(fd);
    RET_CHECK(retention_call("allocate", NULL) != 0 && !access(large, F_OK) &&
              !access(sample, F_OK));
    free(large);
    free(sample);
}

static void retention_dependencies(void)
{
    retention_setup();
    char *sample = retention_bundle(1, NULL);
    RET_CHECK(retention_state(sample, "sealed"));
    struct json *ref = retention_history_ref(sample), *blob = ret_new(JSON_OBJECT),
                *locations = ret_new(JSON_ARRAY);
    ret_set(blob, "sha256", ret_clone(json_member(ref, "sha256")));
    ret_set(blob, "size_bytes", ret_clone(json_member(ref, "size_bytes")));
    ret_push(locations, ret_clone(json_member(ref, "path")));
    ret_set(blob, "locations", locations);
    struct json *payload = ret_new(JSON_OBJECT);
    ret_set(payload, "previous", blob);
    struct jbuf text = {0};
    json_canonical(payload, &text);
    char *newer = retention_bundle(1, text.data);
    RET_CHECK(retention_state(newer, "sealed"));
    retention_old(sample);
    const char *apply[] = {"--apply", NULL};
    RET_CHECK(!retention_call("prune", apply) && !access(sample, F_OK) && !access(newer, F_OK));
    free(sample);
    free(newer);
    json_free(ref);
    json_free(payload);
    jbuf_free(&text);
    char *identity = retention_bundle(
        1,
        "{\"compiler\":{\"path\":\"/missing/"
        "compiler\",\"sha256\":"
        "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"size_bytes\":1}}");
    RET_CHECK(retention_state(identity, "sealed"));
    free(identity);
}

static void retention_external_proof(void)
{
    retention_setup();
    char *leaf = ret_format("%s/external-proof", retention_base);
    retention_put(leaf, "external retained bytes");
    struct json *ref = ret_descriptor(leaf), *blob = ret_new(JSON_OBJECT),
                *locations = ret_new(JSON_ARRAY);
    ret_set(blob, "sha256", ret_clone(json_member(ref, "sha256")));
    ret_set(blob, "size_bytes", ret_clone(json_member(ref, "size_bytes")));
    ret_push(locations, ret_string(leaf));
    ret_set(blob, "locations", locations);
    struct json *payload = ret_new(JSON_OBJECT);
    ret_set(payload, "external", blob);
    struct jbuf text = {0};
    json_canonical(payload, &text);
    char *bundle = retention_bundle(1, text.data);
    RET_CHECK(retention_state(bundle, "protected"));
    struct retention registry;
    char *path;
    struct json *item;
    if (!retention_record(&registry, bundle, &path, &item)) {
        const struct json *proofs = json_member(item, "proofs");
        RET_CHECK(proofs && json_member(proofs, leaf));
        free(path);
        json_free(item);
        ret_registry_close(&registry);
    }
    free(bundle);
    free(leaf);
    json_free(ref);
    json_free(payload);
    jbuf_free(&text);
}

static void retention_references(void)
{
    retention_setup();
    char *sample = retention_bundle(1, NULL);
    RET_CHECK(retention_state(sample, "sealed"));
    retention_old(sample);
    char *budget = ret_format("%s/budget.json", retention_base),
         *reference = ret_format("%s/reference.json", retention_base);
    retention_put(budget, "{}");
    struct json *current = ret_new(JSON_OBJECT), *histories = ret_new(JSON_ARRAY);
    ret_push(histories, retention_history_ref(sample));
    ret_set(current, "format", ret_string("etude-site-reference"));
    ret_set(current, "schema_version", ret_integer(1));
    ret_set(current, "policy", ret_descriptor(budget));
    ret_set(current, "histories", histories);
    RET_CHECK(!ret_write(reference, current, 1));
    const char *extra[] = {"--reference", reference, NULL};
    RET_CHECK(!retention_call("allocate", extra));
    RET_CHECK(!unlink(reference));
    const char *apply[] = {"--apply", NULL};
    RET_CHECK(!retention_call("prune", apply) && !access(sample, F_OK));
    free(sample);
    free(budget);
    free(reference);
    json_free(current);
}

static void retention_live_and_reader(void)
{
    retention_setup();
    char *sample = retention_bundle(1, NULL);
    RET_CHECK(retention_state(sample, "sealed"));
    retention_old(sample);
    setenv("RETENTION_GH_FAIL", "yes", 1);
    char *fresh = retention_allocate();
    RET_CHECK(fresh && !access(sample, F_OK));
    free(fresh);
    unsetenv("RETENTION_GH_FAIL");
    struct retention registry;
    char *path;
    struct json *item;
    char fraction[64];
    snprintf(fraction, sizeof fraction, "%lld.125", (long long)(ret_now() - 3 * 86400));
    if (!retention_record(&registry, sample, &path, &item)) {
        struct json *used = ret_new(JSON_NUMBER);
        used->string = strdup(fraction);
        ret_set(item, "last_used", used);
        RET_CHECK(!ret_write(path, item, 1));
        free(path);
        json_free(item);
        ret_registry_close(&registry);
    }
    const char *old_policy = getenv("ETUDE_RETENTION"), *old_site = getenv("ETUDE_SITE");
    char *saved_policy = old_policy ? strdup(old_policy) : NULL,
         *saved_site = old_site ? strdup(old_site) : NULL;
    setenv("ETUDE_RETENTION", retention_policy, 1);
    setenv("ETUDE_SITE", retention_site, 1);
    const char *paths[] = {sample, NULL};
    struct etude_retention_guard *guard = NULL;
    int64_t first = ret_now();
    RET_CHECK(!etude_retention_read_begin(paths, 1, NULL, &guard) && guard);
    RET_CHECK(!access(sample, F_OK));
    etude_retention_read_end(guard);
    if (saved_policy)
        setenv("ETUDE_RETENTION", saved_policy, 1);
    else
        unsetenv("ETUDE_RETENTION");
    if (saved_site)
        setenv("ETUDE_SITE", saved_site, 1);
    else
        unsetenv("ETUDE_SITE");
    free(saved_policy);
    free(saved_site);
    const char *apply[] = {"--apply", NULL};
    RET_CHECK(!retention_call("prune", apply) && !access(sample, F_OK));
    int64_t last = ret_now();
    if (!retention_record(&registry, sample, &path, &item)) {
        const struct json *previous = json_member(item, "last_used_previous");
        RET_CHECK(previous && previous->type == JSON_NUMBER && !strcmp(previous->string, fraction));
        RET_CHECK(ret_number(item, "last_used") > first &&
                  ret_number(item, "last_used") <= last + 1);
        RET_CHECK(json_text(item, "last_used_normalization") &&
                  !strcmp(json_text(item, "last_used_normalization"), "ceil-epoch-seconds-v1"));
        free(path);
        json_free(item);
        ret_registry_close(&registry);
    }
    const char *touch[] = {sample, NULL};
    RET_CHECK(!retention_call("touch", touch));
    RET_CHECK(!ret_registry_open(&registry, retention_policy, retention_site, NULL));
    ret_registry_close(&registry);
    RET_CHECK(!ret_registry_open(&registry, retention_policy, retention_site, NULL));
    ret_registry_close(&registry);
    uint64_t before = 0, after = 0;
    RET_CHECK(!ret_usage_tree(retention_root, &before));
    retention_put(retention_site, "different installed authority\n");
    RET_CHECK(retention_call("allocate", NULL) != 0);
    RET_CHECK(!ret_usage_tree(retention_root, &after) && before == after);
    retention_put(retention_site, "etude-site 2\n");
    free(sample);
}

#ifndef _WIN32
extern char **environ;
#endif
static void retention_windows_receipt(void)
{
    retention_setup();
    char *bundle = retention_bundle(1, NULL), *path = ret_format("%s/sequence.receipt", bundle);
    FILE *file = fopen(path, "rb");
    char text[4096];
    size_t size = file ? fread(text, 1, sizeof text - 1, file) : 0;
    if (file)
        fclose(file);
    text[size] = 0;
    struct jbuf changed = {0};
    const char *at = text, *found;
    while ((found = strstr(at, "cleanup confirmed cpu_us"))) {
        jbuf_put(&changed, at, (size_t)(found - at));
        jbuf_puts(&changed, "active 0 cpu_us");
        at = found + strlen("cleanup confirmed cpu_us");
    }
    jbuf_puts(&changed, at);
    retention_put(path, changed.data);
    RET_CHECK(retention_state(bundle, "sealed"));
    free(bundle);
    free(path);
    jbuf_free(&changed);
}

static void retention_parallel(void)
{
    retention_setup();
    struct etude_exec *executor = etude_exec_new();
    struct etude_proc *one = NULL, *two = NULL;
    char *argv[] = {retention_self, "--retention-child", "retention", "--policy", retention_policy,
                    "--site",       retention_site,      "allocate",  NULL};
    struct etude_exec_spec spec = {.attempt_id = "retention-one",
                                   .argv = argv,
                                   .envp = environ,
                                   .output_max = 8192,
                                   .grace_us = 1000000};
    RET_CHECK(executor && !etude_exec_start(executor, &spec, &one));
    spec.attempt_id = "retention-two";
    RET_CHECK(executor && !etude_exec_start(executor, &spec, &two));
    unsigned ended = 0;
    while (one && two && ended < 2)
        if (etude_exec_wait(executor, -1))
            ended++;
    if (one && two) {
        const struct etude_exec_result *a = etude_proc_result(one), *b = etude_proc_result(two);
        RET_CHECK(a->end == ETUDE_END_EXITED && !a->value && a->cleanup == ETUDE_CLEANUP_CONFIRMED);
        RET_CHECK(b->end == ETUDE_END_EXITED && !b->value && b->cleanup == ETUDE_CLEANUP_CONFIRMED);
        RET_CHECK(a->out.kept && b->out.kept && strcmp(a->out.data, b->out.data));
        char *first = strndup(a->out.data, strcspn(a->out.data, "\r\n"));
        char *second = strndup(b->out.data, strcspn(b->out.data, "\r\n"));
        RET_CHECK(first && second && !access(first, F_OK) && !access(second, F_OK));
        free(first);
        free(second);
    }
    if (executor)
        etude_exec_free(executor);
}

static void retention_issue_command(void)
{
    char *response = ret_format("%s/gh-response.json", retention_base);
    retention_put(response, "[[{\"number\":991,\"updated_at\":\"2026-10-26T00:00:00Z\","
                            "\"comments\":0,\"state\":\"open\",\"body\":\"keep \\u00e9\"}]]");
    char *mapping = ret_format("%s/issue-map.json", retention_base),
         *output = ret_format("%s/issue-snapshot.json", retention_base);
    retention_put(mapping, "{\"991\":[]}");
    char *argv[] = {"retention", "issues", "--repository", "fixture/retention",
                    "--mapping", mapping,  "--output",     output};
    RET_CHECK(!etude_retention_main(8, argv, NULL));
    struct json *snapshot = ret_document(output);
    const struct json *issues = json_member(snapshot, "issues"),
                      *unknown = json_member(snapshot, "unknown");
    RET_CHECK(issues && issues->type == JSON_ARRAY && issues->count == 1 && unknown &&
              !unknown->count);
    RET_CHECK(!ret_issue_live(snapshot, NULL));
    RET_CHECK(etude_retention_main(8, argv, NULL) != 0);
    json_free(snapshot);
    retention_put(mapping, "{}");
    char *unmapped = ret_format("%s/issue-unmapped.json", retention_base);
    argv[7] = unmapped;
    RET_CHECK(!etude_retention_main(8, argv, NULL));
    snapshot = ret_document(unmapped);
    unknown = json_member(snapshot, "unknown");
    RET_CHECK(unknown && unknown->type == JSON_ARRAY && unknown->count == 1);
    json_free(snapshot);
    retention_put(response, "[[]]");
    free(response);
    free(mapping);
    free(output);
    free(unmapped);
}

static void retention_reference(void)
{
    char *tree = ret_format("%s/forecast", retention_base);
    char *directory = ret_format("%s/tests/cases", tree);
    RET_CHECK(!ret_dirs(directory));
    char *manifest = ret_format("%s/a.json", directory);
    retention_put(manifest, "{}");
    free(manifest);
    manifest = ret_format("%s/z.json", directory);
    retention_put(manifest, "{}");
    free(manifest);
    char *history = ret_format("%s/reference.history", retention_base),
         *budget = ret_format("%s/reference.budget", retention_base);
    char *descriptor = ret_format("%s/reference.desc", retention_base),
         *prepare = ret_format("%s/reference.prepare", retention_base);
    char *output = ret_format("%s/reference.forecast", retention_base);
    retention_put(history, "actual retained history bytes\n");
    retention_put(budget,
                  "{\"site\":{\"machine_id\":\"fixture\",\"os\":\"linux\",\"accelerator\":\"tcg\","
                  "\"capacity\":{\"cpu_millis\":2000,\"memory_bytes\":1073741824,\"disk_bytes\":"
                  "42949672960}}}");
    retention_put(prepare, "{\"format\":\"etude-preparation-receipt\",\"outcome\":\"pass\","
                           "\"cleanup\":\"confirmed\",\"machine_id\":\"fixture\"}");
    retention_put(retention_site,
                  "etude-site 2\nendpoint linux\ncapacity 2000,1073741824,42949672960\n");
    struct json *ref = ret_new(JSON_OBJECT), *histories = ret_new(JSON_ARRAY);
    ret_set(ref, "format", ret_string("etude-site-reference"));
    ret_set(ref, "schema_version", ret_integer(1));
    ret_set(ref, "policy", ret_descriptor(budget));
    ret_push(histories, ret_descriptor(history));
    ret_set(ref, "histories", histories);
    RET_CHECK(!ret_write(descriptor, ref, 0));
    char *argv[] = {"reference",    "--tree",        tree,       "--etude",       retention_self,
                    "--reference",  descriptor,      "--output", output,          "--site",
                    retention_site, "--preparation", prepare,    "--accelerator", "tcg"};
    RET_CHECK(!etude_reference_main(15, argv, NULL));
    RET_CHECK(etude_reference_main(15, argv, NULL) != 0);
    char *receipt = ret_format("%s/reference.receipt.json", retention_base);
    struct json *result = ret_document(receipt);
    RET_CHECK(result && !ret_number(result, "status"));
    char *checked = ret_checked(json_member(result, "log"));
    RET_CHECK(checked != NULL);
    free(checked);
    json_free(result);
    retention_put(budget, "changed policy");
    char *absent = ret_format("%s/refused.forecast", retention_base);
    argv[8] = absent;
    RET_CHECK(etude_reference_main(15, argv, NULL) != 0 && access(absent, F_OK));
    free(tree);
    free(directory);
    free(history);
    free(budget);
    free(descriptor);
    free(prepare);
    free(output);
    free(receipt);
    free(absent);
    json_free(ref);
}

/** Whether a directory above `path` has a `.git`, which makes `retention init`
 * refuse a root under `path`. */
static int retention_in_checkout(const char *path)
{
    char *dir = strdup(path), *slash;
    int found = 0;
    while (dir && !found && (slash = strrchr(dir, '/')) != NULL) {
        *slash = 0;
        char *git = ret_format("%s/.git", dir);
        struct stat st;
        found = git && !lstat(git, &st);
        free(git);
    }
    free(dir);
    return found;
}

int etude_test_retention(const char *self)
{
    char *canonical = realpath(fix_dir(), NULL);
    if (!canonical || strlen(canonical) >= sizeof retention_base ||
        !realpath(self, retention_self)) {
        free(canonical);
        return 1;
    }
    strcpy(retention_base, canonical);
    free(canonical);
    if (retention_in_checkout(retention_base)) {
        fprintf(stderr,
                "retention fixture: %s is inside a git checkout, where retention refuses a "
                "root; set TMPDIR to a directory outside the checkout\n",
                retention_base);
        return 1;
    }
    snprintf(retention_policy, sizeof retention_policy, "%s/policy.json", retention_base);
    snprintf(retention_site, sizeof retention_site, "%s/site", retention_base);
    snprintf(retention_issues, sizeof retention_issues, "%s/issues.json", retention_base);
    retention_put(retention_site, "etude-site 2\n");
    char *bin = ret_format("%s/bin", retention_base), *gh = ret_format("%s/bin/gh", retention_base);
    RET_CHECK(!mkdir(bin, 0700) && !symlink(retention_self, gh));
    char *response = ret_format("%s/gh-response.json", retention_base);
    retention_put(response, "[[]]");
    const char *old = getenv("PATH");
    char *saved = old ? strdup(old) : NULL;
    char *path = ret_format("%s:%s", bin, old ? old : "/bin:/usr/bin");
    setenv("PATH", path, 1);
    setenv("RETENTION_GH_RESPONSE", response, 1);
    retention_pruning();
    retention_uncertain();
    retention_closures();
    retention_protected();
    retention_limits();
    retention_dependencies();
    retention_external_proof();
    retention_references();
    retention_live_and_reader();
    retention_windows_receipt();
    retention_parallel();
    retention_issue_command();
    retention_reference();
    if (saved)
        setenv("PATH", saved, 1);
    else
        unsetenv("PATH");
    unsetenv("RETENTION_GH_RESPONSE");
    free(bin);
    free(gh);
    free(response);
    free(saved);
    free(path);
    json_free(fixture_policy);
    fixture_policy = NULL;
    if (!retention_failures) {
        puts("retention: native ownership, closure, protection and reference checks passed");
        fix_passed();
    }
    return retention_failures != 0;
}
