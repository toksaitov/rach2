/* Fixed inputs and existing process ownership for native routine commands. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "routine_entry.h"
#include "routine_proof.h"
#include "host_prepare.h"
#include "authority_internal.h"
#include "authority_run.h"
#include "measurement_context.h"
#include "sha256.h"

int routine_path(char out[ROUTINE_PATH], const char *root, const char *name)
{
    int n = snprintf(out, ROUTINE_PATH, "%s/%s", root, name);
    return n < 0 || n >= ROUTINE_PATH ? -1 : 0;
}

/**
 * Print on stderr why the routine input `name` was refused, and return -1.
 */
static int refused(const char *name, const char *why)
{
    fprintf(stderr, "etude routine: %s: %s\n", name, why);
    return -1;
}

/**
 * Resolve `path`, which must name an existing directory or regular file.
 *
 * Nothing is made: an OUT directory must exist before the command runs. A
 * refusal says on stderr which input, `name`, was wrong.
 */
static int input_path(const char *name, const char *path, char out[ROUTINE_PATH], int directory)
{
    char *absolute = realpath(path, NULL);
    struct stat st;
    const char *why = NULL;
    if (!absolute || stat(absolute, &st))
        why = strerror(errno);
    else if (strlen(absolute) >= ROUTINE_PATH)
        why = strerror(ENAMETOOLONG);
    else if (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode))
        why = directory ? "not a directory" : "not a regular file";
    else
        strcpy(out, absolute);
    free(absolute);
    return why ? refused(name, why) : 0;
}

static int selection_words(int argc, char **argv)
{
    int i = 0;
    for (; i < argc; i += 2) {
        if (strcmp(argv[i], "-S") && strcmp(argv[i], "-R") && strcmp(argv[i], "-L"))
            break;
        if (i + 1 == argc || !*argv[i + 1])
            return -1;
    }
    if (i == argc)
        return -1;
    for (; i < argc; i++)
        if (!*argv[i] || *argv[i] == '-')
            return -1;
    return 0;
}

int routine_input_read(int argc, char **argv, struct routine_input *input)
{
    memset(input, 0, sizeof *input);
    if (argc < 10)
        return refused("usage",
                       "PURPOSE TREE OUT ARCHES RUN_ID SITE IMMUTABLE_ETUDE ACCEL SELECTION...");
    if (input_path("TREE", argv[2], input->tree, 1) || input_path("OUT", argv[3], input->out, 1) ||
        input_path("SITE", argv[6], input->site, 0) ||
        input_path("IMMUTABLE_ETUDE", argv[7], input->program, 0))
        return -1;
    if (access(input->program, X_OK))
        return refused("IMMUTABLE_ETUDE", strerror(errno));
    input->purpose = argv[1];
    input->arches = argv[4];
    input->run = argv[5];
    input->accel = argv[8];
    input->count = argc - 9;
    input->selection = argv + 9;
    if (strcmp(input->purpose, "push") && strcmp(input->purpose, "ci") &&
        strcmp(input->purpose, "release") && strcmp(input->purpose, "focused") &&
        strcmp(input->purpose, "phase_first_run"))
        return refused("PURPOSE", "not push, ci, release, focused or phase_first_run");
    if (strcmp(input->arches, "both") && strcmp(input->arches, "i686") &&
        strcmp(input->arches, "x86_64"))
        return refused("ARCHES", "not both, i686 or x86_64");
    if (strcmp(input->accel, "tcg") && strcmp(input->accel, "kvm") && strcmp(input->accel, "hvf") &&
        strcmp(input->accel, "whpx"))
        return refused("ACCEL", "not tcg, kvm, hvf or whpx");
    if (!authority_token(input->run))
        return refused("RUN_ID", "not a run token");
    if (selection_words(input->count, input->selection))
        return refused("SELECTION", "not -S, -R or -L pairs followed by suite names");
    if (etude_context_candidate(input->tree, input->commit))
        return refused("TREE", "not a git checkout without changes or untracked files");
    return 0;
}

int routine_command(struct hp_words *args, volatile sig_atomic_t *stop)
{
    if (stop && *stop)
        return 128 + *stop;
    struct authority_run run = {.phase = "routine-preparation", .argv = args->v, .stop = stop};
    struct etude_exec_result result = {0};
    return authority_run_command(&run, &result);
}

int routine_query(const struct routine_input *input, struct hp_words *args, const char *name,
                  volatile sig_atomic_t *stop)
{
    char out[ROUTINE_PATH], err[ROUTINE_PATH], label[128];
    struct stat st;
    int n = snprintf(label, sizeof label, "%s.stdout", name);
    if (n < 0 || n >= (int)sizeof label || routine_path(out, input->out, label))
        return -1;
    n = snprintf(label, sizeof label, "%s.stderr", name);
    if (n < 0 || n >= (int)sizeof label || routine_path(err, input->out, label) ||
        !lstat(out, &st) || errno != ENOENT || !lstat(err, &st) || errno != ENOENT)
        return -1;
    struct hp_context context = {.stop = stop};
    strcpy(context.tree, input->tree);
    return hp_run(&context, args->v, out, err);
}

int routine_selection(const char *expected, const char *actual)
{
    struct json *a = NULL, *b = NULL;
    int error = routine_json(expected, &a) || routine_json(actual, &b) || !json_equal(a, b);
    json_free(a);
    json_free(b);
    return error ? -1 : 0;
}

static int phase_allocation(const struct json *doc)
{
    const struct json *items = json_member(doc, "items");
    const char *bytes = json_text(doc, "selection_bytes");
    const char *expected = json_text(doc, "selection_sha256");
    const struct json *size = json_member(doc, "selection_size_bytes");
    const struct json *version = json_member(doc, "schema_version");
    if (!items || items->type != JSON_ARRAY || !items->count || !bytes || !*bytes || !expected ||
        !size || size->type != JSON_INTEGER || size->integer != (long long)strlen(bytes) ||
        !version || version->type != JSON_INTEGER || version->integer != 1)
        return -1;
    struct sha256 digest;
    unsigned char sum[32];
    char hash[65], why[128];
    sha256_init(&digest);
    sha256_update(&digest, (const unsigned char *)bytes, strlen(bytes));
    sha256_final(&digest, sum);
    sha256_hex(sum, hash);
    struct json *allocation = NULL;
    int error =
        strcmp(hash, expected) || json_parse(bytes, strlen(bytes), 1, &allocation, why, sizeof why);
    version = json_member(allocation, "schema_version");
    error = error || !allocation || allocation->type != JSON_OBJECT || !version ||
            version->type != JSON_INTEGER || version->integer != 1;
    json_free(allocation);
    return error ? -1 : 0;
}

static struct json *phase_export(const char *path)
{
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > (64u << 20))
        return NULL;
    char *raw = hp_read(path), why[256], hash[65];
    struct json *allocation = NULL, *descriptor = NULL;
    struct jbuf buffer = {0};
    int error = !raw || strlen(raw) > (64u << 20) ||
                json_parse(raw, strlen(raw), 1, &allocation, why, sizeof why) ||
                phase_allocation(allocation) || etude_context_descriptor(&buffer, path) ||
                json_parse(buffer.data, buffer.len, 1, &descriptor, why, sizeof why);
    if (!error) {
        struct sha256 digest;
        unsigned char sum[32];
        sha256_init(&digest);
        sha256_update(&digest, (const unsigned char *)raw, strlen(raw));
        sha256_final(&digest, sum);
        sha256_hex(sum, hash);
        const struct json *size = json_member(descriptor, "size_bytes");
        error = strcmp(json_text(descriptor, "sha256"), hash) || !size ||
                size->type != JSON_INTEGER || size->integer != (long long)strlen(raw);
    }
    if (error) {
        json_free(descriptor);
        descriptor = NULL;
    }
    json_free(allocation);
    jbuf_free(&buffer);
    free(raw);
    return descriptor;
}

int routine_phase_inputs(const char *purpose, struct json **value)
{
    const char *path = getenv("ETUDE_PHASE_SELECTION_EXPORT");
    const char *clean = getenv("ETUDE_PREPARATION_CLEAN");
    int first = !strcmp(purpose, "phase_first_run");
    if (!first && path)
        return refused("ETUDE_PHASE_SELECTION_EXPORT",
                       "set for a purpose other than phase_first_run");
    if (first && !path)
        return refused("ETUDE_PHASE_SELECTION_EXPORT", "phase_first_run needs it");
    if (first && clean && strcmp(clean, "1"))
        return refused("ETUDE_PREPARATION_CLEAN", "set to a value other than 1");
    struct json *descriptor = first && authority_absolute(path) ? phase_export(path) : NULL;
    if (first && !descriptor)
        return refused("ETUDE_PHASE_SELECTION_EXPORT", "not an absolute path to a valid export");
    struct json *doc = hp_value(JSON_OBJECT);
    struct json *condition = hp_value(JSON_BOOLEAN);
    condition->integer = first ? clean != NULL : !strcmp(purpose, "release");
    hp_member(doc, "clean", condition);
    hp_member(doc, "selection_export", descriptor ? descriptor : hp_value(JSON_NULL));
    *value = doc;
    return 0;
}
