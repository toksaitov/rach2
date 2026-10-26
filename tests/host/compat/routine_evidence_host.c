/* Retain exact proof locators across staging, restoration and reader checks. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include "../../../tools/etude/win/files.h"
#endif
#define fix_dir    evidence_fix_dir
#define fix_path   evidence_fix_path
#define fix_passed evidence_fix_passed
#define fix_format evidence_fix_format
#include "fixdir_host.c"
#include "../../../tools/etude/retention.h"
#include "../../../tools/etude/routine_evidence.h"

static char *evidence_base;
static int evidence_failures;
#define EVIDENCE_CHECK(test)                                                             \
    do {                                                                                 \
        if (!(test)) {                                                                   \
            fprintf(stderr, "evidence fixture: %s:%d: %s\n", __FILE__, __LINE__, #test); \
            evidence_failures++;                                                         \
        }                                                                                \
    } while (0)

static char *evidence_path(const char *name)
{
    return ret_format("%s/%s", evidence_base, name);
}
static void evidence_put(const char *path, const char *bytes)
{
    FILE *file = fopen(path, "wb");
    if (!file || fputs(bytes, file) < 0 || fclose(file))
        exit(2);
}
static int evidence_file(const char *path)
{
    struct stat st;
    return !lstat(path, &st) && S_ISREG(st.st_mode);
}
static struct json *evidence_ref(const char *path, const char *location)
{
    struct json *actual = ret_descriptor(path), *ref = ret_new(JSON_OBJECT);
    struct json *locations = ret_new(JSON_ARRAY);
    EVIDENCE_CHECK(actual != NULL);
    ret_set(ref, "sha256", ret_clone(json_member(actual, "sha256")));
    ret_set(ref, "size_bytes", ret_clone(json_member(actual, "size_bytes")));
    ret_push(locations, ret_string(location ? location : path));
    ret_set(ref, "locations", locations);
    json_free(actual);
    return ref;
}
static void evidence_grant(const char *source, const struct json *ref)
{
    char *path = ret_format("%s/routine.grant.json", source);
    struct json *grant = ret_new(JSON_OBJECT);
    ret_set(grant, "proof", ret_clone(ref));
    EVIDENCE_CHECK(!ret_write(path, grant, 1));
    json_free(grant);
    free(path);
}
static int evidence_call(const char *action, const char *source, const char *output)
{
    char *argv[] = {"evidence", (char *)action, (char *)source, (char *)output, NULL};
    return etude_prepare_evidence_main(4, argv, NULL);
}
static int evidence_errors(const char *output)
{
    char *path = ret_format("%s/routine-proof-errors.json", output);
    int present = evidence_file(path);
    free(path);
    return present;
}

static void evidence_transport(void)
{
    char *source = evidence_path("routine-source"), *leaf = evidence_path("proof bytes");
    EVIDENCE_CHECK(!ret_dirs(source));
    evidence_put(leaf, "actual proof bytes\n");
    struct json *ref = evidence_ref(leaf, NULL);
    evidence_grant(source, ref);
    char *grant = ret_format("%s/routine.grant.json", source);
    struct json *completion = ret_new(JSON_OBJECT);
    ret_set(completion, "routine_grant", evidence_ref(grant, NULL));
    char *path = ret_format("%s/routine.completion.json", source);
    EVIDENCE_CHECK(!ret_write(path, completion, 0));
    free(path);
    json_free(completion);
    char *objects = ret_format("%s/objects", source);
    EVIDENCE_CHECK(!ret_dirs(objects));
    char *executable = ret_format("%s/do-not-transport", objects);
    evidence_put(executable, "executable object bytes");
    char *out = evidence_path("transport"), *restored = evidence_path("restored");
    EVIDENCE_CHECK(!evidence_call("stage", source, out));
    EVIDENCE_CHECK(!evidence_errors(out));
    path = ret_format("%s/routine-proofs/%s", out, json_text(ref, "sha256"));
    struct json *copied = ret_descriptor(path);
    EVIDENCE_CHECK(copied && !strcmp(json_text(ref, "sha256"), json_text(copied, "sha256")));
    json_free(copied);
    free(path);
    path = ret_format("%s/objects/do-not-transport", out);
    EVIDENCE_CHECK(access(path, F_OK) != 0);
    free(path);
    EVIDENCE_CHECK(!evidence_call("restore", out, restored));
    EVIDENCE_CHECK(evidence_call("restore", out, restored) == 2);
    path = ret_format("%s/routine.grant.json", restored);
    struct json *actual = ret_document(path);
    EVIDENCE_CHECK(json_equal(json_member(actual, "proof"), ref));
    json_free(actual);
    free(path);
    const char *names[] = {"changed", "missing", "ambiguous"};
    for (size_t i = 0; i < 3; i++) {
        if (!i)
            evidence_put(leaf, "changed proof bytes\n");
        else if (i == 1)
            EVIDENCE_CHECK(!unlink(leaf));
        else {
            struct json *locations = (struct json *)json_member(ref, "locations");
            ret_push(locations, ret_string("/second-location"));
            evidence_grant(source, ref);
        }
        char *bad = evidence_path(names[i]);
        EVIDENCE_CHECK(!evidence_call("stage", source, bad));
        EVIDENCE_CHECK(evidence_errors(bad));
        path = ret_format("%s/routine-proofs/%s", bad, json_text(ref, "sha256"));
        EVIDENCE_CHECK(!evidence_file(path));
        free(path);
        free(bad);
    }
    json_free(ref);
    free(source);
    free(leaf);
    free(grant);
    free(objects);
    free(executable);
    free(out);
    free(restored);
}

static void evidence_foreign(void)
{
    char *source = evidence_path("foreign"), *proofs = ret_format("%s/routine-proofs", source);
    EVIDENCE_CHECK(!ret_dirs(proofs));
    char *leaf = evidence_path("foreign bytes");
    evidence_put(leaf, "original Windows authority snapshot\n");
    struct json *ref = evidence_ref(leaf, "D:/private/authority.snapshot");
    char *retained = ret_format("%s/%s", proofs, json_text(ref, "sha256"));
    evidence_put(retained, "original Windows authority snapshot\n");
    EVIDENCE_CHECK(!unlink(leaf));
    evidence_grant(source, ref);
    char *out = evidence_path("foreign-transport");
    EVIDENCE_CHECK(!evidence_call("stage", source, out));
    EVIDENCE_CHECK(!evidence_errors(out));
    char *path = ret_format("%s/routine.grant.json", out);
    struct json *actual = ret_document(path);
    EVIDENCE_CHECK(json_equal(json_member(actual, "proof"), ref));
    json_free(actual);
    free(path);
    const char *names[] = {"foreign-wrong", "foreign-missing", "foreign-symlink"};
    for (size_t i = 0; i < 3; i++) {
        if (!i)
            evidence_put(retained, "changed");
        else if (i == 1)
            EVIDENCE_CHECK(!unlink(retained));
#ifndef _WIN32
        else
            EVIDENCE_CHECK(!symlink("../routine.grant.json", retained));
#else
        else
            continue;
#endif
        char *bad = evidence_path(names[i]);
        EVIDENCE_CHECK(!evidence_call("stage", source, bad));
        EVIDENCE_CHECK(evidence_errors(bad));
        path = ret_format("%s/routine-proofs/%s", bad, json_text(ref, "sha256"));
        EVIDENCE_CHECK(!evidence_file(path));
        free(path);
        free(bad);
    }
    free(source);
    free(proofs);
    free(leaf);
    free(retained);
    free(out);
    json_free(ref);
}

static void evidence_pack(void)
{
    char *out = evidence_path("packed"), *known = evidence_path("known");
    EVIDENCE_CHECK(!ret_dirs(out) && !ret_dirs(known));
    char *leaf = ret_format("%s/unrelated-name", known);
    evidence_put(leaf, "retained independent proof\n");
    struct json *ref = evidence_ref(leaf, "D:/unreachable/proof.json");
    evidence_grant(out, ref);
    const char *roots[] = {known, NULL};
    EVIDENCE_CHECK(!etude_routine_pack(out, roots, NULL));
    EVIDENCE_CHECK(!etude_routine_pack(out, NULL, NULL));
    char *path = ret_format("%s/routine-proofs/%s", out, json_text(ref, "sha256"));
    EVIDENCE_CHECK(evidence_file(path));
    EVIDENCE_CHECK(!chmod(path, 0600));
    evidence_put(path, "changed retained proof");
    EVIDENCE_CHECK(etude_routine_pack(out, roots, NULL) != 0);
    free(path);
    free(leaf);
    free(known);
    free(out);
    json_free(ref);
}

static void evidence_late_proofs(void)
{
    char *out = evidence_path("late"), *inputs = ret_format("%s/routine-proofs-input", out);
    EVIDENCE_CHECK(!ret_dirs(inputs));
    char *policy = ret_format("%s/policy-before", inputs);
    evidence_put(policy, "etude-site 2\nendpoint linux\n");
    struct json *policy_ref = evidence_ref(policy, "D:/original/policy");
    evidence_grant(out, policy_ref);
    EVIDENCE_CHECK(!etude_routine_pack(out, NULL, NULL));
    char *trace = ret_format("%s/physical-load.jsonl", out);
    evidence_put(trace, "{\"kind\":\"end\",\"complete\":true}\n");
    struct json *phase_doc = ret_new(JSON_OBJECT);
    ret_set(phase_doc, "trace", evidence_ref(trace, "D:/original/physical-load.jsonl"));
    char *phase = ret_format("%s/routine.collection.json", out);
    EVIDENCE_CHECK(!ret_write(phase, phase_doc, 0));
    char *history = ret_format("%s/history", out);
    EVIDENCE_CHECK(!ret_dirs(history));
    char *stream = ret_format("%s/run.jsonl", history);
    struct json *run = ret_new(JSON_OBJECT), *data = ret_new(JSON_OBJECT);
    ret_set(data, "load", evidence_ref(phase, NULL));
    ret_set(run, "kind", ret_string("run"));
    ret_set(run, "data", data);
    EVIDENCE_CHECK(!ret_write(stream, run, 0));
    struct json *completion = ret_new(JSON_OBJECT);
    char *grant = ret_format("%s/routine.grant.json", out);
    ret_set(completion, "grant", evidence_ref(grant, NULL));
    ret_set(completion, "history", evidence_ref(stream, NULL));
    ret_set(completion, "phase", evidence_ref(phase, NULL));
    char *end = ret_format("%s/routine.completion.json", out);
    EVIDENCE_CHECK(!ret_write(end, completion, 0));
    EVIDENCE_CHECK(!etude_routine_pack(out, NULL, NULL));
    const char *paths[] = {policy, grant, trace, phase, stream};
    for (size_t i = 0; i < 5; i++) {
        struct json *ref = ret_descriptor(paths[i]);
        char *copy = ret_format("%s/routine-proofs/%s", out, json_text(ref, "sha256"));
        EVIDENCE_CHECK(evidence_file(copy));
        free(copy);
        json_free(ref);
    }
    struct json *phase_ref = ret_descriptor(phase);
    char *bad = ret_format("%s/routine-proofs/%s", out, json_text(phase_ref, "sha256"));
    EVIDENCE_CHECK(!chmod(bad, 0600));
    evidence_put(bad, "changed");
    EVIDENCE_CHECK(etude_routine_pack(out, NULL, NULL) != 0);
    free(bad);
    json_free(phase_ref);
    json_free(policy_ref);
    json_free(phase_doc);
    json_free(run);
    json_free(completion);
    free(out);
    free(inputs);
    free(policy);
    free(trace);
    free(phase);
    free(history);
    free(stream);
    free(grant);
    free(end);
}

static struct json *evidence_inventory(const char *source, const char *name)
{
    char *path = ret_format("%s/member", source);
    evidence_put(path, "actual member bytes\n");
    struct json *ref = ret_descriptor(path), *files = ret_new(JSON_ARRAY);
    ret_set(ref, "path", ret_string(name));
    ret_push(files, ref);
    struct json *inventory = ret_new(JSON_OBJECT);
    ret_set(inventory, "format", ret_string("etude-native-evidence"));
    ret_set(inventory, "schema_version", ret_integer(1));
    ret_set(inventory, "files", files);
    free(path);
    return inventory;
}
static void evidence_restore_refusals(void)
{
    char *source = evidence_path("refusals"), *out = evidence_path("refusal-output");
    EVIDENCE_CHECK(!ret_dirs(source));
    const char *names[] = {"../escape", "/absolute", "C:/foreign", "bad\\name", "a//b", "member"};
    char *index = ret_format("%s/evidence.json", source);
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        struct json *inventory = evidence_inventory(source, names[i]);
        struct json *files = (struct json *)json_member(inventory, "files");
        if (i == 5)
            ret_push(files, ret_clone(files->item[0]));
        EVIDENCE_CHECK(!ret_write(index, inventory, 1));
        EVIDENCE_CHECK(evidence_call("restore", source, out) == 2);
        EVIDENCE_CHECK(access(out, F_OK) != 0);
        json_free(inventory);
    }
    struct json *inventory = evidence_inventory(source, "member");
    EVIDENCE_CHECK(!ret_write(index, inventory, 1));
    char *member = ret_format("%s/member", source);
    evidence_put(member, "corrupt member\n");
    EVIDENCE_CHECK(evidence_call("restore", source, out) == 2);
    EVIDENCE_CHECK(access(out, F_OK) != 0);
    free(member);
    json_free(inventory);
    free(index);
    free(source);
    free(out);
}

static void evidence_reader(void)
{
    char *program = evidence_path("installed reader"), *record = evidence_path("reader.record");
    evidence_put(program, "local immutable reader bytes\n");
    struct json *ref = ret_descriptor(program);
    char *text =
        ret_format("etude-prepare 1\ntree %s\nfile %s %lld 1 2 installed reader\n", evidence_base,
                   json_text(ref, "sha256"), (long long)ret_number(ref, "size_bytes"));
    evidence_put(record, text);
    char *argv[] = {"reader", program, record, NULL};
    EVIDENCE_CHECK(!etude_prepare_reader_main(3, argv));
    FILE *file = fopen(record, "ab");
    EVIDENCE_CHECK(file != NULL);
    if (file) {
        fprintf(file, "file %s %lld 1 2 installed reader\n", json_text(ref, "sha256"),
                (long long)ret_number(ref, "size_bytes"));
        fclose(file);
    }
    EVIDENCE_CHECK(etude_prepare_reader_main(3, argv) == 2);
    evidence_put(record, text);
    evidence_put(program, "changed local reader bytes\n");
    EVIDENCE_CHECK(etude_prepare_reader_main(3, argv) == 2);
    free(text);
    json_free(ref);
    free(program);
    free(record);
}

int etude_test_routine_evidence(void)
{
    evidence_base = ret_path(fix_dir(), 1);
    if (!evidence_base)
        return 1;
    char *policy = getenv("ETUDE_RETENTION") ? strdup(getenv("ETUDE_RETENTION")) : NULL;
    unsetenv("ETUDE_RETENTION");
    evidence_transport();
    evidence_foreign();
    evidence_pack();
    evidence_late_proofs();
    evidence_restore_refusals();
    evidence_reader();
    if (policy)
        setenv("ETUDE_RETENTION", policy, 1);
    free(policy);
    free(evidence_base);
    if (!evidence_failures) {
        puts("evidence: native proof transport, packing and reader checks passed");
        fix_passed();
    }
    return evidence_failures != 0;
}
