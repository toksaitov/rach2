/* Preserve native routine input, sequence and result projection contracts. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include "../../../tools/etude/win/files.h"
#endif
#include "../../../tools/etude/retention.h"
#include "../../../tools/etude/routine_entry.h"
#include "../../../tools/etude/host_prepare.h"
#include "../../../tools/etude/authority_internal.h"
#include "../../../tools/etude/authority_run.h"
#include "../../../tools/etude/observe_sequence.h"
#define fix_dir    entry_fix_dir
#define fix_path   entry_fix_path
#define fix_passed entry_fix_passed
#define fix_format entry_fix_format
#include "fixdir_host.c"

static char *entry_base, *entry_self;
static const char *entry_mode;
static int entry_failures, entry_calls, entry_queries, entry_observed;
static struct hp_words entry_observation;
static const char *entry_export;
static int entry_allocations, entry_ensures;
static unsigned entry_seconds = 86400;
static struct hp_words entry_authorized;
static int entry_authority_status = 2;
#define ENTRY_CHECK(test)                                                     \
    do {                                                                      \
        if (!(test)) {                                                        \
            fprintf(stderr, "routine entry: line %d: %s\n", __LINE__, #test); \
            entry_failures++;                                                 \
        }                                                                     \
    } while (0)

static int mocked_candidate(const char *tree, char commit[41])
{
    (void)tree;
    memset(commit, 'a', 40);
    commit[40] = 0;
    return 0;
}
static int mocked_site(const char *path, const char *route, struct routine_site *site)
{
    (void)path;
    memset(site, 0, sizeof *site);
    strcpy(site->program, entry_self);
    strcpy(site->launcher, entry_self);
    strcpy(site->ledger, "/fixture-ledger");
    strcpy(site->locks, "/fixture-locks");
    strcpy(site->account, "fixture");
    strcpy(site->capacity_text, "8000,20000000000,50000000000");
    strcpy(site->resources, "2000,3221225472,42949672960");
    site->jobs = 2;
    site->wait = 30;
    site->seconds = entry_seconds;
    if (route && entry_mode)
        snprintf(site->mode, sizeof site->mode, "%s", entry_mode);
    return 0;
}
static int mocked_authority(const struct authority_run *run, struct etude_exec_result *result)
{
    (void)result;
    hp_words_free(&entry_authorized);
    for (size_t i = 0; run->argv[i]; i++)
        hp_add(&entry_authorized, run->argv[i]);
    return entry_authority_status;
}
static int mocked_command(struct hp_words *args, volatile sig_atomic_t *stop);
static int mocked_query(const struct routine_input *input, struct hp_words *args, const char *name,
                        volatile sig_atomic_t *stop);
static int mocked_run(struct hp_context *context, char *const argv[], const char *out,
                      const char *err);
static int mocked_observe(int argc, char **argv);
static int mocked_proxy(int argc, char **argv);
#ifndef _WIN32
static char *mocked_owner_program(const char *name);
#endif

/* Keep real input parsing and phase validation. Substitute only the
 * process boundaries used by the former script fixtures. */
#define etude_context_candidate mocked_candidate
#define authority_run_command   mocked_authority
#define hp_run                  mocked_run
#define routine_command         unused_routine_command
#define routine_query           unused_routine_query
#include "../../../tools/etude/routine_inputs.c"
#undef routine_command
#undef routine_query
#define routine_command             mocked_command
#define routine_query               mocked_query
#define routine_site_read           mocked_site
#define etude_observe_sequence_main mocked_observe
#define etude_authority_main        mocked_proxy
#define counted                     entry_sequence_counted
#include "../../../tools/etude/routine_sequence.c"
#undef counted
#define counted entry_owner_counted
#ifndef _WIN32
#define hp_program mocked_owner_program
#endif
#include "../../../tools/etude/routine_owner_entry.c"
#undef hp_program
#undef counted
#include "../../../tools/etude/routine_entry.c"
#include "../../../tools/etude/routine_prepare.c"
#undef etude_context_candidate
#undef authority_run_command
#undef hp_run
#undef routine_command
#undef routine_query
#undef routine_site_read
#undef etude_observe_sequence_main
#undef etude_authority_main

static int mocked_route_binding(const char *site, const char *route,
                                struct authority_binding *binding)
{
    (void)site;
    (void)route;
    (void)binding;
    return -1;
}
#define authority_route   mocked_route_binding
#define routine_site_read retained_site_read
#include "../../../tools/etude/routine_site.c"
#undef authority_route
#undef routine_site_read

static char *entry_path(const char *name)
{
    return ret_format("%s/%s", entry_base, name);
}
static void entry_put(const char *path, const char *text)
{
    ENTRY_CHECK(!hp_write(path, text, strlen(text)));
}
static int entry_has(char *const argv[], const char *word)
{
    for (size_t i = 0; argv[i]; i++)
        if (!strcmp(argv[i], word))
            return 1;
    return 0;
}
static int entry_equal(const char *root, const char *name, const char *expected)
{
    char *path = ret_format("%s/%s", root, name), *bytes = hp_read(path);
    int equal = bytes && !strcmp(bytes, expected);
    free(path);
    free(bytes);
    return equal;
}
static int entry_exists(const char *root, const char *name)
{
    char *path = ret_format("%s/%s", root, name);
    struct stat st;
    int exists = !lstat(path, &st);
    free(path);
    return exists;
}
static int mocked_query(const struct routine_input *input, struct hp_words *args, const char *name,
                        volatile sig_atomic_t *stop)
{
    (void)stop;
    entry_queries++;
    if (entry_mode && !strcmp(entry_mode, "stage")) {
        char *path = ret_format("%s/%s.stdout", input->out, name);
        entry_put(path, "fixture\n");
        free(path);
        return strcmp(name, "selection") ? 2 : 0;
    }
    if (!strcmp(name, "suites")) {
        ENTRY_CHECK(entry_has(args->v, "inventory"));
        char *path = ret_format("%s/suites.stdout", input->out);
        entry_put(path, "# The suites\n\nOne line per suite, in the order the pool starts them.\n\n"
                        "* `style` (full tier; needs clang, python3; no machine): the checks.\n"
                        "* `host` (full tier; needs qemu; 384 MiB): the host tests.\n"
                        "* `default` (core tier; needs qemu; 128 MiB): every probe (each alone).\n"
                        "* `shutdown` (core tier; needs qemu; 128 MiB): the end; i686 only.\n"
                        "* `fpu` (core tier; needs qemu; 128 MiB; x86_64 only): the FPU.\n"
                        "* `bigmem` (full tier; needs qemu; 8320 MiB; x86_64 only): memory.\n");
        free(path);
        return 0;
    }
    if (strstr(name, "native-selection")) {
        ENTRY_CHECK(args->n == 3 && (!strcmp(args->v[1], "-w") || !strcmp(args->v[1], "-u")));
        char *path = ret_format("%s/%s.stdout", input->out, name);
        char *bytes = ret_format("%s\n", args->v[2]);
        entry_put(path, bytes);
        free(path);
        free(bytes);
        return 0;
    }
    return 2;
}
static int entry_completion_mock(const char *out)
{
    char *path = ret_format("%s/routine-completion.path", out);
    char *completion_path = ret_format("%s/routine.completion.json", out);
    char *value = ret_format("%s\n", completion_path);
    entry_put(path, !strcmp(entry_mode, "completion-relative")   ? "relative\n"
                    : !strcmp(entry_mode, "completion-multiple") ? "/one\n/two\n"
                                                                 : value);
    entry_put(completion_path, "fixture\n");
    char *proofs = ret_format("%s/routine-proofs", out);
    int result = mkdir(proofs, 0700);
    free(proofs);
    free(path);
    free(completion_path);
    free(value);
    return result;
}
/* Answer the allocation mode's commands: the export, then the ensure. */
static int mocked_allocation(struct hp_words *args)
{
    ENTRY_CHECK(getenv("ETUDE_PREPARATION_CLEAN") &&
                !strcmp(getenv("ETUDE_PREPARATION_CLEAN"), "1"));
    ENTRY_CHECK(!strcmp(args->v[args->n - 1], "all"));
    if (entry_has(args->v, "-J")) {
        entry_allocations++;
        for (size_t i = 0; i + 1 < args->n; i++)
            if (!strcmp(args->v[i], "-J"))
                return routine_copy(entry_export, args->v[i + 1]);
    }
    if (entry_has(args->v, "ensure")) {
        ENTRY_CHECK(entry_allocations == 1);
        entry_ensures++;
        return 37;
    }
    return 91;
}
static int mocked_command(struct hp_words *args, volatile sig_atomic_t *stop)
{
    (void)stop;
    if (!strcmp(entry_mode, "allocation"))
        return mocked_allocation(args);
    if (!strcmp(entry_mode, "stage")) {
        entry_calls++;
        return 0;
    }
    if (entry_has(args->v, "retention"))
        return 0;
    if (entry_has(args->v, "pack"))
        return !strcmp(entry_mode, "completion-pack-failed") ? 2 : 0;
    if (!entry_has(args->v, "routine-sequence"))
        return 2;
    entry_calls++;
    ENTRY_CHECK(entry_calls == 1 && args->n >= 12);
    ENTRY_CHECK(!strcmp(args->v[3], !strcmp(entry_mode, "push") ? "push" : "ci"));
    if (strcmp(entry_mode, "push")) {
        ENTRY_CHECK(args->n == 16 && !strcmp(args->v[11], "style") &&
                    !strcmp(args->v[12], "host") && !strcmp(args->v[13], "default") &&
                    !strcmp(args->v[14], "shutdown") && !strcmp(args->v[15], "fpu"));
    } else {
        ENTRY_CHECK(entry_has(args->v, "i686:host") && entry_has(args->v, "x86_64:default"));
        ENTRY_CHECK(!strcmp(getenv("RACH2_CASES"), "host.fixture.host,machine.fixture.default"));
    }
    const char *out = args->v[5];
    char *path = ret_format("%s/selection.list", out);
    entry_put(path,
              "style none host.fixture.style\nhost none host.fixture.host\n"
              "default i686 machine.fixture.default\ndefault x86_64 machine.fixture.default\n");
    free(path);
    path = ret_format("%s/log", out);
    entry_put(path, "complete native sequence output\n");
    free(path);
    if (!strcmp(entry_mode, "cancel"))
        return 130;
    if (strcmp(entry_mode, "missing-history")) {
        path = ret_format("%s/history", out);
        ENTRY_CHECK(!mkdir(path, 0700));
        free(path);
        path = ret_format("%s/history/run.jsonl", out);
        entry_put(path, "fixture\n");
        free(path);
        path = ret_format("%s/store", out);
        ENTRY_CHECK(!mkdir(path, 0700));
        free(path);
    }
    if (!strncmp(entry_mode, "completion", 10))
        return entry_completion_mock(out);
    return !strcmp(entry_mode, "pass") || !strcmp(entry_mode, "parent-crlf") ||
                   !strcmp(entry_mode, "push") || strstr(entry_mode, "-import") ||
                   !strcmp(entry_mode, "wsl-native")
               ? 0
               : 1;
}
static int mocked_run(struct hp_context *context, char *const argv[], const char *out,
                      const char *err)
{
    (void)context;
    (void)err;
    ENTRY_CHECK(entry_has(argv, "--project-architecture") && entry_has(argv, "--require-budget") &&
                entry_has(argv, "-b") && entry_has(argv, "-c"));
    if (!strcmp(entry_mode, "push"))
        ENTRY_CHECK(entry_has(argv, "--selection"));
    if (!strncmp(entry_mode, "completion", 10))
        ENTRY_CHECK(entry_has(argv, "--routine-completion") && entry_has(argv, "--routine-proofs"));
    int bad = !strcmp(entry_mode, "budget") || !strcmp(entry_mode, "cleanup") ||
              !strcmp(entry_mode, "missing-history");
    int foreign = !strcmp(entry_mode, "foreign") && entry_has(argv, "x86_64");
    entry_put(out, bad       ? "etude check: result: failed: runner\n"
                   : foreign ? "etude check: result: failed: default\n"
                             : "etude check: result: passed\n");
    return bad || foreign;
}
static int mocked_observe(int argc, char **argv)
{
    entry_observed++;
    hp_words_free(&entry_observation);
    for (int i = 0; i < argc; i++)
        hp_add(&entry_observation, argv[i]);
    return 0;
}
#ifndef _WIN32
static char *mocked_owner_program(const char *name)
{
    return !strcmp(name, "wslpath") ? strdup(entry_self) : hp_program(name);
}
#endif
static int mocked_proxy(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 37;
}

static char *entry_allocation(const char *leaf)
{
    static const char selection[] =
        "{\"schema_version\":1,\"architecture\":\"both\","
        "\"suites\":[{\"suite\":\"fixture\",\"architecture\":\"x86_64\",\"serial\":false,"
        "\"stops\":false,\"cases\":[{\"case_id\":\"fixture.case\",\"architecture\":\"x86_64\","
        "\"repetitions\":1}]}],\"stop_on_fail\":false}\n";
    struct json *doc = hp_value(JSON_OBJECT), *items = hp_value(JSON_ARRAY);
    struct json *item = hp_value(JSON_OBJECT);
    char hex[65];
    struct sha256 digest;
    unsigned char sum[32];
    sha256_init(&digest);
    sha256_update(&digest, (const unsigned char *)selection, strlen(selection));
    sha256_final(&digest, sum);
    sha256_hex(sum, hex);
    hp_member(doc, "schema_version", hp_number(1));
    hp_member(doc, "selection_bytes", hp_string(selection));
    hp_member(doc, "selection_size_bytes", hp_number((long long)strlen(selection)));
    hp_member(doc, "selection_sha256", hp_string(hex));
    hp_member(item, "case_id", hp_string("fixture.case"));
    hp_member(item, "architecture", hp_string("x86_64"));
    hp_member(item, "repetition", hp_number(0));
    hp_item(items, item);
    hp_member(doc, "items", items);
    char *path = entry_path(leaf);
    ENTRY_CHECK(!hp_publish(path, doc));
    json_free(doc);
    return path;
}
static int entry_clean(const struct json *value)
{
    const struct json *clean = json_member(value, "clean");
    return clean && clean->type == JSON_BOOLEAN ? (int)clean->integer : -1;
}
static void entry_mode_rows(void)
{
    char *path = entry_path("mode.json");
    entry_put(path, "{\"format\":\"etude-routine-route\",\"schema_version\":1,"
                    "\"mode\":\"macos-import\"}\n");
    ENTRY_CHECK(!chmod(path, 0600));
    char mode[24];
#ifdef __APPLE__
    ENTRY_CHECK(!routine_route_mode(path, mode) && !strcmp(mode, "macos-import"));
#else
    ENTRY_CHECK(routine_route_mode(path, mode));
#endif
    ENTRY_CHECK(!chmod(path, 0666) && routine_route_mode(path, mode));
    ENTRY_CHECK(!chmod(path, 0600));
    entry_put(path, "{\"format\":\"etude-routine-route\",\"schema_version\":1,"
                    "\"mode\":\"unknown\"}\n");
    ENTRY_CHECK(routine_route_mode(path, mode));
    free(path);
}

static void entry_phase_rows(void)
{
    char *path = entry_allocation("allocated.json");
    struct json *phase = NULL;
    unsetenv("ETUDE_PREPARATION_CLEAN");
    setenv("ETUDE_PHASE_SELECTION_EXPORT", path, 1);
    ENTRY_CHECK(!routine_phase_inputs("phase_first_run", &phase) && entry_clean(phase) == 0);
    const struct json *ref = json_member(phase, "selection_export");
    char hash[65];
    ENTRY_CHECK(!hp_hash(path, hash) && !strcmp(json_text(ref, "sha256"), hash));
    json_free(phase);
    phase = NULL;
    setenv("ETUDE_PREPARATION_CLEAN", "1", 1);
    ENTRY_CHECK(!routine_phase_inputs("phase_first_run", &phase) && entry_clean(phase) == 1);
    json_free(phase);
    phase = NULL;
    const char *purposes[] = {"push", "ci", "release"};
    for (size_t i = 0; i < 3; i++)
        ENTRY_CHECK(routine_phase_inputs(purposes[i], &phase));
    setenv("ETUDE_PREPARATION_CLEAN", "0", 1);
    ENTRY_CHECK(routine_phase_inputs("phase_first_run", &phase));
    unsetenv("ETUDE_PHASE_SELECTION_EXPORT");
    unsetenv("ETUDE_PREPARATION_CLEAN");
    ENTRY_CHECK(routine_phase_inputs("phase_first_run", &phase));
    ENTRY_CHECK(!routine_phase_inputs("ci", &phase) && entry_clean(phase) == 0 &&
                json_member(phase, "selection_export")->type == JSON_NULL);
    json_free(phase);
    phase = NULL;
    ENTRY_CHECK(!routine_phase_inputs("release", &phase) && entry_clean(phase) == 1);
    json_free(phase);
    phase = NULL;
    setenv("ETUDE_PHASE_SELECTION_EXPORT", "", 1);
    ENTRY_CHECK(routine_phase_inputs("ci", &phase));
    struct json *doc = hp_document(path);
    hp_member(doc, "selection_sha256", hp_string("wrong"));
    ENTRY_CHECK(!hp_publish(path, doc));
    setenv("ETUDE_PHASE_SELECTION_EXPORT", path, 1);
    ENTRY_CHECK(routine_phase_inputs("phase_first_run", &phase));
    json_free(doc);
    unsetenv("ETUDE_PHASE_SELECTION_EXPORT");
    free(path);
}
static void entry_selection_rows(void)
{
    char *a = entry_allocation("expected.json"), *b = entry_path("actual.json");
    ENTRY_CHECK(!routine_copy(a, b) && !routine_selection(a, b));
    struct json *doc = hp_document(b);
    struct json *item = json_member(doc, "items")->item[0];
    hp_member(item, "repetition", hp_number(1));
    ENTRY_CHECK(!hp_publish(b, doc) && routine_selection(a, b));
    json_free(doc);
    free(a);
    free(b);
}
static char *entry_output(const char *mode)
{
    char *out = entry_path(mode);
    ENTRY_CHECK(!mkdir(out, 0700));
    return out;
}
static void entry_projection_rows(void)
{
    const char *modes[] = {"pass",
                           "parent-crlf",
                           "foreign",
                           "budget",
                           "cleanup",
                           "missing-history",
                           "cancel",
                           "completion",
                           "completion-relative",
                           "completion-multiple",
                           "completion-pack-failed",
                           "macos-import",
                           "wsl-native",
                           "windows-import",
                           "push"};
    for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
        entry_mode = modes[i];
        entry_calls = entry_queries = 0;
        char *out = entry_output(entry_mode);
        char *argv[] = {entry_self,
                        !strcmp(entry_mode, "push") ? "push" : "core",
                        out,
                        "i686:host",
                        "x86_64:default",
                        NULL};
        int push = !strcmp(entry_mode, "push");
        if (push)
            setenv("RACH2_CASES", "host.fixture.host,machine.fixture.default", 1);
        int routed = strstr(entry_mode, "-import") || !strcmp(entry_mode, "wsl-native");
        if (routed)
            setenv("ETUDE_ROUTINE_ROUTE", "/fixture-route", 1);
        int passed = !strcmp(entry_mode, "pass") || !strcmp(entry_mode, "parent-crlf") ||
                     !strcmp(entry_mode, "completion") || !strcmp(entry_mode, "macos-import") ||
                     push;
        int cancelled = !strcmp(entry_mode, "cancel");
        int status = etude_routine_entry_main(push ? 5 : 3, argv);
        ENTRY_CHECK(status == (passed ? 0 : cancelled ? 125 : 1));
        ENTRY_CHECK(entry_calls == 1 && entry_queries == 1);
        for (unsigned j = 0; j < 2; j++) {
            char *name = ret_format("%s-%s/done", push ? "push" : "core", j ? "x86_64" : "i686");
            ENTRY_CHECK(entry_exists(out, name) == !cancelled);
            free(name);
            if (cancelled)
                continue;
            name = ret_format("%s-%s/failed", push ? "push" : "core", j ? "x86_64" : "i686");
            const char *failure = passed                           ? ""
                                  : !strcmp(entry_mode, "foreign") ? (j ? "default\n" : "")
                                                                   : "runner\n";
            ENTRY_CHECK(entry_equal(out, name, failure));
            free(name);
        }
        unsetenv("RACH2_CASES");
        unsetenv("ETUDE_ROUTINE_ROUTE");
        free(out);
    }
}
static void entry_counted_rows(void)
{
    char *allocated = entry_allocation("sequence-allocation.json");
    char *tree = entry_path("tree"), *site = entry_path("site"), *out = entry_output("sequence");
    const char *purposes[] = {"phase_first_run", "phase_first_run", "release", "ci"};
    const char *clean[] = {NULL, "1", NULL, "1"};
    for (unsigned i = 0; i < 4; i++) {
        if (clean[i])
            setenv("ETUDE_PREPARATION_CLEAN", clean[i], 1);
        else
            unsetenv("ETUDE_PREPARATION_CLEAN");
        if (i < 2)
            setenv("ETUDE_PHASE_SELECTION_EXPORT", allocated, 1);
        else
            unsetenv("ETUDE_PHASE_SELECTION_EXPORT");
        unsetenv("ETUDE_ADMITTED");
        char *argv[] = {"routine-sequence",
                        (char *)purposes[i],
                        tree,
                        out,
                        "both",
                        "sample",
                        site,
                        entry_self,
                        "tcg",
                        "all",
                        NULL};
        entry_seconds = i == 1 ? 1234 : 86400;
        entry_observed = 0;
        ENTRY_CHECK(!etude_routine_sequence_main(10, argv) && entry_observed == 1);
        const char *actual = getenv("ETUDE_PREPARATION_CLEAN");
        ENTRY_CHECK(i == 1 || i == 2 ? actual && !strcmp(actual, "1") : !actual);
        ENTRY_CHECK(entry_has(entry_observation.v, "sample:prepare"));
        ENTRY_CHECK(entry_has(entry_observation.v, "prepare") &&
                    entry_has(entry_observation.v, "routine") &&
                    entry_has(entry_observation.v, purposes[i]) &&
                    entry_has(entry_observation.v, "all"));
        if (i == 1)
            ENTRY_CHECK(!strcmp(entry_observation.v[25], "1234"));
        int at = 9;
        for (unsigned phase = 0; phase < 2; phase++) {
            ENTRY_CHECK(at + 2 < (int)entry_observation.n);
            char **v = entry_observation.v;
            const char *program = v[at++];
            char *end;
            unsigned long count = strtoul(v[at++], &end, 10);
            ENTRY_CHECK(!*end && count >= 2 && count <= entry_observation.n - (size_t)at);
            ENTRY_CHECK(!strcmp(program, v[at]) && !strcmp(v[at + 1], phase ? "run" : "admit"));
            at += (int)count;
        }
        ENTRY_CHECK(at == (int)entry_observation.n && entry_has(entry_observation.v, "-I") &&
                    entry_has(entry_observation.v, "-g"));
    }
    entry_seconds = 86400;
    setenv("ETUDE_PHASE_SELECTION_EXPORT", "", 1);
    char *argv[] = {"routine-sequence", "ci",  tree,  out, "both", "sample", site,
                    entry_self,         "tcg", "all", NULL};
    entry_observed = 0;
    ENTRY_CHECK(etude_routine_sequence_main(10, argv) == 2 && !entry_observed);
    setenv("ETUDE_PHASE_SELECTION_EXPORT", allocated, 1);
    setenv("ETUDE_PREPARATION_CLEAN", "0", 1);
    argv[1] = "phase_first_run";
    ENTRY_CHECK(etude_routine_sequence_main(10, argv) == 2 && !entry_observed);
    unsetenv("ETUDE_PHASE_SELECTION_EXPORT");
    unsetenv("ETUDE_PREPARATION_CLEAN");
    hp_words_free(&entry_observation);
    free(allocated);
    free(tree);
    free(site);
    free(out);
}
static void entry_refusal_rows(void)
{
    char *tree = entry_path("tree"), *site = entry_path("site"), *out = entry_path("missing");
    char *log = entry_path("refusal.stderr");
    char *argv[] = {"routine-sequence", "ci",  tree,  out, "both", "sample", site,
                    entry_self,         "tcg", "all", NULL};
    FILE *file = fopen(log, "wb");
    int saved = dup(2);
    ENTRY_CHECK(file && saved >= 0);
    fflush(stderr);
    if (file && saved >= 0 && dup2(fileno(file), 2) >= 0) {
        entry_observed = 0;
        ENTRY_CHECK(etude_routine_sequence_main(10, argv) == 2 && !entry_observed);
        fflush(stderr);
        dup2(saved, 2);
    }
    if (saved >= 0)
        close(saved);
    if (file)
        fclose(file);
    char *bytes = hp_read(log);
    ENTRY_CHECK(bytes && !strncmp(bytes, "etude routine: OUT: ", 20) && strchr(bytes, '\n') &&
                !strchr(bytes, '\n')[1]);
    free(bytes);
    free(log);
    free(out);
    free(site);
    free(tree);
}
static void entry_preallocation_rows(void)
{
    char *expected = entry_allocation("preallocated.json");
    char *actual = entry_allocation("allocator-current.json");
    char *tree = entry_path("tree"), *site = entry_path("site"), *bins = entry_path("bin");
    ENTRY_CHECK(!mkdir(bins, 0700));
    const char *tools[] = {"qemu-system-i386", "qemu-system-x86_64"};
    for (unsigned i = 0; i < 2; i++) {
        char *path = ret_format("%s/%s", bins, tools[i]);
        ENTRY_CHECK(!routine_copy(entry_self, path) && !chmod(path, 0700));
        free(path);
    }
    char *old_path = getenv("PATH") ? strdup(getenv("PATH")) : NULL;
    setenv("PATH", bins, 1);
    setenv("CC", entry_self, 1);
    setenv("ETUDE_ADMITTED", site, 1);
    setenv("ETUDE_PHASE_SELECTION_EXPORT", expected, 1);
    setenv("ETUDE_ROUTINE_ENSURE", "/fixed/route", 1);
    unsetenv("ETUDE_PREPARATION_CLEAN");
    unsetenv("ETUDE_PHYSICAL_TRACE");
    unsetenv("ETUDE_PHYSICAL_STOP_REQUEST");
    setenv("ETUDE_PREPARATION_CLEAN", "1", 1);
    entry_export = actual;
    entry_mode = "allocation";
    for (unsigned changed = 0; changed < 2; changed++) {
        char *out = entry_output(changed ? "allocation-refused" : "allocation-before-producer");
        char *trace = ret_format("%s/outer-load.jsonl", out);
        entry_put(trace, "{}\n");
        if (changed) {
            struct json *doc = hp_document(actual);
            hp_member(json_member(doc, "items")->item[0], "case_id", hp_string("fixture.other"));
            ENTRY_CHECK(!hp_publish(actual, doc));
            json_free(doc);
        }
        char *argv[] = {"routine", "phase_first_run", tree,  out,   "both", "sample",
                        site,      entry_self,        "tcg", "all", NULL};
        entry_allocations = entry_ensures = 0;
        int result = etude_prepare_routine_main(10, argv, NULL);
        ENTRY_CHECK(result == (changed ? 2 : 37));
        ENTRY_CHECK(entry_allocations == 1 && entry_ensures == !changed);
        ENTRY_CHECK(!entry_exists(out, "prepare.record"));
        free(trace);
        free(out);
    }
    if (old_path)
        setenv("PATH", old_path, 1);
    else
        unsetenv("PATH");
    free(old_path);
    unsetenv("CC");
    unsetenv("ETUDE_ADMITTED");
    unsetenv("ETUDE_PHASE_SELECTION_EXPORT");
    unsetenv("ETUDE_ROUTINE_ENSURE");
    free(expected);
    free(actual);
    free(tree);
    free(site);
    free(bins);
}

/*
 * A sequence in two runs: `run routine-prepare` admits the build alone under
 * RUN:prepare, unobserved, and gives its status, a cancellation's among them;
 * `run routine-collect` observes a preparation that composes the context
 * under RUN:context, with no grant, and then the collection. Neither takes a
 * route.
 */
static void entry_split_rows(void)
{
    char *tree = entry_path("tree"), *site = entry_path("site"), *out = entry_output("split");
    char *argv[] = {"routine-prepare", "ci",  tree,  out, "both", "sample", site,
                    entry_self,        "tcg", "all", NULL};
    unsetenv("ETUDE_ADMITTED");
    unsetenv("ETUDE_PHASE_SELECTION_EXPORT");
    unsetenv("ETUDE_PREPARATION_CLEAN");
    unsetenv("ETUDE_ROUTINE_ROUTE");
    const int statuses[] = {0, 1, 130};
    for (unsigned i = 0; i < 3; i++) {
        entry_authority_status = statuses[i];
        entry_observed = 0;
        ENTRY_CHECK(etude_routine_prepare_main(10, argv) == statuses[i] && !entry_observed);
        ENTRY_CHECK(entry_authorized.n > 2 && !strcmp(entry_authorized.v[0], entry_self) &&
                    !strcmp(entry_authorized.v[1], "admit") &&
                    entry_has(entry_authorized.v, "sample:prepare") &&
                    entry_has(entry_authorized.v, "routine-build") &&
                    !entry_has(entry_authorized.v, "routine") &&
                    entry_has(entry_authorized.v, "all") && getenv("ETUDE_PREPARATION_GRANT"));
        hp_words_free(&entry_authorized);
    }
    entry_authority_status = 2;
    argv[0] = "routine-collect";
    entry_observed = 0;
    ENTRY_CHECK(!etude_routine_collect_main(10, argv) && entry_observed == 1 &&
                !entry_authorized.n && !getenv("ETUDE_PREPARATION_GRANT"));
    char *result = ret_format("%s/context.admit.result", out);
    ENTRY_CHECK(entry_has(entry_observation.v, "sample:context") &&
                entry_has(entry_observation.v, "routine-context") &&
                entry_has(entry_observation.v, result) &&
                !entry_has(entry_observation.v, "routine-build") &&
                entry_has(entry_observation.v, "run") && entry_has(entry_observation.v, "-I"));
    free(result);
    setenv("ETUDE_ROUTINE_ROUTE", "/fixture-route", 1);
    entry_observed = 0;
    ENTRY_CHECK(etude_routine_collect_main(10, argv) == 2);
    argv[0] = "routine-prepare";
    ENTRY_CHECK(etude_routine_prepare_main(10, argv) == 2 && !entry_observed &&
                !entry_authorized.n);
    unsetenv("ETUDE_ROUTINE_ROUTE");
    hp_words_free(&entry_observation);
    free(tree);
    free(site);
    free(out);
}

/*
 * The stages of `prepare routine`: the build stage refuses a trace or an
 * import and stops before the source; the context stage needs the build's
 * outputs, the trace and a receipt that names its conditioning, and goes
 * straight to the source.
 */
static void entry_stage_rows(void)
{
    char *tree = entry_path("tree"), *site = entry_path("site"), *bins = entry_path("stage-bin");
    ENTRY_CHECK(!mkdir(bins, 0700));
    const char *tools[] = {"qemu-system-i386", "qemu-system-x86_64"};
    for (unsigned i = 0; i < 2; i++) {
        char *path = ret_format("%s/%s", bins, tools[i]);
        ENTRY_CHECK(!routine_copy(entry_self, path) && !chmod(path, 0700));
        free(path);
    }
    char *old_path = getenv("PATH") ? strdup(getenv("PATH")) : NULL;
    setenv("PATH", bins, 1);
    setenv("CC", entry_self, 1);
    setenv("ETUDE_ADMITTED", site, 1);
    unsetenv("ETUDE_PHASE_SELECTION_EXPORT");
    unsetenv("ETUDE_PREPARATION_CLEAN");
    unsetenv("ETUDE_ROUTINE_ENSURE");
    unsetenv("ETUDE_PREPARATION_IMPORT");
    unsetenv("ETUDE_PHYSICAL_TRACE");
    unsetenv("ETUDE_PHYSICAL_STOP_REQUEST");
    setenv("ETUDE_REFERENCE", "/fixture-reference", 1);
    entry_mode = "stage";
    char *out = entry_output("stage");
    char *argv[] = {"routine-build", "ci",  tree,  out, "both", "sample", site,
                    entry_self,      "tcg", "all", NULL};
    char *trace = ret_format("%s/outer-load.jsonl", out);
    entry_put(trace, "{}\n");
    entry_calls = entry_queries = 0;
    ENTRY_CHECK(etude_prepare_routine_build_main(10, argv, NULL) == 2 && !entry_calls &&
                !entry_queries);
    ENTRY_CHECK(!unlink(trace));
    setenv("ETUDE_ROUTINE_ENSURE", "/fixed/route", 1);
    ENTRY_CHECK(etude_prepare_routine_build_main(10, argv, NULL) == 2 && !entry_calls &&
                !entry_queries);
    unsetenv("ETUDE_ROUTINE_ENSURE");
    ENTRY_CHECK(!etude_prepare_routine_build_main(10, argv, NULL) && entry_calls == 3 &&
                entry_queries == 1 && entry_exists(out, "selection.list") &&
                !entry_exists(out, "source.stdout") && !entry_exists(out, "context") &&
                !getenv("ETUDE_PREPARATION_RECEIPT"));
    argv[0] = "routine-context";
    entry_calls = entry_queries = 0;
    ENTRY_CHECK(etude_prepare_routine_context_main(10, argv, NULL) == 2 && !entry_queries);
    const char *built[] = {"prepare.record", "selection.export.json", "preparation.receipt.json"};
    for (unsigned i = 0; i < 3; i++) {
        char *path = ret_format("%s/%s", out, built[i]);
        entry_put(path, i < 2 ? "fixture\n" : "{\"conditioning\":{}}\n");
        free(path);
    }
    ENTRY_CHECK(etude_prepare_routine_context_main(10, argv, NULL) == 2 && !entry_queries);
    entry_put(trace, "{}\n");
    ENTRY_CHECK(etude_prepare_routine_context_main(10, argv, NULL) == 2 && !entry_queries);
    char *receipt = ret_format("%s/preparation.receipt.json", out);
    entry_put(receipt, "{\"conditioning\":{\"inhouse\":\"retained\"}}\n");
    ENTRY_CHECK(etude_prepare_routine_context_main(10, argv, NULL) == 2 && entry_queries == 1 &&
                !entry_calls && entry_exists(out, "source.stdout"));
    if (old_path)
        setenv("PATH", old_path, 1);
    else
        unsetenv("PATH");
    free(old_path);
    unsetenv("CC");
    unsetenv("ETUDE_ADMITTED");
    unsetenv("ETUDE_REFERENCE");
    free(receipt);
    free(trace);
    free(out);
    free(tree);
    free(site);
    free(bins);
}

static void entry_request_rows(void)
{
    struct owner_entry owner = {0};
    char *out = entry_output("requests"), *allocated = entry_allocation("request-allocation.json");
    strcpy(owner.in.out, out);
    strcpy(owner.in.tree, "/tree");
    strcpy(owner.in.site, "/site");
    strcpy(owner.in.program, "/etude");
    memset(owner.in.commit, 'a', 40);
    owner.in.commit[40] = 0;
    owner.in.purpose = "phase_first_run";
    owner.in.arches = "both";
    owner.in.run = "sample-01";
    owner.in.accel = "kvm";
    char *selection[] = {"all", NULL};
    owner.in.selection = selection;
    owner.in.count = 1;
    setenv("ETUDE_PHASE_SELECTION_EXPORT", allocated, 1);
    setenv("ETUDE_PREPARATION_CLEAN", "1", 1);
    ENTRY_CHECK(!routine_phase_inputs(owner.in.purpose, &owner.phase));
    owner.physical = hp_value(JSON_OBJECT);
    owner.producer = hp_value(JSON_OBJECT);
    hp_member((struct json *)owner.physical, "wsl", hp_string("C:/Windows/System32/wsl.exe"));
    hp_member((struct json *)owner.producer, "distribution", hp_string("Ubuntu"));
    hp_member((struct json *)owner.producer, "user", hp_string("testing"));
    hp_member((struct json *)owner.producer, "environment", hp_value(JSON_OBJECT));
#ifndef _WIN32
    ENTRY_CHECK(!request_write(&owner));
    char *written = ret_format("%s/routine.request.json", out);
    struct json *actual_request = hp_document(written);
    ENTRY_CHECK(actual_request &&
                !strcmp(json_text(actual_request, "selection_export_linux"), allocated));
    const struct json *exported =
        json_member(json_member(actual_request, "preparation"), "selection_export");
    char digest[65];
    ENTRY_CHECK(!hp_hash(allocated, digest) && !strcmp(json_text(exported, "sha256"), digest));
    json_free(actual_request);
    free(written);
#else
    ENTRY_CHECK(request_write(&owner));
#endif
    struct json *request = request_json(&owner.in, owner.phase);
    hp_member(request, "selection_export_linux", hp_string(allocated));
    char *path = ret_format("%s/routine.request.json", out);
    ENTRY_CHECK(!hp_publish(path, request) && !request_read(&owner));
    json_free(owner.request);
    owner.request = NULL;
    struct hp_words command = {0};
    ENTRY_CHECK(!direct_command(&owner, &command));
    char *export = ret_format("ETUDE_PHASE_SELECTION_EXPORT=%s", allocated);
    ENTRY_CHECK(entry_has(command.v, export) && entry_has(command.v, "ETUDE_PREPARATION_CLEAN=1"));
    hp_words_free(&command);
    json_free(owner.request);
    owner.request = NULL;
    hp_member(request, "selection", hp_value(JSON_ARRAY));
    ENTRY_CHECK(!hp_publish(path, request) && request_read(&owner));
    json_free(owner.request);
    owner.request = NULL;
    hp_member(request, "selection", hp_array(&(struct hp_words){.v = selection, .n = 1}));
    hp_member(request, "tree", hp_value(JSON_NULL));
    ENTRY_CHECK(!hp_publish(path, request) && request_read(&owner));
    setenv("WSLENV", "ETUDE_PHASE_SELECTION_EXPORT/p:ETUDE_PREPARATION_CLEAN/u:KEEP", 1);
    const char *names[] = {"ETUDE_PHASE_SELECTION_EXPORT", "ETUDE_PREPARATION_CLEAN", NULL};
    ENTRY_CHECK(
        !wsl_environment(names) &&
        !strcmp(getenv("WSLENV"), "KEEP:ETUDE_PHASE_SELECTION_EXPORT:ETUDE_PREPARATION_CLEAN"));
    json_free(owner.request);
    json_free(request);
    json_free(owner.phase);
    json_free((struct json *)owner.physical);
    json_free((struct json *)owner.producer);
    unsetenv("ETUDE_PHASE_SELECTION_EXPORT");
    unsetenv("ETUDE_PREPARATION_CLEAN");
    free(export);
    free(path);
    free(out);
    free(allocated);
}

int etude_test_routine_entry(const char *self)
{
    static const char *const keys[] = {"ETUDE_SITE",
                                       "ETUDE_RETENTION",
                                       "ARCH",
                                       "RACH2_QEMU_ACCEL",
                                       "ETUDE_ROUTINE_ROUTE",
                                       "ETUDE_PHASE_SELECTION_EXPORT",
                                       "ETUDE_PREPARATION_CLEAN",
                                       "ETUDE_PURPOSE",
                                       "SUITES",
                                       "RACH2_CASES",
                                       "ETUDE_ADMITTED",
                                       "WSLENV",
                                       "ETUDE_BUILD_JOBS",
                                       "JOBS",
                                       "RACH2_MIN_FREE_GIB",
                                       "ETUDE_PREPARATION_GRANT",
                                       "MAKEFLAGS",
                                       "MFLAGS",
                                       "ETUDE_SEQUENCE_LOG",
                                       "ETUDE_RUNTIME_PATH",
                                       "ETUDE_PREPARATION_RUN_ID",
                                       "ETUDE_PREPARATION_RECEIPT",
                                       "ETUDE_PREPARATION_COMMIT",
                                       "ETUDE_PHYSICAL_TRACE",
                                       "ETUDE_PHYSICAL_STOP_REQUEST",
                                       "ETUDE_ROUTINE_ENSURE",
                                       "ETUDE_PREPARATION_IMPORT",
                                       "ETUDE_REFERENCE",
                                       "CC"};
    char *saved[sizeof keys / sizeof *keys];
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        saved[i] = getenv(keys[i]) ? strdup(getenv(keys[i])) : NULL;
    entry_base = ret_path(entry_fix_dir(), 1);
    entry_self = hp_program(self);
    if (!entry_base || !entry_self)
        return 1;
    char *tree = entry_path("tree"), *site = entry_path("site");
    ENTRY_CHECK(!mkdir(tree, 0700));
    entry_put(site, "private fixture policy\n");
    char *cwd = getcwd(NULL, 0);
    ENTRY_CHECK(cwd && !chdir(tree));
    setenv("ETUDE_SITE", site, 1);
    setenv("ETUDE_RETENTION", "/fixture-retention", 1);
    setenv("ARCH", "i686 x86_64", 1);
    setenv("RACH2_QEMU_ACCEL", "tcg", 1);
    unsetenv("ETUDE_ROUTINE_ROUTE");
    unsetenv("ETUDE_PHASE_SELECTION_EXPORT");
    unsetenv("ETUDE_PREPARATION_CLEAN");
    unsetenv("ETUDE_PURPOSE");
    unsetenv("SUITES");
    unsetenv("RACH2_CASES");
    entry_projection_rows();
    entry_mode_rows();
    entry_phase_rows();
    entry_selection_rows();
    entry_counted_rows();
    entry_refusal_rows();
    entry_request_rows();
    entry_preallocation_rows();
    entry_split_rows();
    entry_stage_rows();
    ENTRY_CHECK(!chdir(cwd));
    free(cwd);
    free(tree);
    free(site);
    free(entry_self);
    free(entry_base);
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
        if (saved[i])
            setenv(keys[i], saved[i], 1);
        else
            unsetenv(keys[i]);
        free(saved[i]);
    }
    hp_words_free(&entry_authorized);
    if (!entry_failures) {
        entry_fix_passed();
        puts("routine entry: native projection, phase, selection and sequence checks passed");
    }
    return entry_failures != 0;
}
