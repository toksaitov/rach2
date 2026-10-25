/* host.c: execute prepared host checks and report every declared assertion. */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#else
extern char **environ;
#endif
#include "etude/exec.h"
#include "etude/report.h"
#include "json.h"
#include "sha256.h"
#include "sanitizer_tool.h"

#define PATH_SIZE  4096
#define CHECKS_MAX 1024

struct check {
    const struct json *entry;
    const char *id, *label;
    char program[PATH_SIZE], scratch[PATH_SIZE], log[PATH_SIZE], why[PATH_SIZE];
    char **argv, **env;
    struct etude_proc *proc;
    int alone, selected, reported, interrupted;
};

struct host {
    char tree[PATH_SIZE], work[PATH_SIZE], case_id[128];
    struct json *manifest, *inventory;
    const struct json *declared, *companion, *script_parts[5];
    size_t nscript_parts;
    struct check *checks;
    size_t nchecks, reported, failures, records;
    int uncertain;
    void *report_ctx;
    void (*report_assert)(void *, const char *, int, const char *);
    void (*report_progress)(void *, size_t);
    const char *only;
    struct etude_exec *executor;
};

static volatile sig_atomic_t stopped;

static void on_signal(int sig)
{
    stopped = sig;
}

static int path_of(char out[PATH_SIZE], const char *base, const char *name)
{
    int n = snprintf(out, PATH_SIZE, "%s/%s", base, name);
    return n < 0 || n >= PATH_SIZE ? -1 : 0;
}

static int relative_path(const char *p)
{
    if (!p || !*p || *p == '/' || *p == '\\' || strchr(p, ':') || strchr(p, '\\'))
        return 0;
    for (const char *s = p; *s;) {
        size_t n = strcspn(s, "/");
        if (!n || (n == 1 && s[0] == '.') || (n == 2 && !strncmp(s, "..", 2)))
            return 0;
        s += n;
        if (*s)
            s++;
    }
    return 1;
}

static int file_digest(const char *path, char hex[65])
{
    unsigned char bytes[8192], digest[32];
    size_t n;
    struct sha256 hash;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    sha256_init(&hash);
    while ((n = fread(bytes, 1, sizeof(bytes), f)) != 0)
        sha256_update(&hash, bytes, n);
    int bad = ferror(f);
    fclose(f);
    if (bad)
        return -1;
    sha256_final(&hash, digest);
    sha256_hex(digest, hex);
    return 0;
}

static struct json *document(const char *path)
{
    struct jbuf data = {0};
    char bytes[8192], why[256];
    size_t n;
    FILE *f = fopen(path, "rb");
    struct json *out = NULL;
    if (!f)
        return NULL;
    while ((n = fread(bytes, 1, sizeof(bytes), f)) != 0) {
        if (data.len + n > 16 * 1024 * 1024)
            break;
        jbuf_put(&data, bytes, n);
    }
    if (!ferror(f) && feof(f) && json_parse(data.data, data.len, 1, &out, why, sizeof(why)) != 0)
        fprintf(stderr, "host: %s: %s\n", path, why);
    fclose(f);
    jbuf_free(&data);
    return out;
}

static int remove_tree(const char *path)
{
    struct stat st;
    DIR *d;
    struct dirent *e;
    char child[PATH_SIZE];
    int rc = 0;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    d = opendir(path);
    if (!d)
        return -1;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (path_of(child, path, e->d_name) != 0 || remove_tree(child) != 0)
            rc = -1;
    }
    closedir(d);
    return rc ? rc : rmdir(path);
}

/* Report records fit one write and contain printable ASCII only. */
static void assertion(struct host *h, const char *id, int pass, const char *why)
{
    char line[256], detail[145];
    if (h->report_assert) {
        h->report_assert(h->report_ctx, id, pass, why);
        h->records++;
        h->failures += !pass;
        return;
    }
    size_t n = 0;
    size_t limit = strlen(id) < 220 ? 220 - strlen(id) : 0;
    if (limit >= sizeof(detail))
        limit = sizeof(detail) - 1;
    if (why)
        while (*why && n < limit) {
            unsigned char c = (unsigned char)*why++;
            detail[n++] = c >= 32 && c <= 126 ? (char)c : '?';
        }
    while (n && detail[n - 1] == ' ')
        n--;
    detail[n] = 0;
    int length = snprintf(line, sizeof(line), "@etude 1 assert %s %s%s%s\n", id,
                          pass ? "pass" : "fail", pass ? "" : " ", pass ? "" : detail);
    if (length < 0 || length >= (int)sizeof(line) || write(1, line, (size_t)length) != length)
        stopped = SIGTERM;
    h->records++;
    h->failures += !pass;
}

static int declaration(const struct json *assertions, const char *id)
{
    for (size_t i = 0; assertions && i < assertions->count; i++) {
        const char *name = json_text(assertions->item[i], "assertion_id");
        if (name && !strcmp(name, id))
            return 1;
    }
    return 0;
}

static int declared(const struct host *h, const char *id)
{
    if (h->nscript_parts) {
        unsigned count = 0;
        for (size_t i = 0; i < h->nscript_parts; i++)
            count += declaration(h->script_parts[i], id);
        return count == 1;
    }
    return declaration(h->declared, id);
}

/* Both Etude partitions consume the same complete prepared inventory. */
static const char *canonical_group(const char *group)
{
    return !strcmp(group, "etude_quiet") ? "etude" : group;
}

static void progress(struct host *h)
{
    char line[80];
    if (h->report_progress) {
        h->report_progress(h->report_ctx, ++h->reported);
        return;
    }
    int n = snprintf(line, sizeof(line), "@etude 1 progress checks %lu\n",
                     (unsigned long)++h->reported);
    if (write(1, line, (size_t)n) != n)
        stopped = SIGTERM;
}

/* A child is registered before release, including with the outer executor. */
static int register_child(void *ctx, const char *id, const struct etude_pid *keeper,
                          const struct etude_pid *process)
{
    char line[96];
    (void)ctx;
    (void)id;
    int n = snprintf(line, sizeof(line), "@etude 1 child %ld\n@etude 1 child %ld\n", keeper->pid,
                     process->pid);
    return write(1, line, (size_t)n) == n ? 0 : EIO;
}

static int input_matches(const struct host *h, const char *relative, const char *expected)
{
    char path[PATH_SIZE], actual[65];
    return relative_path(relative) && expected && strlen(expected) == 64 &&
           path_of(path, h->tree, relative) == 0 && file_digest(path, actual) == 0 &&
           !strcmp(actual, expected);
}

/* Input filenames may contain bytes outside the ASCII JSON vocabulary. */
static const char *input_path(const struct json *input, char decoded[PATH_SIZE])
{
    const char *plain = json_text(input, "path");
    const struct json *encoded = json_member(input, "path_hex");
    const char *hex;
    size_t n;
    if (!encoded)
        return plain;
    if (json_member(input, "path") || encoded->type != JSON_STRING)
        return NULL;
    hex = encoded->string;
    if (!(n = strlen(hex)) || n % 2 || n / 2 >= PATH_SIZE)
        return NULL;
    for (size_t i = 0; i < n; i += 2) {
        unsigned value = 0;
        for (size_t j = 0; j < 2; j++) {
            unsigned char c = (unsigned char)hex[i + j];
            if (c >= '0' && c <= '9')
                value = value * 16 + c - '0';
            else if (c >= 'a' && c <= 'f')
                value = value * 16 + c - 'a' + 10;
            else
                return NULL;
        }
        if (!value)
            return NULL;
        decoded[i / 2] = (char)value;
    }
    decoded[n / 2] = 0;
    return decoded;
}

/* Every file present in a corpus must be one of the prepared arguments. */
static int corpus_matches(const struct host *h, const struct json *args, const char *relative)
{
    char directory[PATH_SIZE], path[PATH_SIZE];
    if (!relative_path(relative) || path_of(directory, h->tree, relative) != 0)
        return 0;
    DIR *d = opendir(directory);
    struct dirent *e;
    int ok = d != NULL;
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        char child[PATH_SIZE];
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (path_of(child, relative, e->d_name) != 0 || path_of(path, h->tree, child) != 0 ||
            lstat(path, &st) != 0) {
            ok = 0;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            ok &= corpus_matches(h, args, child);
            continue;
        }
        size_t i;
        for (i = 0; args && i < args->count; i++)
            if (args->item[i]->type == JSON_STRING && !strcmp(args->item[i]->string, child))
                break;
        if (!args || i == args->count)
            ok = 0;
    }
    closedir(d);
    return ok;
}

static int native_binary(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char magic[4];
    int binary = f && fread(magic, 1, 4, f) == 4 &&
                 (!memcmp(magic, "\177ELF", 4) || (magic[0] == 'M' && magic[1] == 'Z') ||
                  !memcmp(magic, "\317\372\355\376", 4) || !memcmp(magic, "\316\372\355\376", 4) ||
                  !memcmp(magic, "\312\376\272\276", 4) || !memcmp(magic, "\312\376\272\277", 4));
    if (f)
        fclose(f);
    return binary;
}

#ifdef __APPLE__
static const char *sanitizer_matches(const struct host *h, const struct json *inputs,
                                     const char *symbolizer)
{
    char snapshot[PATH_SIZE], native[PATH_SIZE], provenance[PATH_SIZE];
    int found = 0;
    if (!symbolizer)
        return NULL;
    if (snprintf(provenance, sizeof(provenance), "%s.provenance.json", symbolizer) >=
        (int)sizeof(provenance))
        return "the sanitizer provenance path is too long";
    for (size_t i = 0; i < inputs->count; i++) {
        char decoded[PATH_SIZE];
        const char *path = input_path(inputs->item[i], decoded);
        found |= path && !strcmp(path, provenance);
    }
    if (!found || path_of(snapshot, h->tree, symbolizer) ||
        host_sanitizer_source(snapshot, native, sizeof(native)))
        return "the native sanitizer tool differs from its prepared snapshot";
    return NULL;
}
#endif

static const char *prepared(const struct host *h, struct check *c)
{
    const char *unsupported = json_text(c->entry, "unsupported");
    if (unsupported && *unsupported)
        return unsupported;
    const char *program = json_text(c->entry, "program");
    const char *hash = json_text(c->entry, "sha256");
    if (!input_matches(h, program, hash) || path_of(c->program, h->tree, program) != 0)
        return "the prepared binary is missing or changed";
    if (!native_binary(c->program))
        return "the prepared program is a script; a native adapter is required";
    const struct json *inputs = json_member(c->entry, "inputs");
    const char *symbolizer = json_text(c->entry, "sanitizer_symbolizer");
    int symbolizer_found = symbolizer == NULL;
    if (json_member(c->entry, "sanitizer_symbolizer") &&
        (symbolizer == NULL || !relative_path(symbolizer)))
        return "the sanitizer symbolizer path is invalid";
#ifdef __APPLE__
    const struct json *instrumentation = json_member(c->entry, "instrumentation");
    if (instrumentation && instrumentation->type == JSON_ARRAY && instrumentation->count &&
        symbolizer == NULL)
        return "the prepared sanitizer symbolizer is missing";
#endif
    if (!inputs || inputs->type != JSON_ARRAY)
        return "the prepared input inventory is missing";
    for (size_t i = 0; i < inputs->count; i++) {
        char decoded[PATH_SIZE];
        const char *path = input_path(inputs->item[i], decoded);
        if (!input_matches(h, path, json_text(inputs->item[i], "sha256"))) {
            snprintf(c->why, sizeof(c->why),
                     "a source or corpus input changed after preparation: %.4000s",
                     path ? path : "(invalid path)");
            return c->why;
        }
        if (symbolizer && !strcmp(path, symbolizer))
            symbolizer_found = 1;
    }
    if (!symbolizer_found)
        return "the sanitizer symbolizer is not a prepared input";
#ifdef __APPLE__
    const char *error = sanitizer_matches(h, inputs, symbolizer);
    if (error)
        return error;
#endif
    const char *corpus = json_text(c->entry, "corpus");
    if (corpus && !corpus_matches(h, json_member(c->entry, "arguments"), corpus))
        return "the corpus contains files that were not prepared";
    return NULL;
}

static int environment(struct host *h, struct check *c)
{
    size_t n = 0, used = 0;
    while (environ[n])
        n++;
    c->env = calloc(n + 5, sizeof(*c->env));
    if (!c->env)
        return -1;
    for (size_t i = 0; i < n; i++) {
        if (!strncmp(environ[i], "TMPDIR=", 7) || !strncmp(environ[i], "ETUDE_TREE=", 11) ||
            !strncmp(environ[i], "ASAN_OPTIONS=", 13) ||
            !strncmp(environ[i], "ASAN_SYMBOLIZER_PATH=", 21) ||
            !strncmp(environ[i], "UBSAN_OPTIONS=", 14) || !strncmp(environ[i], "LSAN_OPTIONS=", 13))
            continue;
        c->env[used++] = strdup(environ[i]);
    }
    char tmp[PATH_SIZE + 8];
    snprintf(tmp, sizeof(tmp), "TMPDIR=%s", c->scratch);
    c->env[used++] = strdup(tmp);
    c->env[used++] = strdup("ASAN_OPTIONS=halt_on_error=1:exitcode=1");
    c->env[used++] = strdup("UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:exitcode=1");
    const char *symbolizer = json_text(c->entry, "sanitizer_symbolizer");
    if (symbolizer) {
        char path[PATH_SIZE], value[PATH_SIZE + 32];
        if (!relative_path(symbolizer) || path_of(path, h->tree, symbolizer) != 0)
            return -1;
#ifdef __APPLE__
        char native[PATH_SIZE];
        if (host_sanitizer_source(path, native, sizeof(native)))
            return -1;
        strcpy(path, native);
#endif
        snprintf(value, sizeof(value), "ASAN_SYMBOLIZER_PATH=%s", path);
        c->env[used++] = strdup(value);
    }
    for (size_t i = 0; i < used; i++)
        if (!c->env[i])
            return -1;
    return 0;
}

static int start(struct host *h, struct check *c)
{
    const struct json *args = json_member(c->entry, "arguments");
    struct etude_exec_spec spec;
    if (!args || args->type != JSON_ARRAY || args->count > 8192)
        return -1;
    if (path_of(c->scratch, h->work, c->id) != 0 || mkdir(c->scratch, 0700) != 0)
        return -1;
    char name[160];
    snprintf(name, sizeof(name), "%s.log", c->id);
    if (path_of(c->log, h->work, name) != 0 || environment(h, c) != 0)
        return -1;
    c->argv = calloc(args->count + 2, sizeof(*c->argv));
    if (!c->argv)
        return -1;
    c->argv[0] = c->program;
    for (size_t i = 0; i < args->count; i++) {
        if (args->item[i]->type != JSON_STRING)
            return -1;
        c->argv[i + 1] = args->item[i]->string;
    }
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = c->id;
    spec.argv = c->argv;
    spec.envp = c->env;
    spec.cwd = h->tree;
    spec.output_max = 4096;
    spec.stdout_path = c->log;
    spec.stderr_path = c->log;
    spec.grace_us = 2000000;
    spec.reg = register_child;
    return etude_exec_start(h->executor, &spec, &c->proc);
}

static void print_log(const char *path, int failed)
{
    FILE *f = fopen(path, "r");
    char *line = NULL;
    size_t room = 0;
    if (!f)
        return;
    while (getline(&line, &room, f) >= 0)
        if (failed || strstr(line, ": not run: "))
            printf("    | %s%s", line, strchr(line, '\n') ? "" : "\n");
    free(line);
    fclose(f);
}

static int empty_directory(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    int empty = d != NULL;
    while (d && (e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
            empty = 0;
            break;
        }
    if (d)
        closedir(d);
    return empty;
}

static void finished(struct host *h, struct check *c, const char *error)
{
    int ok = error == NULL;
    printf("  %s: %s%s%s\n",
           c->interrupted ? "STOPPED"
           : ok           ? "PASS"
                          : "FAIL",
           c->label, error ? ": " : "", error ? error : "");
    if (c->log[0])
        print_log(c->log, !ok);
    if (c->scratch[0]) {
        if ((ok || stopped || empty_directory(c->scratch)) && !h->uncertain)
            remove_tree(c->scratch);
        else
            printf("    scratch kept: %s\n", c->scratch);
    }
    if (!c->interrupted) {
        assertion(h, c->id, ok, error);
        progress(h);
    }
    c->reported = 1;
}

/* A new source check must not disappear behind an older inventory. */
static int live_sources(struct host *h, const char *group)
{
    int listed = 1;
    const char *directory = !strcmp(group, "fuzz") ? "tests/fuzz" : "tests/host";
    if (!strcmp(group, "tests") || !strcmp(group, "etude") || !strcmp(group, "boots") ||
        !strcmp(group, "fuzz")) {
        char path[PATH_SIZE];
        if (path_of(path, h->tree, directory) != 0)
            return -1;
        DIR *d = opendir(path);
        struct dirent *e;
        if (!d)
            return -1;
        while ((e = readdir(d)) != NULL) {
            const char *suffix = !strcmp(group, "fuzz") ? "_fuzz.c" : "_hosttest.c";
            size_t n = strlen(e->d_name), z = strlen(suffix);
            if (n <= z || strcmp(e->d_name + n - z, suffix))
                continue;
            char source[PATH_SIZE];
            if (path_of(source, path, e->d_name) != 0) {
                closedir(d);
                return -1;
            }
            FILE *f = fopen(source, "r");
            char line[4096];
            int boots = 0;
            if (!f) {
                closedir(d);
                return -1;
            }
            while (fgets(line, sizeof(line), f))
                boots |= strstr(line, "host-arch:") != NULL;
            fclose(f);
            if (strcmp(group, "fuzz")) {
                const char *actual = boots                              ? "boots"
                                     : !strncmp(e->d_name, "etude_", 6) ? "etude"
                                                                        : "tests";
                if (strcmp(actual, group))
                    continue;
            }
            char id[160];
            if (n - z >= sizeof(id)) {
                listed = 0;
                continue;
            }
            memcpy(id, e->d_name, n - z);
            id[n - z] = 0;
            size_t i;
            for (i = 0; i < h->nchecks && strcmp(id, h->checks[i].id); i++) {
            }
            if (i == h->nchecks) {
                listed = 0;
                printf("host: listed: source check %s was not prepared\n", id);
            }
        }
        closedir(d);
    }
    return listed;
}

static int live_scripts(struct host *h, const char *group)
{
    int listed = 1;
    if (!strcmp(group, "scripts")) {
        char path[PATH_SIZE], line[8192];
        if (path_of(path, h->tree, "tests/cases/host/host.list") != 0)
            return -1;
        FILE *f = fopen(path, "r");
        if (!f)
            return -1;
        while (fgets(line, sizeof(line), f)) {
            if (!*line || *line == '#' || *line == '\n')
                continue;
            char *separator = strchr(line, '|');
            if (!separator) {
                listed = 0;
                continue;
            }
            *separator = 0;
            size_t i;
            for (i = 0; i < h->nchecks && strcmp(line, h->checks[i].id); i++) {
            }
            if (i == h->nchecks) {
                listed = 0;
                printf("host: listed: script check %s was not prepared\n", line);
            }
        }
        if (ferror(f))
            listed = 0;
        fclose(f);
    }
    return listed;
}

/* Missing declared entries still produce their own failed assertion. */
static int complete_declarations(struct host *h, const struct json *assertions, int selected)
{
    int listed = 1;
    for (size_t i = 0; assertions && i < assertions->count; i++) {
        const char *id = json_text(assertions->item[i], "assertion_id");
        if (!id || !strcmp(id, "listed"))
            continue;
        size_t j;
        for (j = 0; j < h->nchecks && strcmp(id, h->checks[j].id); j++) {
        }
        if (j == h->nchecks) {
            listed = 0;
            if (selected) {
                struct check absent = {0};
                absent.id = id;
                absent.label = id;
                finished(h, &absent, "the prepared inventory has no check for this assertion");
            } else {
                printf("host: listed: companion assertion %s was not prepared\n", id);
            }
        }
    }
    return listed;
}

/* Every prepared entry belongs to exactly one matching scheduling partition. */
static int partition_matches(const struct host *h, const struct check *c)
{
    int other = declaration(h->companion, c->id);
    if (!h->companion)
        return c->selected;
    int quiet = !strcmp(h->case_id, "host.host.etude_quiet") ? c->selected : other;
    return c->selected != other && c->alone == quiet;
}

static int select_one(struct host *h)
{
    if (h->only) {
        int found = 0;
        for (size_t i = 0; i < h->nchecks; i++) {
            h->checks[i].selected &= !strcmp(h->checks[i].id, h->only);
            found += h->checks[i].selected;
        }
        if (found != 1)
            return -1;
    }
    return 0;
}

static int inventory_declarations(struct host *h)
{
    int listed = 1;
    listed &= complete_declarations(h, h->declared, 1);
    listed &= complete_declarations(h, h->companion, 0);
    for (size_t i = 0; i < h->nscript_parts; i++)
        if (h->script_parts[i] != h->declared)
            listed &= complete_declarations(h, h->script_parts[i], 1);
    return listed;
}

static int check_inventory(struct host *h, const char *group)
{
    const struct json *entries = json_member(h->inventory, "checks");
    if (!entries || entries->type != JSON_ARRAY || entries->count > CHECKS_MAX)
        return -1;
    h->checks = calloc(entries->count + 1, sizeof(*h->checks));
    if (!h->checks)
        return -1;
    int listed = 1;
    for (size_t i = 0; i < entries->count; i++) {
        struct check *c = &h->checks[h->nchecks++];
        c->entry = entries->item[i];
        c->id = json_text(c->entry, "assertion_id");
        c->label = json_text(c->entry, "label");
        const char *mode = json_text(c->entry, "mode");
        if (!c->id || strlen(c->id) > 128 ||
            strspn(c->id, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") !=
                strlen(c->id) ||
            !c->label || !mode || !relative_path(c->id) || strchr(c->id, '/') ||
            (strcmp(mode, "alone") && strcmp(mode, "shared")))
            return -1;
        c->alone = !strcmp(mode, "alone");
        c->selected = declared(h, c->id);
        for (size_t j = 0; j < i; j++)
            if (!strcmp(c->id, h->checks[j].id))
                return -1;
        if (!partition_matches(h, c)) {
            listed = 0;
            c->selected = 0;
            printf("host: listed: unlisted or wrongly partitioned prepared check %s\n", c->id);
        }
    }
    listed &= inventory_declarations(h);
    group = canonical_group(group);
    int sources = live_sources(h, group), scripts = live_scripts(h, group);
    if (sources < 0 || scripts < 0)
        return -1;
    listed &= sources && scripts;
    assertion(h, "listed", listed, "the declared, source and prepared inventories differ");
    if ((h->companion || h->nscript_parts) && !listed)
        return -1;
    return select_one(h);
}

static void finish_process(struct host *h, struct check *c, struct etude_proc *p)
{
    const struct etude_exec_result *result = etude_proc_result(p);
    char why[160];
    const char *error = NULL;
    if (result->cleanup != ETUDE_CLEANUP_CONFIRMED) {
        h->uncertain = 1;
        error = "the check's process cleanup is unconfirmed";
    } else if (result->output_error)
        error = "the complete output could not be saved";
    else if (result->end != ETUDE_END_EXITED)
        error = stopped ? "the check was interrupted" : "the check did not exit normally";
    else if (result->value) {
        snprintf(why, sizeof(why), "the check exited with status %d", result->value);
        error = why;
    } else
        error = prepared(h, c);
    c->interrupted = stopped && result->end == ETUDE_END_STOPPED;
    finished(h, c, error);
}

static int execute(struct host *h, unsigned width)
{
    size_t next = 0, running = 0;
    int alone = 0;
    while (next < h->nchecks || running) {
        while (!stopped && next < h->nchecks && running < width && !alone) {
            struct check *c = &h->checks[next];
            if (!c->selected) {
                next++;
                continue;
            }
            if (c->alone && running)
                break;
            next++;
            const char *why = prepared(h, c);
            if (why) {
                finished(h, c, why);
                continue;
            }
            if (start(h, c) != 0) {
                finished(h, c, "the prepared check could not start");
                continue;
            }
            running++;
            alone = c->alone;
        }
        if (stopped) {
            for (size_t i = 0; i < h->nchecks; i++)
                if (h->checks[i].proc)
                    etude_exec_cancel(h->checks[i].proc);
            if (!running)
                break;
        }
        if (!running)
            continue;
        struct etude_proc *p = etude_exec_wait(h->executor, 200);
        if (!p)
            continue;
        for (size_t i = 0; i < h->nchecks; i++) {
            struct check *c = &h->checks[i];
            if (c->proc != p)
                continue;
            finish_process(h, c, p);
            alone = 0;
            running--;
            etude_proc_free(c->proc);
            c->proc = NULL;
            break;
        }
    }
    return stopped ? 128 + stopped : h->failures ? 1 : 0;
}

/* The legacy scripts driver executes the union; scenarios own partitions. */
static int script_declarations(struct host *h, const struct json *cases)
{
    static const char *ids[] = {"host.host.scripts", "host.host.scripts_native",
                                "host.host.scripts_images", "host.host.scripts_build",
                                "host.host.scripts_tools"};
    for (size_t i = 0; cases && i < cases->count; i++) {
        const char *id = json_text(cases->item[i], "case_id");
        if (!id || strncmp(id, "host.host.scripts", 17))
            continue;
        size_t j;
        for (j = 0; j < 5 && strcmp(id, ids[j]); j++) {
        }
        if (j == 5 || h->script_parts[j])
            return -1;
        h->script_parts[j] = json_member(cases->item[i], "assertions");
        if (!h->script_parts[j] || h->script_parts[j]->type != JSON_ARRAY)
            return -1;
        h->nscript_parts++;
    }
    if (h->nscript_parts == 1 && h->script_parts[0])
        h->nscript_parts = 0;
    else if (h->nscript_parts != 5)
        return -1;
    return 0;
}

static int load_declarations(struct host *h, const char *group)
{
    const struct json *cases = json_member(h->manifest, "cases");
    int etude = !strcmp(canonical_group(group), "etude");
    const char *other = !strcmp(group, "etude_quiet") ? "host.host.etude" : "host.host.etude_quiet";
    for (size_t i = 0; cases && i < cases->count; i++) {
        const char *id = json_text(cases->item[i], "case_id");
        if (id && !strcmp(id, h->case_id))
            h->declared = json_member(cases->item[i], "assertions");
        if (etude && id && !strcmp(id, other))
            h->companion = json_member(cases->item[i], "assertions");
    }
    if (!strcmp(group, "scripts") && script_declarations(h, cases) != 0)
        return -1;
    if (!h->declared || h->declared->type != JSON_ARRAY)
        return -1;
    if (etude && (!h->companion || h->companion->type != JSON_ARRAY))
        return -1;
    if (h->only && declaration(h->companion, h->only) && !declared(h, h->only)) {
        const struct json *swap = h->declared;
        h->declared = h->companion;
        h->companion = swap;
        snprintf(h->case_id, sizeof(h->case_id), "%s", other);
        if (getenv("ETUDE_CASE") && strcmp(getenv("ETUDE_CASE"), h->case_id))
            return -1;
    }
    return 0;
}

static int load_inventory(struct host *h, const char *group, const char *arch)
{
    char manifest[PATH_SIZE], inventory[PATH_SIZE], digest[65], node[160];
    if (path_of(manifest, h->tree, "tests/cases/host.host.json") != 0)
        return -1;
    h->manifest = document(manifest);
    if (load_declarations(h, group) != 0)
        return -1;
    group = canonical_group(group);
    snprintf(node, sizeof(node), "%s/tests/host-suite/%s.inventory.json",
             !strcmp(arch, "x86_64") && (!strcmp(group, "tests") || !strcmp(group, "boots"))
                 ? "build/x86_64"
                 : "build",
             group);
    if (path_of(inventory, h->tree, node) != 0)
        return -1;
    h->inventory = document(inventory);
    const char *inventory_group = json_text(h->inventory, "group");
    const struct json *version = json_member(h->inventory, "schema_version");
    if (!inventory_group || strcmp(inventory_group, group) || !version ||
        version->type != JSON_INTEGER || version->integer != 1)
        return -1;
    const char *hash = json_text(h->inventory, "manifest_sha256");
    if (!hash || file_digest(manifest, digest) != 0 || strcmp(hash, digest)) {
        fprintf(stderr,
                "host: the prepared inventory is missing or belongs to another manifest: %s\n",
                inventory);
        return -1;
    }
    const char *prepared_arch = json_text(h->inventory, "architecture");
    const char *expected_arch = !strcmp(group, "tests") || !strcmp(group, "boots") ? arch : "none";
    if (!prepared_arch || strcmp(prepared_arch, expected_arch))
        return -1;
    return 0;
}

static void release_host(struct host *h, int status)
{
    if (h->executor)
        etude_exec_free(h->executor);
    for (size_t i = 0; i < h->nchecks; i++) {
        free(h->checks[i].argv);
        if (h->checks[i].env) {
            for (size_t j = 0; h->checks[i].env[j]; j++)
                free(h->checks[i].env[j]);
            free(h->checks[i].env);
        }
    }
    if (h->work[0]) {
        if ((!status || stopped) && !h->uncertain)
            remove_tree(h->work);
        else
            printf("host: evidence: %s\n", h->work);
    }
    free(h->checks);
    json_free(h->manifest);
    json_free(h->inventory);
}

static int valid_group(const char *group)
{
    static const char *const groups[] = {"tests",    "etude", "etude_quiet", "boots",
                                         "checkers", "fuzz",  "scripts"};
    for (size_t i = 0; i < sizeof groups / sizeof groups[0]; i++)
        if (!strcmp(group, groups[i]))
            return 1;
    return 0;
}

static int host_options(struct host *h, int argc, char **argv, const char **selected_group,
                        const char **selected_arch, unsigned *width)
{
    const char *tree = getenv("ETUDE_TREE");
    const char *group = "", *arch = getenv("ARCH"), *jobs = getenv("RACH2_WORKERS");
    json_program = "host";
    if (argc == 2)
        group = argv[1];
    else if (argc == 4 || argc == 5) {
        if (strcmp(argv[1], "--one"))
            return 2;
        group = argv[2];
        h->only = argv[3];
        if (argc == 5)
            arch = argv[4];
    }
    if (!valid_group(group)) {
        fputs(
            "usage: host [--one] tests|etude|etude_quiet|boots|checkers|fuzz|scripts [ASSERTION]\n",
            stderr);
        return 2;
    }
    if (!tree)
        tree = ".";
    if (!realpath(tree, h->tree))
        return 2;
    if (!arch || !*arch)
        arch = "i686";
    if (strcmp(arch, "i686") && strcmp(arch, "x86_64"))
        return 2;
    snprintf(h->case_id, sizeof(h->case_id), "host.host.%s", group);
    if (getenv("ETUDE_CASE") && strcmp(getenv("ETUDE_CASE"), h->case_id))
        return 2;
    if (jobs) {
        char *end;
        unsigned long n = strtoul(jobs, &end, 10);
        if (*end || !n || n > CHECKS_MAX)
            return 2;
        *width = (unsigned)n;
    }
    *selected_group = group;
    *selected_arch = arch;
    return 0;
}

int main(int argc, char **argv)
{
    struct host h;
    const char *group, *arch, *tmp = getenv("HOST_TEST_WORKDIR");
    unsigned width = 4;
    int rc = 2;
    memset(&h, 0, sizeof h);
    if (host_options(&h, argc, argv, &group, &arch, &width) != 0)
        return 2;
    if (!tmp || !*tmp)
        tmp = getenv("TMPDIR");
    if (load_inventory(&h, group, arch) != 0) {
        fputs("host: missing, invalid or stale prepared inventory; prepare the host suite again\n",
              stderr);
        goto done;
    }
#ifdef _WIN32
    if (!tmp)
        tmp = etude_win_temp();
#else
    if (!tmp)
        tmp = "/tmp";
#endif
    snprintf(h.work, sizeof(h.work), "%s/rach2-host.XXXXXX", tmp);
    if (!mkdtemp(h.work)) {
        h.work[0] = 0;
        goto done;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifndef _WIN32
    signal(SIGHUP, on_signal);
#endif
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("host: logs: %s\n@etude 1 begin %s\n", h.work, h.case_id);
    h.executor = etude_exec_new();
    if (!h.executor || check_inventory(&h, group) != 0)
        goto done;
    rc = execute(&h, width);
    if (!stopped)
        printf("@etude 1 end %lu\nhost: result: %s: %s\n", (unsigned long)h.records,
               rc ? "failed" : "passed", h.case_id);
    else
        printf("host: result: stopped by signal %d: %lu of %lu checks finished\n", (int)stopped,
               (unsigned long)h.reported, (unsigned long)h.nchecks);
done:
    release_host(&h, rc);
    return stopped ? 128 + stopped : rc;
}
