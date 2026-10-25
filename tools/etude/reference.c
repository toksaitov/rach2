/* Forecast against immutable selected-site references through native Etude. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#endif
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "retention.h"

struct reference_options {
    const char *tree, *etude, *reference, *output, *site, *preparation, *accelerator;
};

static int reference_options(int argc, char **argv, struct reference_options *o)
{
    memset(o, 0, sizeof(*o));
    for (int i = 1; i < argc; i += 2) {
        const char **slot = !strcmp(argv[i], "--tree")          ? &o->tree
                            : !strcmp(argv[i], "--etude")       ? &o->etude
                            : !strcmp(argv[i], "--reference")   ? &o->reference
                            : !strcmp(argv[i], "--output")      ? &o->output
                            : !strcmp(argv[i], "--site")        ? &o->site
                            : !strcmp(argv[i], "--preparation") ? &o->preparation
                            : !strcmp(argv[i], "--accelerator") ? &o->accelerator
                                                                : NULL;
        if (!slot || *slot || i + 1 == argc)
            return -1;
        *slot = argv[i + 1];
    }
    return o->tree && o->etude && o->reference && o->output && o->site && o->preparation &&
                   o->accelerator
               ? 0
               : -1;
}

static int reference_preparation(const struct reference_options *o, const struct json *selected)
{
    struct json *preparation = ret_document(o->preparation);
    const char *format = json_text(preparation, "format"),
               *outcome = json_text(preparation, "outcome");
    const char *cleanup = json_text(preparation, "cleanup"),
               *machine = json_text(preparation, "machine_id");
    const char *expected = json_text(selected, "machine_id"),
               *accelerator = json_text(selected, "accelerator");
    int rc = !format || strcmp(format, "etude-preparation-receipt") || !outcome ||
             strcmp(outcome, "pass") || !cleanup || strcmp(cleanup, "confirmed") || !machine ||
             !expected || strcmp(machine, expected) || !accelerator ||
             strcmp(accelerator, o->accelerator);
    json_free(preparation);
    return rc;
}

static int reference_site(const struct reference_options *o, const struct json *policy)
{
    const struct json *selected = json_member(policy, "site"),
                      *capacity = json_member(selected, "capacity");
    int rc = reference_preparation(o, selected);
    FILE *file = !rc ? fopen(o->site, "rb") : NULL;
    char line[8192], endpoint[64] = {0};
    long long cpu = -1, memory = -1, disk = -1;
    unsigned endpoints = 0, capacities = 0;
    while (file && fgets(line, sizeof line, file)) {
        if (!strncmp(line, "endpoint ", 9)) {
            line[strcspn(line, "\r\n")] = 0;
            if (snprintf(endpoint, sizeof endpoint, "%s", line + 9) >= (int)sizeof endpoint)
                rc = -1;
            endpoints++;
        } else if (!strncmp(line, "capacity ", 9)) {
            int n = 0;
            if (sscanf(line + 9, "%lld,%lld,%lld%n", &cpu, &memory, &disk, &n) != 3 ||
                strspn(line + 9 + n, "\r\n") != strlen(line + 9 + n))
                rc = -1;
            capacities++;
        }
    }
    if (!file || ferror(file) || endpoints != 1 || capacities != 1)
        rc = -1;
    if (file)
        fclose(file);
    const char *os = json_text(selected, "os");
    if (!os || strcmp(os, endpoint) || ret_number(capacity, "cpu_millis") != cpu ||
        ret_number(capacity, "memory_bytes") != memory ||
        ret_number(capacity, "disk_bytes") != disk)
        rc = -1;
    return rc ? ret_fail("reference policy differs from successful preparation or the actual "
                         "selected site")
              : 0;
}

static int reference_order(const void *a, const void *b)
{
    const struct json *const *x = a, *const *y = b;
    return strcmp((*x)->string, (*y)->string);
}
static struct json *reference_manifests(const char *tree)
{
    char *directory = ret_format("%s/tests/cases", tree);
    DIR *dir = directory ? opendir(directory) : NULL;
    struct json *paths = ret_new(JSON_ARRAY);
    struct dirent *entry;
    int rc = !dir;
    while (!rc && (entry = ret_next(dir, &rc))) {
        size_t n = strlen(entry->d_name);
        if (n < 5 || strcmp(entry->d_name + n - 5, ".json"))
            continue;
        char *path = ret_format("%s/%s", directory, entry->d_name);
        struct json *ref = path ? ret_descriptor(path) : NULL;
        if (!ref)
            rc = -1;
        else
            ret_push(paths, ret_string(json_text(ref, "path")));
        json_free(ref);
        free(path);
    }
    if (dir)
        closedir(dir);
    free(directory);
    if (rc || !paths->count) {
        json_free(paths);
        ret_fail("no complete case manifests");
        return NULL;
    }
    qsort(paths->item, paths->count, sizeof(*paths->item), reference_order);
    return paths;
}

static char *reference_receipt_path(const char *output)
{
    char *copy = strdup(output);
    if (!copy)
        return NULL;
    char *slash = strrchr(copy, '/'), *dot = strrchr(copy, '.');
    if (dot && (!slash || dot > slash))
        *dot = 0;
    char *result = ret_format("%s.receipt.json", copy);
    free(copy);
    return result;
}

static int reference_save(const struct reference_options *o, const struct json *descriptor,
                          const struct json *command, int status)
{
    struct json *receipt = ret_new(JSON_OBJECT), *log = ret_descriptor(o->output);
    char *receipt_path = reference_receipt_path(o->output);
    ret_set(receipt, "format", ret_string("etude-reference-check"));
    ret_set(receipt, "schema_version", ret_integer(1));
    ret_set(receipt, "reference", ret_clone(descriptor));
    ret_set(receipt, "command", ret_clone(command));
    ret_set(receipt, "status", ret_integer(status));
    if (log)
        ret_set(receipt, "log", log);
    int rc = !log || !receipt_path || ret_write(receipt_path, receipt, 0);
    json_free(receipt);
    free(receipt_path);
    return rc;
}

static int reference_recheck(const struct json *descriptor)
{
    const struct json *policy_ref = json_member(descriptor, "policy"),
                      *histories = json_member(descriptor, "histories");
    char *path = ret_checked(policy_ref);
    int rc = !path;
    free(path);
    for (size_t i = 0; !rc && i < histories->count; i++) {
        path = ret_checked(histories->item[i]);
        rc = !path;
        free(path);
    }
    return rc;
}

static int reference_forecast(const struct reference_options *o, volatile sig_atomic_t *stop)
{
    struct json *descriptor = ret_document(o->reference), *policy = NULL, *manifests = NULL;
    const char *format = json_text(descriptor, "format");
    const struct json *histories = json_member(descriptor, "histories"),
                      *policy_ref = json_member(descriptor, "policy");
    struct json *paths = ret_new(JSON_ARRAY), *command = ret_new(JSON_ARRAY);
    char *policy_path = NULL, **argv = NULL, **reading = NULL;
    struct etude_retention_guard *guard = NULL;
    int rc = !format || strcmp(format, "etude-site-reference") ||
             ret_number(descriptor, "schema_version") != 1 || !histories ||
             histories->type != JSON_ARRAY || !histories->count;
    if (!rc) {
        policy_path = ret_checked(policy_ref);
        policy = policy_path ? ret_document(policy_path) : NULL;
        rc = !policy;
    }
    if (!rc)
        rc = reference_site(o, policy);
    if (!rc)
        ret_push(paths, ret_string(policy_path));
    for (size_t i = 0; !rc && i < histories->count; i++) {
        char *path = ret_checked(histories->item[i]);
        if (!path)
            rc = -1;
        else
            ret_push(paths, ret_string(path));
        free(path);
    }
    if (!rc) {
        manifests = reference_manifests(o->tree);
        rc = !manifests;
    }
    char *schema = ret_format("%s/tools/etude/schemas", o->tree);
    const char *prefix[] = {o->etude,   "estimate",  "-s",      schema,
                            "forecast", policy_path, "release", NULL};
    if (!rc) {
        for (size_t i = 0; prefix[i]; i++)
            ret_push(command, ret_string(prefix[i]));
        for (size_t i = 0; i < manifests->count; i++)
            ret_push(command, ret_clone(manifests->item[i]));
        ret_push(command, ret_string("--"));
        for (size_t i = 1; i < paths->count; i++)
            ret_push(command, ret_clone(paths->item[i]));
        argv = json_alloc((command->count + 1) * sizeof(*argv));
        reading = json_alloc((paths->count + 1) * sizeof(*reading));
        for (size_t i = 0; i < command->count; i++)
            argv[i] = command->item[i]->string;
        for (size_t i = 0; i < paths->count; i++)
            reading[i] = paths->item[i]->string;
        rc = etude_retention_read_begin((const char *const *)reading, 0, stop, &guard);
    }
    int status = -1;
    if (!rc) {
        int fd = open(o->output, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0)
            rc = ret_fail("reference log output already exists or cannot be created");
        else {
            close(fd);
            status = ret_execute(argv, NULL, o->output, NULL, stop, 0);
            rc = status < 0;
        }
    }
    etude_retention_read_end(guard);
    if (!rc)
        rc = reference_save(o, descriptor, command, status);
    if (!rc)
        rc = reference_recheck(descriptor);
    free(schema);
    free(policy_path);
    free(argv);
    free(reading);
    json_free(descriptor);
    json_free(policy);
    json_free(manifests);
    json_free(paths);
    json_free(command);
    return rc ? -1 : status;
}

static int reference_paths(struct reference_options *o, char *owned[6])
{
    const char **slots[] = {&o->tree,   &o->etude, &o->reference,
                            &o->output, &o->site,  &o->preparation};
    for (size_t i = 0; i < 6; i++) {
        owned[i] = ret_path(*slots[i], i != 3);
        if (!owned[i])
            return ret_fail("reference input path is unavailable");
        *slots[i] = owned[i];
    }
    return 0;
}

int etude_reference_main(int argc, char **argv, volatile sig_atomic_t *stop)
{
    struct reference_options o;
    char *owned[6] = {0};
    if (stop && *stop)
        return 128 + *stop;
    int rc = reference_options(argc, argv, &o);
    if (rc)
        ret_fail("usage: etude reference --tree TREE --etude ETUDE --reference REF --output LOG "
                 "--site SITE --preparation RECEIPT --accelerator ACCEL");
    else {
        rc = reference_paths(&o, owned);
        if (!rc)
            rc = reference_forecast(&o, stop);
    }
    for (size_t i = 0; i < 6; i++)
        free(owned[i]);
    if (rc < 0)
        fprintf(stderr, "ci: reference: %s\n", ret_error());
    return stop && *stop ? 128 + *stop : rc < 0 ? 2 : rc;
}
