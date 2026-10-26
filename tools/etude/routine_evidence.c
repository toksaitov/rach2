/* Copy reporting evidence and its non-executable routine proof closure. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "measurement_context.h"
#include "retention.h"
#include "routine_evidence.h"

struct proof_copy {
    const char *source, *out;
    char *directory, *original;
    struct json *pending, *known, *seen, *errors;
    volatile sig_atomic_t *stop;
    int packing;
};

static int evidence_stopped(volatile sig_atomic_t *stop)
{
    return stop && *stop ? ret_fail("evidence copy interrupted") : 0;
}
static int evidence_hash(const char *hash)
{
    return hash && strlen(hash) == 64 && strspn(hash, "0123456789abcdef") == 64;
}
static int evidence_exists(const char *path)
{
    struct stat st;
    if (!lstat(path, &st))
        return 1;
    return errno == ENOENT ? 0 : -1;
}
static int evidence_regular(const char *path)
{
    struct stat st;
    return !lstat(path, &st) && S_ISREG(st.st_mode);
}
static int evidence_directory(const char *path)
{
    struct stat st;
    return !lstat(path, &st) && S_ISDIR(st.st_mode);
}

/* Check the spelling before canonicalization can hide a symlink parent. */
static int evidence_parents(const char *path)
{
    char *copy = strdup(path);
    int rc = !copy;
    for (char *p = copy; !rc && *p; p++) {
        if ((*p != '/' && *p != '\\') || p == copy || p[-1] == ':' || p[-1] == '/')
            continue;
        char saved = *p;
        *p = 0;
        struct stat st;
        int status = lstat(copy, &st);
        if ((!status && !S_ISDIR(st.st_mode)) || (status && errno != ENOENT))
            rc = ret_fail("evidence parent is not a directory: %s", copy);
        *p = saved;
    }
    free(copy);
    return rc;
}

static int evidence_destination(const char *path)
{
    struct stat st;
    if (evidence_parents(path))
        return -1;
    int status = lstat(path, &st);
    return (!status && !S_ISDIR(st.st_mode)) || (status && errno != ENOENT)
               ? ret_fail("evidence destination is not an ordinary directory")
               : 0;
}

static int evidence_member(const char *name)
{
    if (!name || !*name || *name == '/' || strpbrk(name, ":\\\r\n"))
        return 0;
    const char *at = name;
    do {
        const char *end = strchr(at, '/');
        size_t n = end ? (size_t)(end - at) : strlen(at);
        if (!n || (n == 1 && *at == '.') || (n == 2 && !strncmp(at, "..", 2)))
            return 0;
        at = end ? end + 1 : NULL;
    } while (at);
    return 1;
}

static int evidence_same(const struct json *a, const struct json *b)
{
    const char *ah = json_text(a, "sha256"), *bh = json_text(b, "sha256");
    return ah && bh && !strcmp(ah, bh) && ret_number(a, "size_bytes") >= 0 &&
           ret_number(a, "size_bytes") == ret_number(b, "size_bytes");
}

/* Publish no destination over an existing name, even after a failed copy. */
static int evidence_copy(const char *source, const char *target, const struct json *expected,
                         volatile sig_atomic_t *stop)
{
    struct json *before = ret_descriptor(source);
    if (!before || (expected && !evidence_same(before, expected))) {
        json_free(before);
        return ret_fail("evidence source bytes differ: %s", source);
    }
    int in = open(source, O_RDONLY | O_NOFOLLOW), out = -1;
    if (in >= 0)
        out = open(target, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    int rc = in < 0 || out < 0;
    char bytes[65536];
    ssize_t n;
    while (!rc && (n = read(in, bytes, sizeof bytes)) > 0) {
        rc = evidence_stopped(stop);
        for (ssize_t at = 0; !rc && at < n;) {
            ssize_t written = write(out, bytes + at, (size_t)(n - at));
            if (written <= 0)
                rc = -1;
            else
                at += written;
        }
    }
    if (!rc && (n < 0 || fsync(out)))
        rc = -1;
    if (in >= 0 && close(in))
        rc = -1;
    if (out >= 0 && close(out))
        rc = -1;
    struct json *after = !rc ? ret_descriptor(source) : NULL;
    struct json *copied = !rc ? ret_descriptor(target) : NULL;
    if (!rc && (!evidence_same(before, after) || !evidence_same(before, copied) ||
                !json_equal(before, after)))
        rc = -1;
    json_free(before);
    json_free(after);
    json_free(copied);
    return rc ? ret_fail("evidence copy failed or bytes changed: %s", source) : 0;
}

static int evidence_compare_name(const void *a, const void *b)
{
    const struct json *const *aa = a, *const *bb = b;
    return strcmp((*aa)->string, (*bb)->string);
}
static struct json *evidence_names(const char *root)
{
    DIR *dir = opendir(root);
    struct json *names = ret_new(JSON_ARRAY);
    int rc = !dir;
    struct dirent *entry;
    while (!rc && (entry = ret_next(dir, &rc)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
            ret_push(names, ret_string(entry->d_name));
    if (dir && closedir(dir))
        rc = -1;
    if (rc) {
        json_free(names);
        return NULL;
    }
    qsort(names->item, names->count, sizeof(*names->item), evidence_compare_name);
    return names;
}

static int evidence_known(struct proof_copy *copy, const char *path, unsigned depth)
{
    if (depth > 64 || evidence_stopped(copy->stop))
        return ret_fail("proof input directory is too deep or interrupted");
    struct stat st;
    if (lstat(path, &st))
        return -1;
    if (S_ISREG(st.st_mode)) {
        struct json *ref = ret_descriptor(path);
        char *key = ref ? ret_format("%s:%lld", json_text(ref, "sha256"),
                                     (long long)ret_number(ref, "size_bytes"))
                        : NULL;
        int rc = !ref || !key;
        if (!rc && !json_member(copy->known, key))
            rc = ret_set(copy->known, key, ret_clone(ref));
        json_free(ref);
        free(key);
        return rc;
    }
    if (!S_ISDIR(st.st_mode))
        return 0;
    struct json *names = evidence_names(path);
    int rc = !names;
    for (size_t i = 0; !rc && i < names->count; i++) {
        char *child = ret_format("%s/%s", path, names->item[i]->string);
        rc = evidence_known(copy, child, depth + 1);
        free(child);
    }
    json_free(names);
    return rc;
}

static char *evidence_candidate(const char *path, const struct json *ref)
{
    if (!path || !evidence_regular(path))
        return NULL;
    struct json *actual = ret_descriptor(path);
    char *result = evidence_same(actual, ref) ? strdup(path) : NULL;
    json_free(actual);
    return result;
}

static const char *evidence_location(const struct json *ref, int packing)
{
    const struct json *locations = json_member(ref, "locations");
    const char *sha = json_text(ref, "sha256");
    int64_t size = ret_number(ref, "size_bytes");
    const char *location = locations && locations->type == JSON_ARRAY && locations->count == 1 &&
                                   locations->item[0]->type == JSON_STRING
                               ? locations->item[0]->string
                               : NULL;
    if (!evidence_hash(sha) || size < 0 || size > 64 * 1024 * 1024 || !location ||
        (!packing && !(ret_absolute(location) || (location[0] && location[1] == ':' &&
                                                  (location[2] == '/' || location[2] == '\\'))))) {
        ret_fail("invalid or ambiguous routine proof reference");
        return NULL;
    }
    return location;
}

static char *evidence_resolve(struct proof_copy *copy, const struct json *ref)
{
    const char *location = evidence_location(ref, copy->packing);
    if (!location)
        return NULL;
    const char *sha = json_text(ref, "sha256");
    int64_t size = ret_number(ref, "size_bytes");
    char *path = evidence_candidate(location, ref);
    char *candidate = ret_format("%s/%s", copy->directory, sha);
    if (!path)
        path = evidence_candidate(candidate, ref);
    free(candidate);
    candidate = ret_format("%s/routine-proofs/%s", copy->source, sha);
    if (!path)
        path = evidence_candidate(candidate, ref);
    free(candidate);
    if (!copy->packing)
        return path;
    candidate = ret_format("%s/store/objects/%s", copy->out, sha);
    if (!path)
        path = evidence_candidate(candidate, ref);
    free(candidate);
    if (!path && copy->original && ret_inside(copy->original, location)) {
        candidate = ret_format("%s/%s", copy->out, location + strlen(copy->original) + 1);
        path = evidence_candidate(candidate, ref);
        free(candidate);
    }
    const char *base = location;
    for (const char *p = location; *p; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    const char *directories[] = {"", "history/"};
    for (size_t i = 0; !path && i < 2; i++) {
        candidate = ret_format("%s/%s%s", copy->out, directories[i], base);
        path = evidence_candidate(candidate, ref);
        free(candidate);
    }
    char *key = ret_format("%s:%lld", sha, (long long)size);
    const struct json *known = json_member(copy->known, key);
    if (!path && known)
        path = evidence_candidate(json_text(known, "path"), ref);
    free(key);
    return path;
}

static int evidence_references(struct proof_copy *copy, const struct json *value)
{
    if (!value)
        return 0;
    if (value->type == JSON_OBJECT && json_member(value, "sha256") &&
        json_member(value, "size_bytes") && json_member(value, "locations"))
        return ret_push(copy->pending, ret_clone(value));
    if (value->type == JSON_ARRAY || value->type == JSON_OBJECT)
        for (size_t i = 0; i < value->count; i++)
            if (evidence_references(copy, value->item[i]))
                return -1;
    return 0;
}

/* Histories retain preparation and load proofs, without executable copies. */
static int evidence_document(struct proof_copy *copy, const struct json *value)
{
    const char *kind = json_text(value, "kind"), *format = json_text(value, "format");
    if (kind && (!strcmp(kind, "plan") || !strcmp(kind, "run"))) {
        const struct json *data = json_member(value, "data");
        return evidence_references(copy,
                                   json_member(data, !strcmp(kind, "plan") ? "artifacts" : "load"));
    }
    if (format && !strcmp(format, "etude-preparation-nodes")) {
        const char *names[] = {"routine_grant", "upstream_preparation", "preparation"};
        for (size_t i = 0; i < 3; i++)
            if (evidence_references(copy, json_member(value, names[i])))
                return -1;
        return 0;
    }
    return evidence_references(copy, value);
}

static int evidence_documents(struct proof_copy *copy, const char *path)
{
    FILE *file = fopen(path, "rb");
    struct jbuf bytes = {0};
    char chunk[65536], error[256];
    size_t n;
    int rc = !file;
    while (!rc && (n = fread(chunk, 1, sizeof chunk, file))) {
        rc = evidence_stopped(copy->stop);
        if (!rc)
            jbuf_put(&bytes, chunk, n);
    }
    if (file) {
        if (ferror(file))
            rc = -1;
        fclose(file);
    }
    struct json *value = NULL;
    if (!rc &&
        !json_parse_data(bytes.data ? bytes.data : "", bytes.len, 1, &value, error, sizeof error))
        rc = evidence_document(copy, value);
    else if (!rc) {
        const char *at = bytes.data ? bytes.data : "", *end;
        while (!rc && *at) {
            end = strchr(at, '\n');
            size_t size = end ? (size_t)(end - at) : strlen(at);
            if (!json_parse_data(at, size, 1, &value, error, sizeof error))
                rc = evidence_document(copy, value);
            json_free(value);
            value = NULL;
            at += size + (end != NULL);
        }
    }
    json_free(value);
    jbuf_free(&bytes);
    return rc;
}

static int evidence_seeds(struct proof_copy *copy)
{
    const char *names[] = {"routine.grant.json", "routine.upstream.json",
                           "routine.completion.json"};
    int rc = 0;
    for (size_t i = 0; !rc && i < 3; i++) {
        char *path = ret_format("%s/%s", copy->source, names[i]);
        if (evidence_regular(path))
            rc = evidence_documents(copy, path);
        free(path);
    }
    char *history = ret_format("%s/history", copy->source);
    struct json *entries =
        evidence_directory(history) ? evidence_names(history) : ret_new(JSON_ARRAY);
    if (!entries)
        rc = -1;
    for (size_t i = 0; !rc && i < entries->count; i++) {
        char *path = ret_format("%s/%s", history, entries->item[i]->string);
        if (evidence_regular(path))
            rc = evidence_documents(copy, path);
        free(path);
    }
    json_free(entries);
    free(history);
    return rc;
}

static int evidence_proof(struct proof_copy *copy, const struct json *ref)
{
    char *source = evidence_resolve(copy, ref);
    const char *sha = json_text(ref, "sha256");
    if (!source)
        return ret_fail("missing, changed or ambiguous routine proof");
    const struct json *seen = json_member(copy->seen, sha);
    int rc = 0;
    if (seen) {
        rc = evidence_same(seen, ref) ? 0 : ret_fail("conflicting routine proof size");
        free(source);
        return rc;
    }
    char *target = ret_format("%s/%s", copy->directory, sha);
    if (evidence_exists(target)) {
        struct json *actual = ret_descriptor(target);
        rc = evidence_same(actual, ref) ? 0 : ret_fail("changed retained routine proof");
        json_free(actual);
    } else
        rc = evidence_copy(source, target, ref, copy->stop);
    if (!rc && chmod(target, 0444))
        rc = -1;
    if (!rc)
        rc = ret_set(copy->seen, sha, ret_clone(ref)) || evidence_documents(copy, target);
    free(source);
    free(target);
    return rc;
}

static int evidence_pack_inputs(struct proof_copy *copy, const char *const roots[])
{
    int rc = 0;
    char *request = ret_format("%s/routine.request.json", copy->out);
    if (evidence_regular(request)) {
        struct json *value = ret_document(request);
        const char *original = json_text(value, "out");
        copy->original = original ? strdup(original) : NULL;
        if (!value)
            rc = -1;
        json_free(value);
    }
    free(request);
    char *input = ret_format("%s/routine-proofs-input", copy->out);
    if (!rc && evidence_directory(input))
        rc = evidence_known(copy, input, 0);
    free(input);
    for (size_t i = 0; !rc && roots && roots[i]; i++)
        if (evidence_directory(roots[i]))
            rc = evidence_known(copy, roots[i], 0);
    return rc;
}

static int evidence_proofs(const char *source, const char *out, const char *const roots[],
                           int packing, volatile sig_atomic_t *stop)
{
    struct proof_copy copy = {.source = source, .out = out, .packing = packing, .stop = stop};
    char *grant = ret_format("%s/routine.grant.json", source);
    int present = evidence_regular(grant);
    free(grant);
    if (!packing && !present)
        return 0;
    copy.directory = ret_format("%s/routine-proofs", out);
    copy.pending = ret_new(JSON_ARRAY);
    copy.known = ret_new(JSON_OBJECT);
    copy.seen = ret_new(JSON_OBJECT);
    copy.errors = ret_new(JSON_ARRAY);
    int rc = mkdir(copy.directory, 0700) &&
             !(packing && errno == EEXIST && evidence_directory(copy.directory));
    if (!rc && packing)
        rc = evidence_pack_inputs(&copy, roots);
    if (!rc)
        rc = evidence_seeds(&copy);
    for (size_t at = 0; !rc && at < copy.pending->count; at++) {
        rc = evidence_stopped(stop) || evidence_proof(&copy, copy.pending->item[at]);
        if (rc && !packing && !(stop && *stop)) {
            ret_push(copy.errors, ret_string(ret_error()));
            rc = 0;
        }
    }
    if (!rc && copy.errors->count) {
        char *errors = ret_format("%s/routine-proof-errors.json", out);
        rc = ret_write(errors, copy.errors, 0);
        free(errors);
    }
    free(copy.directory);
    free(copy.original);
    json_free(copy.pending);
    json_free(copy.known);
    json_free(copy.seen);
    json_free(copy.errors);
    return rc;
}

static int evidence_skip(const char *name)
{
    return !strcmp(name, "objects") || !strcmp(name, "native-evidence") ||
           !strcmp(name, "routine-proofs") || !strncmp(name, "core-", 5) ||
           !strncmp(name, "full-", 5) || !strncmp(name, "push-", 5);
}

static int evidence_tree(const char *source, const char *out, const char *relative,
                         struct json *inventory, volatile sig_atomic_t *stop, unsigned depth)
{
    if (depth > 64 || evidence_stopped(stop))
        return -1;
    char *path = *relative ? ret_format("%s/%s", source, relative) : strdup(source);
    struct stat st;
    int rc = lstat(path, &st);
    if (!rc && S_ISDIR(st.st_mode)) {
        struct json *names = evidence_names(path);
        rc = !names;
        for (size_t i = 0; !rc && i < names->count; i++) {
            const char *name = names->item[i]->string;
            if (!*relative && evidence_skip(name))
                continue;
            char *child = *relative ? ret_format("%s/%s", relative, name) : strdup(name);
            rc = evidence_tree(source, out, child, inventory, stop, depth + 1);
            free(child);
        }
        json_free(names);
    } else if (!rc && S_ISREG(st.st_mode)) {
        char *target = ret_format("%s/%s", out, relative), *slash = strrchr(target, '/');
        *slash = 0;
        rc = ret_dirs(target);
        *slash = '/';
        if (!rc)
            rc = evidence_copy(path, target, NULL, stop);
        struct json *ref = !rc ? ret_descriptor(target) : NULL;
        if (!rc && !ref)
            rc = -1;
        if (!rc) {
            ret_set(ref, "path", ret_string(relative));
            rc = ret_push(inventory, ref);
        } else
            json_free(ref);
        free(target);
    } else if (!rc)
        rc = ret_fail("evidence contains a link or special file: %s", path);
    free(path);
    return rc;
}

static int evidence_extra(const char *out, struct json *files)
{
    char *proofs = ret_format("%s/routine-proofs", out);
    uint64_t size;
    struct json *refs = evidence_directory(proofs) ? ret_files(proofs, &size, NULL) : NULL;
    int rc = 0;
    if (evidence_directory(proofs) && !refs)
        rc = -1;
    for (size_t i = 0; !rc && refs && i < refs->count; i++) {
        struct json *ref = ret_clone(refs->item[i]);
        char *path = ret_format("routine-proofs/%s", refs->item[i]->name);
        ret_set(ref, "path", ret_string(path));
        free(path);
        rc = ret_push(files, ref);
    }
    char *errors = ret_format("%s/routine-proof-errors.json", out);
    if (!rc && evidence_regular(errors)) {
        struct json *ref = ret_descriptor(errors);
        if (ref) {
            ret_set(ref, "path", ret_string("routine-proof-errors.json"));
            rc = ret_push(files, ref);
        } else
            rc = -1;
    }
    free(errors);
    free(proofs);
    json_free(refs);
    return rc;
}

static int evidence_stage(const char *source, const char *out, volatile sig_atomic_t *stop)
{
    const char *paths[] = {source, NULL};
    struct etude_retention_guard *guard = NULL;
    if (!strcmp(source, out) || ret_inside(source, out) || evidence_parents(out) ||
        etude_retention_read_begin(paths, 1, stop, &guard))
        return ret_fail("evidence staging needs a separate, available destination");
    char *parent = strdup(out), *slash = strrchr(parent, '/');
    int rc = !slash;
    if (slash) {
        *slash = 0;
        rc = ret_dirs(*parent ? parent : "/") || mkdir(out, 0700);
    }
    free(parent);
    struct json *files = ret_new(JSON_ARRAY);
    if (!rc)
        rc = evidence_proofs(source, out, NULL, 0, stop) ||
             evidence_tree(source, out, "", files, stop, 0) || evidence_extra(out, files);
    struct json *inventory = ret_new(JSON_OBJECT);
    ret_set(inventory, "format", ret_string("etude-native-evidence"));
    ret_set(inventory, "schema_version", ret_integer(1));
    ret_set(inventory, "files", files);
    if (!rc) {
        char *path = ret_format("%s/evidence.json", out);
        rc = ret_write(path, inventory, 0);
        free(path);
    }
    json_free(inventory);
    etude_retention_read_end(guard);
    return rc;
}

static int evidence_restore_check(const char *source, const char *out, const struct json *files)
{
    struct json *seen = ret_new(JSON_ARRAY);
    int rc = !files || files->type != JSON_ARRAY;
    for (size_t i = 0; !rc && i < files->count; i++) {
        const struct json *ref = files->item[i];
        const char *name = json_text(ref, "path");
        if (!evidence_member(name) || ret_has(seen, name)) {
            rc = ret_fail("invalid or duplicate evidence member");
            break;
        }
        ret_add_name(seen, name);
        char *path = ret_format("%s/%s", source, name), *target = ret_format("%s/%s", out, name);
        struct json *actual = ret_descriptor(path);
        rc = evidence_parents(path) || evidence_parents(target) || evidence_exists(target) ||
             !evidence_hash(json_text(ref, "sha256")) || !evidence_same(ref, actual);
        json_free(actual);
        free(path);
        free(target);
    }
    json_free(seen);
    return rc ? ret_fail("invalid, changed or already present evidence member") : 0;
}

static int evidence_restore(const char *source, const char *out, volatile sig_atomic_t *stop)
{
    char *index = ret_format("%s/evidence.json", source);
    struct json *inventory = ret_document(index);
    const char *format = json_text(inventory, "format");
    const struct json *files = json_member(inventory, "files");
    int rc = !format || strcmp(format, "etude-native-evidence") ||
             ret_number(inventory, "schema_version") != 1 || evidence_parents(out) ||
             evidence_restore_check(source, out, files);
    for (size_t i = 0; !rc && i < files->count; i++) {
        const struct json *ref = files->item[i];
        const char *name = json_text(ref, "path");
        char *path = ret_format("%s/%s", source, name), *target = ret_format("%s/%s", out, name);
        char *slash = strrchr(target, '/');
        *slash = 0;
        rc = evidence_stopped(stop) || ret_dirs(target);
        *slash = '/';
        if (!rc)
            rc = evidence_copy(path, target, ref, stop);
        free(path);
        free(target);
    }
    free(index);
    json_free(inventory);
    return rc;
}

int etude_routine_pack(const char *out, const char *const roots[], volatile sig_atomic_t *stop)
{
    char *path = ret_path(out, 1);
    int rc = !path || evidence_destination(out) || evidence_proofs(path, path, roots, 1, stop);
    free(path);
    return rc;
}

int etude_prepare_evidence_main(int argc, char **argv, volatile sig_atomic_t *stop)
{
    if (argc >= 3 && !strcmp(argv[1], "pack")) {
        int rc = etude_routine_pack(argv[2], (const char *const *)(argv + 3), stop);
        if (rc)
            fprintf(stderr, "etude prepare evidence: %s\n", ret_error());
        return rc ? 2 : 0;
    }
    if (argc != 4 || (strcmp(argv[1], "stage") && strcmp(argv[1], "restore"))) {
        fprintf(stderr, "etude prepare evidence stage|restore SOURCE OUTPUT\n");
        return 2;
    }
    char *source = ret_path(argv[2], 1), *out = ret_path(argv[3], 0);
    int rc = !source || !out || evidence_destination(argv[3]);
    if (!rc)
        rc = !strcmp(argv[1], "stage") ? evidence_stage(source, out, stop)
                                       : evidence_restore(source, out, stop);
    if (rc)
        fprintf(stderr, "etude prepare evidence: %s\n", ret_error());
    free(source);
    free(out);
    return rc ? 2 : 0;
}
