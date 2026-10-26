/* Register and prune whole evidence bundles under the selected site's lock. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#endif
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "retention.h"
#include "authority_platform.h"
#include "../../utils/sha256.h"

static int retained_state(const char *state)
{
    return state && (!strcmp(state, "allocated") || !strcmp(state, "active") ||
                     !strcmp(state, "sealed") || !strcmp(state, "protected"));
}

/* Existing fractional registry data remains readable; age always rounds up. */
static int64_t retained_used(const struct json *item)
{
    const struct json *value = json_member(item, "last_used");
    if (value && value->type == JSON_INTEGER)
        return value->integer;
    if (!value || value->type != JSON_NUMBER || !value->string)
        return -1;
    char *end = NULL;
    errno = 0;
    double number = strtod(value->string, &end);
    if (errno || !end || *end || !isfinite(number) || number < 0 || number > JSON_INTEGER_MAX - 1)
        return -1;
    int64_t whole = (int64_t)number;
    return whole + (number > (double)whole);
}

static int retained_touch(struct json *item)
{
    const struct json *previous = json_member(item, "last_used");
    if (retained_used(item) < 0 || ret_now() < 0)
        return ret_fail("invalid retained last-used timestamp");
    if (previous->type == JSON_NUMBER) {
        ret_set(item, "last_used_previous", ret_clone(previous));
        ret_set(item, "last_used_normalization", ret_string("ceil-epoch-seconds-v1"));
    }
    return ret_set(item, "last_used", ret_integer(ret_now() + 1));
}

static struct json *ret_records(struct retention *r)
{
    char *directory = ret_format("%s/registry", r->root);
    DIR *dir = directory ? opendir(directory) : NULL;
    struct json *records = ret_new(JSON_OBJECT);
    struct dirent *entry;
    int rc = !dir;
    while (!rc && (entry = ret_next(dir, &rc))) {
        size_t size = strlen(entry->d_name);
        if (size <= 5 || strcmp(entry->d_name + size - 5, ".json"))
            continue;
        char *name = strndup(entry->d_name, size - 5),
             *path = ret_format("%s/%s", directory, entry->d_name);
        struct json *item = path ? ret_document(path) : NULL;
        char *bundle = name ? ret_format("%s/runs/%s", r->root, name) : NULL;
        const char *format = json_text(item, "format"), *actual = json_text(item, "path");
        if (!name || strncmp(name, "run-", 4) || !format ||
            strcmp(format, "etude-retained-bundle") || ret_number(item, "schema_version") != 1 ||
            !retained_state(json_text(item, "state")) || !bundle || !actual ||
            strcmp(actual, bundle) || ret_number(item, "reservation_bytes") <= 0)
            rc = ret_fail("unknown retained bundle record");
        if (!rc) {
            ret_set(records, name, item);
            item = NULL;
        }
        json_free(item);
        free(name);
        free(path);
        free(bundle);
    }
    if (dir)
        closedir(dir);
    free(directory);
    if (rc) {
        json_free(records);
        records = NULL;
    }
    return records;
}

int ret_record(struct retention *r, const char *given, char **record_path, struct json **item)
{
    *record_path = NULL;
    *item = NULL;
    char *bundle = ret_path(given, 1), *runs = ret_format("%s/runs/", r->root);
    if (!bundle || !runs || strncmp(bundle, runs, strlen(runs)) || !bundle[strlen(runs)] ||
        strchr(bundle + strlen(runs), '/')) {
        free(bundle);
        free(runs);
        return ret_fail("OUT is not a registered persistent bundle");
    }
    const char *name = bundle + strlen(runs);
    char *path = ret_format("%s/registry/%s.json", r->root, name);
    struct json *value = path ? ret_document(path) : NULL;
    const char *registered = json_text(value, "path"), *format = json_text(value, "format");
    int rc = !registered || strcmp(bundle, registered) || !format ||
             strcmp(format, "etude-retained-bundle") || ret_number(value, "schema_version") != 1 ||
             !retained_state(json_text(value, "state"));
    free(bundle);
    free(runs);
    if (rc) {
        free(path);
        json_free(value);
        return ret_fail("registered bundle identity differs");
    }
    *record_path = path;
    *item = value;
    return 0;
}

static int retained_roots_add(struct json *roots, const struct json *values)
{
    if (!values || values->type != JSON_ARRAY)
        return ret_fail("missing retained protection roots");
    for (size_t i = 0; i < values->count; i++)
        if (ret_push(roots, ret_clone(values->item[i])))
            return -1;
    return 0;
}

static struct json *retained_reference(const char *path, struct json *roots)
{
    struct json *ref = ret_descriptor(path);
    char *checked = ref ? ret_checked(ref) : NULL;
    struct json *current = checked ? ret_document(checked) : NULL;
    const char *format = json_text(current, "format");
    const struct json *histories = json_member(current, "histories");
    int valid = format && !strcmp(format, "etude-site-reference") && histories &&
                histories->type == JSON_ARRAY && histories->count && json_member(current, "policy");
    if (valid && roots) {
        ret_push(roots, ret_clone(ref));
        ret_push(roots, ret_clone(json_member(current, "policy")));
        valid = !retained_roots_add(roots, histories);
    }
    json_free(ref);
    free(checked);
    if (!valid) {
        json_free(current);
        current = NULL;
        ret_fail("selected-site reference is incomplete");
    }
    return current;
}

static int retained_accepted(struct retention *r, struct json *roots)
{
    char *directory = ret_format("%s/accepted", r->root);
    DIR *dir = directory ? opendir(directory) : NULL;
    struct dirent *entry;
    int rc = !dir;
    while (!rc && (entry = ret_next(dir, &rc))) {
        size_t n = strlen(entry->d_name);
        if (n < 5 || strcmp(entry->d_name + n - 5, ".json"))
            continue;
        char *path = ret_format("%s/%s", directory, entry->d_name);
        struct json *accepted = path ? ret_document(path) : NULL;
        rc = !accepted || retained_roots_add(roots, json_member(accepted, "roots"));
        json_free(accepted);
        free(path);
    }
    if (dir)
        closedir(dir);
    free(directory);
    return rc;
}

static int retained_baselines(struct retention *r, struct json *roots)
{
    const struct json *baselines = json_member(r->policy, "baselines");
    for (size_t i = 0; i < baselines->count; i++) {
        const struct json *ref = baselines->item[i];
        char *path = ret_checked(ref);
        struct json *value = path ? ret_document(path) : NULL;
        const char *format = json_text(value, "format"), *kind = json_text(value, "kind");
        const struct json *items = json_member(value, "roots");
        int rc = !format || strcmp(format, "etude-retention-protection") || !kind ||
                 strcmp(kind, "baseline") || !items || items->type != JSON_ARRAY || !items->count ||
                 retained_roots_add(roots, items);
        if (!rc)
            rc = ret_push(roots, ret_clone(ref));
        free(path);
        json_free(value);
        if (rc)
            return ret_fail("baseline protection descriptor is incomplete");
    }
    return 0;
}

static int retained_snapshot_entry(const struct json *issue, struct json *roots)
{
    int64_t number = ret_number(issue, "number");
    const struct json *snapshot = json_member(issue, "snapshot");
    const char *state = json_text(snapshot, "state"),
               *expected = json_text(issue, "snapshot_sha256");
    if (number <= 0 || ret_number(snapshot, "number") != number || !state ||
        strcmp(state, "open") || !expected)
        return ret_fail("invalid open-issue snapshot entry");
    struct jbuf text = {0};
    ret_python_json(snapshot, &text);
    struct sha256 hash;
    unsigned char raw[32];
    char digest[65];
    sha256_init(&hash);
    sha256_update(&hash, (const unsigned char *)text.data, text.len);
    sha256_final(&hash, raw);
    sha256_hex(raw, digest);
    jbuf_free(&text);
    if (strcmp(digest, expected))
        return ret_fail("open-issue snapshot digest differs");
    return retained_roots_add(roots, json_member(issue, "evidence"));
}

static int retained_issues(struct retention *r, struct json *roots)
{
    const struct json *ref = json_member(r->policy, "open_issues");
    char *path = ret_checked(ref);
    struct json *snapshot = path ? ret_document(path) : NULL;
    free(path);
    const char *format = json_text(snapshot, "format"),
               *captured = json_text(snapshot, "captured_utc");
    const struct json *complete = json_member(snapshot, "complete"),
                      *unknown = json_member(snapshot, "unknown");
    const struct json *issues = json_member(snapshot, "issues");
    int64_t at = ret_timestamp_us(captured), now = ret_now_us(), age = now - at;
    int64_t limit = ret_number(r->policy, "issue_snapshot_max_age_seconds");
    int rc = !format || strcmp(format, "etude-retention-open-issues") ||
             ret_number(snapshot, "schema_version") != 1 || !complete ||
             complete->type != JSON_BOOLEAN || !complete->integer || !unknown ||
             unknown->type != JSON_ARRAY || unknown->count || !issues ||
             issues->type != JSON_ARRAY || at < 0 || now < 0 || age < 0 || age / 1000000 > limit ||
             (age / 1000000 == limit && age % 1000000);
    for (size_t i = 0; !rc && i < issues->count; i++)
        rc = retained_snapshot_entry(issues->item[i], roots);
    if (!rc)
        rc = ret_push(roots, ret_clone(ref));
    json_free(snapshot);
    return rc ? ret_fail("open-issue protection snapshot is incomplete or stale") : 0;
}

static int retained_bundle_name(struct retention *r, const char *path, struct json *keep)
{
    char *prefix = ret_format("%s/runs/", r->root);
    int rc = 0;
    if (!strncmp(path, prefix, strlen(prefix))) {
        const char *start = path + strlen(prefix);
        size_t n = strcspn(start, "/");
        char *name = strndup(start, n);
        if (!n || !name)
            rc = -1;
        else
            rc = ret_add_name(keep, name);
        free(name);
    }
    free(prefix);
    return rc;
}

static int retained_extend(struct json *object, const struct json *items)
{
    if (!items || items->type != JSON_OBJECT)
        return ret_fail("invalid retained proof map");
    for (size_t i = 0; i < items->count; i++)
        if (ret_set(object, items->item[i]->name, ret_clone(items->item[i])))
            return -1;
    return 0;
}

static int retained_protection(struct retention *r, const struct json *records,
                               const char *reference, struct json *keep, struct json *external,
                               struct json *warnings)
{
    for (size_t i = 0; i < records->count; i++) {
        const struct json *record = records->item[i], *proofs = json_member(record, "proofs");
        if (strcmp(json_text(record, "state"), "sealed"))
            ret_add_name(keep, record->name);
        if (proofs && retained_extend(external, proofs))
            return -1;
    }
    const struct json *anchors = json_member(r->policy, "anchors");
    for (size_t i = 0; i < anchors->count; i++) {
        const char *given = json_text(anchors->item[i], "path");
        char *path = given ? ret_path(given, 0) : NULL;
        if (!path)
            return ret_fail("invalid immutable evidence anchor");
        ret_set(external, path, ret_clone(anchors->item[i]));
        free(path);
    }
    struct json *roots = ret_new(JSON_ARRAY), *current = NULL, *proofs = NULL;
    int rc =
        retained_accepted(r, roots) || retained_baselines(r, roots) || retained_issues(r, roots);
    if (!rc && reference) {
        current = retained_reference(reference, roots);
        rc = !current;
    }
    if (!rc) {
        proofs = ret_closure(roots);
        rc = !proofs;
    }
    if (!rc)
        rc = retained_extend(external, proofs);
    for (size_t i = 0; !rc && i < proofs->count; i++)
        rc = retained_bundle_name(r, json_text(proofs->item[i], "path"), keep);
    for (size_t i = 0; !rc && i < records->count; i++) {
        const struct json *deps = json_member(records->item[i], "proofs");
        for (size_t j = 0; deps && !rc && j < deps->count; j++) {
            char *path = ret_checked(deps->item[j]);
            if (!path) {
                rc = -1;
                break;
            }
            char *own = ret_format("%s/runs/%s", r->root, records->item[i]->name);
            if (!ret_inside(own, path))
                rc = retained_bundle_name(r, path, keep);
            free(own);
            free(path);
        }
    }
    if (rc) {
        ret_push(warnings, ret_string(ret_error()));
        for (size_t i = 0; i < records->count; i++)
            ret_add_name(keep, records->item[i]->name);
    }
    json_free(roots);
    json_free(current);
    json_free(proofs);
    return 0;
}

static int retained_usage(struct retention *r, const struct json *records,
                          const struct json *external, uint64_t *total)
{
    if (ret_usage_tree(r->root, total))
        return -1;
    for (size_t i = 0; i < external->count; i++) {
        const struct json *ref = external->item[i];
        const char *given = json_text(ref, "path");
        char *path = given ? ret_path(given, 0) : NULL;
        int64_t bytes = ret_number(ref, "size_bytes");
        if (!path || bytes < 0) {
            free(path);
            return ret_fail("unknown external proof size");
        }
        if (!ret_inside(r->root, path)) {
            struct stat st;
            if (!stat(path, &st) && st.st_size > bytes)
                bytes = st.st_size;
            if (*total > (uint64_t)JSON_INTEGER_MAX - (uint64_t)bytes) {
                free(path);
                return -1;
            }
            *total += (uint64_t)bytes;
        }
        free(path);
    }
    for (size_t i = 0; i < records->count; i++) {
        const struct json *item = records->item[i];
        if (!strcmp(json_text(item, "state"), "sealed"))
            continue;
        uint64_t actual = 0;
        if (ret_usage_tree(json_text(item, "path"), &actual))
            return -1;
        int64_t reserved = ret_number(item, "reservation_bytes");
        if (reserved < 0)
            return -1;
        if ((uint64_t)reserved > actual) {
            uint64_t extra = (uint64_t)reserved - actual;
            if (*total > (uint64_t)JSON_INTEGER_MAX - extra)
                return -1;
            *total += extra;
        }
    }
    return 0;
}

static int retained_candidate(const struct json *item, const struct json *actual, int64_t latest)
{
    const struct json *expected = json_member(item, "files");
    int64_t used = retained_used(item);
    if (!expected || !json_equal(expected, actual))
        return ret_fail("bundle changed after sealing");
    if (used < 0 || ret_now() - (latest > used ? latest : used) < 86400)
        return ret_fail("bundle was used or changed within one day");
    return 0;
}

static int retained_order(const void *a, const void *b)
{
    const struct json *const *x = a, *const *y = b;
    const char *one = json_text(*x, "sealed_utc"), *two = json_text(*y, "sealed_utc");
    int order = strcmp(one ? one : "", two ? two : "");
    return order ? order : strcmp((*x)->name, (*y)->name);
}
static int retained_string_order(const void *a, const void *b)
{
    const struct json *const *x = a, *const *y = b;
    return strcmp((*x)->string, (*y)->string);
}

static int retained_live(struct retention *r)
{
    const struct json *ref = json_member(r->policy, "open_issues");
    char *path = ret_checked(ref);
    struct json *snapshot = path ? ret_document(path) : NULL;
    free(path);
    int rc = !snapshot || ret_issue_live(snapshot, r->stop);
    json_free(snapshot);
    return rc;
}

static int retained_delete(struct retention *r, const struct json *removed)
{
    for (size_t i = 0; i < removed->count; i++) {
        const char *bundle = removed->item[i]->string, *name = strrchr(bundle, '/');
        char *record = name ? ret_format("%s/registry/%s.json", r->root, name + 1) : NULL;
        if ((r->stop && *r->stop) || !record || ret_remove(bundle) || unlink(record)) {
            free(record);
            return ret_fail("retained bundle removal did not finish");
        }
        free(record);
    }
    return 0;
}

static struct json *ret_reclaim(struct retention *r, const char *reference, int apply)
{
    struct json *records = ret_records(r), *keep = ret_new(JSON_ARRAY),
                *external = ret_new(JSON_OBJECT);
    struct json *warnings = ret_new(JSON_ARRAY), *removed = ret_new(JSON_ARRAY),
                *report = ret_new(JSON_OBJECT);
    uint64_t total = 0, original = 0;
    int rc = !records || retained_protection(r, records, reference, keep, external, warnings) ||
             retained_usage(r, records, external, &total);
    original = total;
    const struct json **ordered =
        json_alloc(((records ? records->count : 0) + 1) * sizeof(*ordered));
    for (size_t i = 0; records && i < records->count; i++)
        ordered[i] = records->item[i];
    if (records)
        qsort(ordered, records->count, sizeof(*ordered), retained_order);
    int64_t reserve = ret_number(r->policy, "reserve_bytes"),
            limit = ret_number(r->policy, "max_bytes");
    for (size_t i = 0; !rc && i < records->count; i++) {
        const struct json *item = ordered[i];
        if (ret_has(keep, item->name))
            continue;
        int64_t sealed = ret_seconds(json_text(item, "sealed_utc")), age = ret_now() - sealed;
        int64_t days = ret_number(r->policy, "max_age_days");
        if (sealed < 0) {
            rc = ret_fail("invalid bundle sealing timestamp");
            break;
        }
        if (age < 86400 || (age / 86400 < days && total <= (uint64_t)(limit - reserve)))
            continue;
        uint64_t size = 0;
        int64_t latest = 0;
        struct json *files = ret_files(json_text(item, "path"), &size, &latest);
        if (!files || retained_candidate(item, files, latest))
            ret_push(warnings, ret_string(ret_error()));
        else {
            ret_push(removed, ret_string(json_text(item, "path")));
            total -= size;
        }
        json_free(files);
    }
    if (!rc && apply && removed->count) {
        if (retained_live(r)) {
            for (size_t i = 0; i < records->count; i++)
                ret_add_name(keep, records->item[i]->name);
            ret_push(warnings, ret_string(ret_error()));
            json_free(removed);
            removed = ret_new(JSON_ARRAY);
            total = original;
        } else
            rc = retained_delete(r, removed);
    }
    if (!rc) {
        qsort(keep->item, keep->count, sizeof(*keep->item), retained_string_order);
        ret_set(report, "bytes", ret_integer((int64_t)total));
        ret_set(report, "limit_bytes", ret_integer(limit));
        ret_set(report, "reserved_next_bytes", ret_integer(reserve));
        ret_set(report, "can_allocate", ret_boolean(total <= (uint64_t)(limit - reserve)));
        ret_set(report, "protected", keep);
        keep = NULL;
        ret_set(report, "protection_warnings", warnings);
        warnings = NULL;
        ret_set(report, apply ? "removed" : "would_remove", removed);
        removed = NULL;
        ret_set(report, apply ? "would_remove" : "removed", ret_new(JSON_ARRAY));
    }
    free(ordered);
    json_free(records);
    json_free(keep);
    json_free(external);
    json_free(warnings);
    json_free(removed);
    if (rc) {
        json_free(report);
        report = NULL;
    }
    return report;
}

static int retained_accept_reference(struct retention *r, const char *reference)
{
    if (!reference)
        return 0;
    struct json *current = retained_reference(reference, NULL), *roots = ret_new(JSON_ARRAY);
    if (!current) {
        json_free(roots);
        return -1;
    }
    ret_push(roots, ret_clone(json_member(current, "policy")));
    int rc = retained_roots_add(roots, json_member(current, "histories"));
    struct json *proofs = rc ? NULL : ret_closure(roots), *ref = ret_descriptor(reference);
    struct json *accepted = ret_new(JSON_OBJECT);
    char *path = ref ? ret_format("%s/accepted/%s.json", r->root, json_text(ref, "sha256")) : NULL;
    if (!proofs || !path)
        rc = -1;
    if (!rc) {
        ret_set(accepted, "format", ret_string("etude-retention-accepted-reference"));
        ret_set(accepted, "schema_version", ret_integer(1));
        ret_set(accepted, "reference", ret_clone(current));
        ret_set(accepted, "roots", roots);
        roots = NULL;
        ret_set(accepted, "proofs", proofs);
        proofs = NULL;
        struct stat st;
        if (lstat(path, &st))
            rc = errno == ENOENT ? ret_write(path, accepted, 0) : -1;
        else {
            struct json *prior = ret_document(path);
            rc = !prior || !json_equal(prior, accepted);
            json_free(prior);
        }
    }
    free(path);
    json_free(current);
    json_free(roots);
    json_free(proofs);
    json_free(ref);
    json_free(accepted);
    return rc;
}

static char *ret_allocate(struct retention *r, const char *reference)
{
    if (retained_accept_reference(r, reference))
        return NULL;
    struct json *report = ret_reclaim(r, reference, 1);
    const struct json *can = json_member(report, "can_allocate");
    if (!can || can->type != JSON_BOOLEAN || !can->integer) {
        ret_fail("retained evidence %lld bytes plus reservation %lld exceeds byte limit %lld; "
                 "refresh protections or increase policy capacity",
                 (long long)ret_number(report, "bytes"),
                 (long long)ret_number(r->policy, "reserve_bytes"),
                 (long long)ret_number(r->policy, "max_bytes"));
        json_free(report);
        return NULL;
    }
    json_free(report);
    char nonce[65], stamp[64];
    if (authority_nonce(nonce))
        return NULL;
    char *bundle = ret_format("%s/runs/run-%.32s", r->root, nonce);
    char *path = ret_format("%s/registry/run-%.32s.json", r->root, nonce);
    struct json *item = ret_new(JSON_OBJECT);
    int rc = !bundle || !path || mkdir(bundle, 0700);
    if (!rc) {
        ret_stamp(stamp);
        ret_set(item, "format", ret_string("etude-retained-bundle"));
        ret_set(item, "schema_version", ret_integer(1));
        ret_set(item, "path", ret_string(bundle));
        ret_set(item, "state", ret_string("allocated"));
        ret_set(item, "created_utc", ret_string(stamp));
        ret_set(item, "last_used", ret_integer(ret_now() + 1));
        ret_set(item, "reservation_bytes", ret_integer(ret_number(r->policy, "reserve_bytes")));
        rc = ret_write(path, item, 0);
    }
    free(path);
    json_free(item);
    if (rc) {
        free(bundle);
        bundle = NULL;
        ret_fail("retained bundle could not be allocated");
    }
    return bundle;
}

static int ret_empty(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir)
        return 0;
    struct dirent *entry;
    int empty = 1, rc = 0;
    while ((entry = ret_next(dir, &rc)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            empty = 0;
            break;
        }
    closedir(dir);
    return empty && !rc;
}

static int ret_begin(struct retention *r, const char *bundle, const char *run)
{
    char *path = NULL;
    struct json *item = NULL;
    int rc = ret_record(r, bundle, &path, &item);
    if (!rc &&
        (strcmp(json_text(item, "state"), "allocated") || !ret_empty(json_text(item, "path"))))
        rc = ret_fail("persistent OUT was already used");
    if (!rc) {
        ret_set(item, "state", ret_string("active"));
        ret_set(item, "run_id", ret_string(run));
        rc = retained_touch(item) || ret_write(path, item, 1);
    }
    free(path);
    json_free(item);
    return rc;
}

static int retained_proof_anchors(struct retention *r, const struct json *proofs)
{
    const struct json *anchors = json_member(r->policy, "anchors");
    struct json *paths = ret_new(JSON_ARRAY);
    int rc = 0;
    for (size_t i = 0; !rc && i < anchors->count; i++) {
        char *path = ret_checked(anchors->item[i]);
        if (!path)
            rc = -1;
        else
            ret_add_name(paths, path);
        free(path);
    }
    char *runs = ret_format("%s/runs", r->root);
    for (size_t i = 0; !rc && i < proofs->count; i++) {
        const char *path = json_text(proofs->item[i], "path");
        if (!path || (!ret_inside(runs, path) && !ret_has(paths, path)))
            rc = ret_fail("proof is outside registered bundles and immutable anchors");
    }
    free(runs);
    json_free(paths);
    return rc;
}

static struct json *ret_seal(struct retention *r, const char *bundle)
{
    char *path = NULL;
    struct json *item = NULL;
    if (ret_record(r, bundle, &path, &item))
        return NULL;
    if (strcmp(json_text(item, "state"), "active")) {
        free(path);
        json_free(item);
        ret_fail("bundle is not active");
        return NULL;
    }
    const char *run = json_text(item, "run_id"), *actual = json_text(item, "path");
    uint64_t size = 0;
    struct json *files = NULL, *roots = ret_new(JSON_ARRAY), *proofs = NULL;
    int rc = !run || ret_terminal(actual, run);
    if (!rc) {
        files = ret_files(actual, &size, NULL);
        rc = !files;
    }
    for (size_t i = 0; !rc && i < files->count; i++)
        if (!strncmp(files->item[i]->name, "history/", 8))
            ret_push(roots, ret_clone(files->item[i]));
    if (!rc) {
        proofs = ret_closure(roots);
        rc = !proofs;
        if (proofs) {
            ret_set(item, "proofs", proofs);
            proofs = NULL;
        }
    }
    if (!rc)
        rc = retained_proof_anchors(r, json_member(item, "proofs"));
    if (!rc) {
        char stamp[64];
        ret_stamp(stamp);
        ret_set(item, "state", ret_string("sealed"));
        ret_set(item, "files", files);
        files = NULL;
        ret_set(item, "sealed_utc", ret_string(stamp));
    } else {
        ret_set(item, "state", ret_string("protected"));
        ret_set(item, "reason", ret_string(ret_error()));
    }
    ret_set(item, "last_used", ret_integer(ret_now() + 1));
    if (ret_write(path, item, 1)) {
        json_free(item);
        item = NULL;
    }
    free(path);
    json_free(files);
    json_free(roots);
    json_free(proofs);
    return item;
}

static int retained_parent_check(const char *root)
{
    char *path = strdup(root);
    if (!path)
        return -1;
    int rc = 0;
    for (;;) {
        char *slash = strrchr(path, '/');
        if (!slash)
            break;
        if (slash == path)
            path[1] = 0;
        else if (slash == path + 2 && path[1] == ':')
            path[3] = 0;
        else
            *slash = 0;
        struct stat st;
        if (!lstat(path, &st) && !S_ISDIR(st.st_mode)) {
            rc = -1;
            break;
        }
        char *git = ret_format("%s/.git", path);
        if (!git || !lstat(git, &st))
            rc = -1;
        free(git);
        if (rc || !strcmp(path, "/") || (strlen(path) == 3 && path[1] == ':'))
            break;
    }
    free(path);
    return rc ? ret_fail("retention root has an aliased parent or is inside a checkout") : 0;
}

static int ret_initialize(const char *policy_path, const char *site)
{
    struct json *policy = ret_document(policy_path);
    if (!policy || ret_policy_check(policy, site)) {
        json_free(policy);
        return -1;
    }
    const char *given = json_text(policy, "root");
    struct stat st;
    char *root = given && ret_absolute(given) ? ret_path(given, 0) : NULL;
    int rc = !root || !lstat(root, &st) || errno != ENOENT || retained_parent_check(root);
    if (!rc)
        rc = ret_dirs(root);
    const char *names[] = {"registry", "runs", "accepted"};
    for (size_t i = 0; !rc && i < 3; i++) {
        char *path = ret_format("%s/%s", root, names[i]);
        rc = !path || mkdir(path, 0700);
        free(path);
    }
    if (!rc) {
        struct json *marker = ret_new(JSON_OBJECT);
        ret_set(marker, "format", ret_string("etude-retention-root"));
        ret_set(marker, "schema_version", ret_integer(1));
        ret_set(marker, "root", ret_string(root));
        ret_set(marker, "site_sha256", ret_string(json_text(policy, "site_sha256")));
        char *path = ret_format("%s/.etude-retention-root.json", root);
        rc = !path || ret_write(path, marker, 0);
        free(path);
        json_free(marker);
    }
    if (!rc)
        puts(root);
    free(root);
    json_free(policy);
    return rc ? ret_fail("retention root must be a new absolute owned directory") : 0;
}

struct etude_retention_guard {
    struct retention registry;
};
int etude_retention_read_begin(const char *const *paths, int bundle, volatile sig_atomic_t *stop,
                               struct etude_retention_guard **guard)
{
    *guard = NULL;
    const char *policy = getenv("ETUDE_RETENTION");
    if (!policy || !*policy)
        return 0;
    struct etude_retention_guard *owned = json_alloc(sizeof(*owned));
    if (ret_registry_open(&owned->registry, policy, getenv("ETUDE_SITE"), stop)) {
        free(owned);
        return -1;
    }
    struct json *values = ret_new(JSON_OBJECT), *roots = ret_new(JSON_ARRAY),
                *touched = ret_new(JSON_ARRAY);
    int rc = 0;
    for (size_t i = 0; paths && paths[i]; i++) {
        if (bundle) {
            char *path = ret_path(paths[i], 1);
            if (!path) {
                rc = -1;
                break;
            }
            ret_set(values, path, ret_string(path));
            free(path);
        } else {
            struct json *ref = ret_descriptor(paths[i]);
            if (!ref) {
                rc = -1;
                break;
            }
            ret_push(roots, ref);
        }
    }
    if (!rc && !bundle) {
        json_free(values);
        values = ret_closure(roots);
        rc = !values;
    }
    char *prefix = ret_format("%s/runs/", owned->registry.root);
    for (size_t i = 0; !rc && i < values->count; i++) {
        const char *path = bundle ? values->item[i]->string : json_text(values->item[i], "path");
        if (strncmp(path, prefix, strlen(prefix))) {
            if (bundle)
                rc = ret_fail("evidence source is not a registered persistent bundle");
            continue;
        }
        const char *start = path + strlen(prefix);
        char *name = strndup(start, strcspn(start, "/"));
        if (!ret_has(touched, name)) {
            char *member = ret_format("%s%s", prefix, name), *record = NULL;
            struct json *item = NULL;
            rc = ret_record(&owned->registry, member, &record, &item) || retained_touch(item) ||
                 ret_write(record, item, 1);
            free(member);
            free(record);
            json_free(item);
            if (!rc)
                ret_add_name(touched, name);
        }
        free(name);
    }
    free(prefix);
    json_free(values);
    json_free(roots);
    json_free(touched);
    if (rc) {
        etude_retention_read_end(owned);
        return -1;
    }
    *guard = owned;
    return 0;
}
void etude_retention_read_end(struct etude_retention_guard *guard)
{
    if (guard) {
        ret_registry_close(&guard->registry);
        free(guard);
    }
}

struct retention_options {
    const char *policy, *site, *action, *reference, *github_env, *output, *run;
    const char *repository, *mapping;
    int apply;
};
static int retention_options_valid(const struct retention_options *o)
{
    enum { OUT = 1, RUN = 2, REF = 4, ENV = 8, APPLY = 16, REPO = 32, MAP = 64 };
    const char *names[] = {"init", "allocate", "prune", "begin", "seal", "touch", "issues"};
    const unsigned allowed[] = {0, REF | ENV, REF | APPLY, OUT | RUN, OUT, OUT, OUT | REPO | MAP};
    const unsigned required[] = {0, 0, 0, OUT | RUN, OUT, OUT, OUT | REPO | MAP};
    if (!o->action)
        return -1;
    size_t action;
    for (action = 0; action < 7; action++)
        if (!strcmp(o->action, names[action]))
            break;
    if (action == 7 || (action != 6 && (!o->policy || !o->site)))
        return -1;
    unsigned fields = (o->output != NULL) | ((o->run != NULL) << 1) |
                      ((o->reference != NULL) << 2) | ((o->github_env != NULL) << 3) |
                      ((o->apply != 0) << 4) | ((o->repository != NULL) << 5) |
                      ((o->mapping != NULL) << 6);
    return (fields & ~allowed[action]) || (fields & required[action]) != required[action] ? -1 : 0;
}

static const char **retention_option_slot(struct retention_options *o, const char *word)
{
    const char *names[] = {"--policy",     "--site",    "--reference", "--github-env",
                           "--repository", "--mapping", "--output"};
    const char **slots[] = {&o->policy,     &o->site,    &o->reference, &o->github_env,
                            &o->repository, &o->mapping, &o->output};
    for (size_t i = 0; i < sizeof names / sizeof(*names); i++)
        if (!strcmp(word, names[i]))
            return slots[i];
    return NULL;
}

static int retention_options(int argc, char **argv, struct retention_options *o)
{
    memset(o, 0, sizeof(*o));
    int i = 1;
    for (; i < argc; i++) {
        const char *word = argv[i];
        if (!strcmp(word, "--apply")) {
            o->apply = 1;
            continue;
        }
        const char **slot = retention_option_slot(o, word);
        if (slot) {
            if (++i == argc || *slot)
                return -1;
            *slot = argv[i];
        } else if (*word == '-')
            return -1;
        else if (!o->action)
            o->action = word;
        else if (!o->output)
            o->output = word;
        else if (!o->run)
            o->run = word;
        else
            return -1;
    }
    return retention_options_valid(o);
}

static int retention_action(struct retention *r, const struct retention_options *o)
{
    int rc = 0;
    if (!strcmp(o->action, "allocate")) {
        char *bundle = ret_allocate(r, o->reference);
        if (!bundle)
            return -1;
        if (o->github_env) {
            FILE *env = fopen(o->github_env, "ab");
            if (!env || fprintf(env, "ETUDE_RESULTS=%s\n", bundle) < 0)
                rc = -1;
            if (env && fclose(env))
                rc = -1;
        }
        if (!rc)
            puts(bundle);
        free(bundle);
    } else if (!strcmp(o->action, "begin"))
        rc = ret_begin(r, o->output, o->run);
    else if (!strcmp(o->action, "touch")) {
        char *path = NULL;
        struct json *item = NULL;
        rc = ret_record(r, o->output, &path, &item) || retained_touch(item) ||
             ret_write(path, item, 1);
        free(path);
        json_free(item);
    } else {
        struct json *result = !strcmp(o->action, "seal") ? ret_seal(r, o->output)
                                                         : ret_reclaim(r, o->reference, o->apply);
        if (!result)
            return -1;
        struct jbuf text = {0};
        if (!strcmp(o->action, "seal")) {
            struct json *summary = ret_new(JSON_OBJECT);
            ret_set(summary, "state", ret_clone(json_member(result, "state")));
            const struct json *reason = json_member(result, "reason");
            ret_set(summary, "reason", reason ? ret_clone(reason) : ret_new(JSON_NULL));
            json_canonical(summary, &text);
            json_free(summary);
        } else
            json_canonical(result, &text);
        printf("%s\n", text.data);
        jbuf_free(&text);
        json_free(result);
    }
    return rc;
}
int etude_retention_main(int argc, char **argv, volatile sig_atomic_t *stop)
{
    struct retention_options options;
    if (stop && *stop)
        return 128 + *stop;
    int rc = retention_options(argc, argv, &options);
    if (rc)
        ret_fail("usage: etude prepare retention --policy POLICY --site SITE "
                 "init|allocate|begin|seal|touch|prune");
    else if (!strcmp(options.action, "init"))
        rc = ret_initialize(options.policy, options.site);
    else if (!strcmp(options.action, "issues"))
        rc = ret_issue_snapshot(options.repository, options.mapping, options.output, stop);
    else {
        struct retention registry;
        rc = ret_registry_open(&registry, options.policy, options.site, stop);
        if (!rc) {
            rc = retention_action(&registry, &options);
            ret_registry_close(&registry);
        }
    }
    if (rc)
        fprintf(stderr, "ci: retention: %s\n", ret_error());
    return stop && *stop ? 128 + *stop : rc ? 2 : 0;
}
