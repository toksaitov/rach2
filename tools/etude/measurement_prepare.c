/* Capture preparation inputs through the existing native executor. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "etude/artifact.h"
#include "etude/exec.h"
#include "measurement_context.h"
#include "preparation_record.h"
#include "routine_proof.h"

#define SOURCE_PATH_MAX  4096
#define SOURCE_NAMES_MAX (16u << 20)

struct source_names {
    char **paths;
    size_t count, capacity;
};

static int source_git_program(const char *tree, char out[SOURCE_PATH_MAX])
{
#ifdef _WIN32
    (void)tree;
    return etude_win_program("git", out, SOURCE_PATH_MAX);
#else
    const char *path = getenv("PATH");
    if (!path)
        path = "/usr/bin:/bin";
    for (;;) {
        const char *end;
        end = path + strcspn(path, ":");
        int n = *path == '/'
                    ? snprintf(out, SOURCE_PATH_MAX, "%.*s/git", (int)(end - path), path)
                    : snprintf(out, SOURCE_PATH_MAX, "%s/%.*s/git", tree, (int)(end - path), path);
        char *resolved =
            n >= 0 && n < SOURCE_PATH_MAX && !access(out, X_OK) ? realpath(out, NULL) : NULL;
        if (resolved) {
            n = snprintf(out, SOURCE_PATH_MAX, "%s", resolved);
            free(resolved);
            return n < 0 || n >= SOURCE_PATH_MAX ? -1 : 0;
        }
        if (!*end)
            break;
        path = end + 1;
    }
    errno = ENOENT;
    return -1;
#endif
}

/* A query cannot publish a snapshot after output loss or uncertain cleanup.
 * A quiet query keeps git's diagnostics to itself. */
static char *source_query(const char *tree, char *const args[], size_t *size, int quiet)
{
    struct etude_exec *executor = etude_exec_new();
    struct etude_proc *process = NULL;
    char program[SOURCE_PATH_MAX], *resolved[16];
    size_t count = 0;
    while (args[count] && count + 1 < sizeof resolved / sizeof(*resolved)) {
        resolved[count] = args[count];
        count++;
    }
    resolved[count] = NULL;
    struct etude_exec_spec spec = {.attempt_id = "prepare-source-query",
                                   .argv = resolved,
                                   .cwd = tree,
                                   .output_max = SOURCE_NAMES_MAX,
                                   .grace_us = 1000000};
    char *bytes = NULL;
    if (!executor || args[count] || source_git_program(tree, program))
        goto done;
    resolved[0] = program;
    if (etude_exec_start(executor, &spec, &process)) {
        fprintf(stderr, "etude prepare: git query could not start: %s\n", strerror(errno));
        goto done;
    }
    while (etude_exec_wait(executor, -1) != process) {
    }
    const struct etude_exec_result *result = etude_proc_result(process);
    if (result->end != ETUDE_END_EXITED || result->value || result->output_error ||
        result->cleanup != ETUDE_CLEANUP_CONFIRMED || result->out.total != result->out.kept) {
        if (result->err.kept && !quiet)
            fwrite(result->err.data, 1, result->err.kept, stderr);
        goto done;
    }
    bytes = malloc(result->out.kept + 1);
    if (bytes) {
        if (result->out.kept)
            memcpy(bytes, result->out.data, result->out.kept);
        bytes[result->out.kept] = 0;
        *size = result->out.kept;
    }
done:
    if (executor)
        etude_exec_free(executor);
    return bytes;
}

/* Read HEAD's commit as 40 hexadecimal digits. */
static int source_head(const char *tree, char commit[41], int quiet)
{
    char *head_args[] = {"git", "rev-parse", "HEAD", NULL};
    size_t size = 0;
    char *bytes = source_query(tree, head_args, &size, quiet);
    while (size && (bytes[size - 1] == '\n' || bytes[size - 1] == '\r'))
        size--;
    int valid = bytes && size == 40 && strspn(bytes, "0123456789abcdef") == 40;
    if (valid) {
        memcpy(commit, bytes, 40);
        commit[40] = 0;
    }
    free(bytes);
    return valid ? 0 : -1;
}

int etude_context_head(const char *tree, char commit[41])
{
    char *prefix_args[] = {"git", "rev-parse", "--show-prefix", NULL};
    size_t size = 0;
    char *bytes = source_query(tree, prefix_args, &size, 1);
    int top = bytes && strspn(bytes, "\r\n") == size;
    free(bytes);
    return top ? source_head(tree, commit, 1) : -1;
}

int etude_context_candidate(const char *tree, char commit[41])
{
    char *status_args[] = {"git", "status", "--porcelain", "--untracked-files=all", NULL};
    size_t size = 0;
    char *bytes;
    if (source_head(tree, commit, 0))
        return -1;
    bytes = source_query(tree, status_args, &size, 0);
    int valid = bytes && !size;
    free(bytes);
    return valid ? 0 : -1;
}

static int source_frozen(const char *tree, const char *commit)
{
    char actual[41];
    return etude_context_candidate(tree, actual) || strcmp(actual, commit) ? -1 : 0;
}

static int source_relative(const char *path)
{
    if (!*path || *path == '/' || strchr(path, ':') || strchr(path, '\\'))
        return 0;
    for (const unsigned char *at = (const unsigned char *)path; *at; at++)
        if (*at > 127)
            return 0;
    while (*path) {
        size_t size = strcspn(path, "/");
        if (!size || (size == 1 && *path == '.') || (size == 2 && !strncmp(path, "..", 2)))
            return 0;
        path += size;
        if (!*path)
            return 1;
        path++;
        if (!*path)
            return 0;
    }
    return 1;
}

static int source_add(struct source_names *names, const char *path)
{
    if (!source_relative(path))
        return -1;
    if (names->count == names->capacity) {
        size_t capacity = names->capacity ? names->capacity * 2 : 64;
        if (capacity > SOURCE_NAMES_MAX / sizeof(*names->paths))
            return -1;
        char **more = realloc(names->paths, capacity * sizeof(*more));
        if (!more)
            return -1;
        names->paths = more;
        names->capacity = capacity;
    }
    names->paths[names->count] = strdup(path);
    if (!names->paths[names->count])
        return -1;
    names->count++;
    return 0;
}

static int source_list(const char *tree, struct source_names *names)
{
    char *args[] = {"git", "ls-files", "-z", NULL};
    size_t size = 0, at = 0;
    char *bytes = source_query(tree, args, &size, 0);
    int error = !bytes || !size || bytes[size - 1] != 0;
    while (!error && at < size) {
        error = source_add(names, bytes + at);
        at += strlen(bytes + at) + 1;
    }
    free(bytes);
    return error ? -1 : 0;
}

static int source_by_path(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int source_same(const struct stat *a, const struct stat *b)
{
    return S_ISREG(a->st_mode) && S_ISREG(b->st_mode) && a->st_dev == b->st_dev &&
           a->st_ino == b->st_ino && a->st_size == b->st_size &&
           etude_record_mtime_ns(a) == etude_record_mtime_ns(b);
}

int etude_context_digest(const char *path, struct stat *st, char digest[65])
{
    struct stat before, opened;
    if (stat(path, &before) || !S_ISREG(before.st_mode) ||
        etude_digest_file(NULL, path, &opened, digest) || stat(path, st))
        return -1;
    if (!source_same(&before, &opened) || !source_same(&opened, st) || st->st_size < 0 ||
        (uint64_t)st->st_size > JSON_INTEGER_MAX) {
        errno = EBUSY;
        return -1;
    }
    return 0;
}

static int source_input(struct jbuf *buffer, const char *tree, const char *name)
{
    char path[SOURCE_PATH_MAX], digest[65], numbers[160];
    struct stat before, after;
    int n = snprintf(path, sizeof path, "%s/%s", tree, name);
    if (n < 0 || (size_t)n >= sizeof path || lstat(path, &before) || !S_ISREG(before.st_mode) ||
        etude_context_digest(path, &after, digest)) {
        fprintf(stderr, "etude prepare: source input cannot be captured: %s\n", name);
        return -1;
    }
    jbuf_puts(buffer, "{\"path\":");
    routine_quote(buffer, name);
    snprintf(numbers, sizeof numbers, ",\"sha256\":\"%s\",\"size_bytes\":%llu}", digest,
             (unsigned long long)after.st_size);
    jbuf_puts(buffer, numbers);
    return 0;
}

static int source_document(const char *tree, const char *commit, struct source_names *names,
                           struct jbuf *buffer)
{
    int error = 0;
    qsort(names->paths, names->count, sizeof(*names->paths), source_by_path);
    jbuf_puts(buffer, "{\"commit\":");
    routine_quote(buffer, commit);
    jbuf_puts(buffer, ",\"inputs\":[");
    size_t written = 0;
    for (size_t i = 0; !error && i < names->count; i++) {
        if (i && !strcmp(names->paths[i - 1], names->paths[i]))
            continue;
        if (written++)
            jbuf_puts(buffer, ",");
        error = source_input(buffer, tree, names->paths[i]);
    }
    jbuf_puts(buffer, "],\"schema_version\":1,\"scope\":\"all tracked regular files and "
                      "explicitly listed prepared source inputs\"}\n");
    if (!error)
        error = source_frozen(tree, commit);
    return error ? -1 : 0;
}

int etude_context_source_build(const char *tree, const char *commit, const char *const extra[],
                               struct jbuf *buffer)
{
    struct source_names names = {0};
    char *root = tree ? realpath(tree, NULL) : NULL;
    int error = !root || !commit || strlen(commit) != 40 ||
                strspn(commit, "0123456789abcdef") != 40 || !buffer || buffer->len;
    if (!error)
        error = source_frozen(root, commit) || source_list(root, &names);
    for (size_t i = 0; !error && extra && extra[i]; i++)
        error = source_add(&names, extra[i]);
    if (!error)
        error = source_document(root, commit, &names, buffer);
    for (size_t i = 0; i < names.count; i++)
        free(names.paths[i]);
    free(names.paths);
    free(root);
    return error ? -1 : 0;
}

int etude_context_source(const char *tree, const char *commit, const char *output,
                         const char *const extra[])
{
    struct jbuf buffer = {0};
    int error = !output || etude_context_source_build(tree, commit, extra, &buffer) ||
                routine_create(output, &buffer);
    jbuf_free(&buffer);
    return error ? -1 : 0;
}

int etude_context_descriptor(struct jbuf *buffer, const char *input)
{
    char *path = realpath(input, NULL), digest[65], numbers[160];
    struct stat st;
    int error = !path || etude_context_digest(path, &st, digest);
    if (!error) {
        jbuf_puts(buffer, "{\"path\":");
        routine_quote(buffer, path);
        snprintf(numbers, sizeof numbers, ",\"sha256\":\"%s\",\"size_bytes\":%llu}", digest,
                 (unsigned long long)st.st_size);
        jbuf_puts(buffer, numbers);
    }
    free(path);
    return error ? -1 : 0;
}

static int source_descriptor(const char *output)
{
    struct jbuf buffer = {0};
    int error = etude_context_descriptor(&buffer, output);
    jbuf_puts(&buffer, "\n");
    if (!error)
        error = fwrite(buffer.data, 1, buffer.len, stdout) != buffer.len || fflush(stdout);
    jbuf_free(&buffer);
    return error ? -1 : 0;
}

int etude_prepare_source_main(int argc, char **argv)
{
    const char *tree = NULL, *commit = NULL, *output = NULL;
    struct source_names extra = {0};
    int error = 0;
    for (int i = 1; !error && i < argc; i += 2) {
        if (i + 1 == argc)
            error = 1;
        else if (!strcmp(argv[i], "--tree") && !tree)
            tree = argv[i + 1];
        else if (!strcmp(argv[i], "--commit") && !commit)
            commit = argv[i + 1];
        else if (!strcmp(argv[i], "--output") && !output)
            output = argv[i + 1];
        else if (!strcmp(argv[i], "--extra"))
            error = source_add(&extra, argv[i + 1]);
        else
            error = 1;
    }
    const char **extras = calloc(extra.count + 1, sizeof(*extras));
    if (extras)
        for (size_t i = 0; i < extra.count; i++)
            extras[i] = extra.paths[i];
    error = error || !extras || etude_context_source(tree, commit, output, extras);
    if (!error)
        error = source_descriptor(output);
    if (error)
        fputs("etude prepare: source snapshot refused or could not be written\n", stderr);
    for (size_t i = 0; i < extra.count; i++)
        free(extra.paths[i]);
    free(extra.paths);
    free(extras);
    return error ? 2 : 0;
}
