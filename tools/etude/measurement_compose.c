/* Compose hash-bound measurement inputs with the native context reader. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#endif
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "history_json.h"
#include "measurement_context.h"
#include "routine_proof.h"
#include "sha256.h"

#define COMPOSE_PATH 8192
#define COMPOSE_MAX  65536

enum compose_option {
    C_TREE,
    C_RECORD,
    C_SITE,
    C_ETUDE,
    C_SOURCE,
    C_OUTPUT,
    C_COMMIT,
    C_RUN,
    C_SOURCE_HASH,
    C_METHOD,
    C_ORDINARY,
    C_ONCE,
    C_PURPOSE,
    C_CACHE,
    C_PREPARATION,
    C_OUTER,
    C_OUTER_STOP,
    C_PHYSICAL,
    C_PHYSICAL_STOP,
    C_SELECTION,
    C_GRANT,
    C_UPSTREAM,
    C_PROOFS,
    C_COUNT
};
static const char *const compose_flags[] = {"--tree",
                                            "--record",
                                            "--site",
                                            "--etude",
                                            "--source-snapshot",
                                            "--output",
                                            "--commit",
                                            "--run-id",
                                            "--source-sha256",
                                            "--cache-method",
                                            "--ordinary-preparation",
                                            "--one-time-production",
                                            "--purpose",
                                            "--cache-protocol",
                                            "--preparation",
                                            "--outer-trace",
                                            "--outer-stop-request",
                                            "--physical-trace",
                                            "--physical-stop-request",
                                            "--selection-export",
                                            "--routine-grant",
                                            "--upstream-preparation",
                                            "--routine-proofs"};
struct compose {
    const char *value[C_COUNT];
    char **tools;
    size_t count;
    char *tree, *output, *program, *record, *site;
    struct json *source, *receipt;
    struct jbuf context;
};

static int compose_hex(const char *text, size_t size)
{
    return text && strlen(text) == size && strspn(text, "0123456789abcdef") == size;
}
static int compose_text(const struct json *doc, const char *key, const char *expected)
{
    const char *got = json_text(doc, key);
    return got && expected && !strcmp(got, expected);
}
static int compose_version(const struct json *doc)
{
    const struct json *version = json_member(doc, "schema_version");
    return version && version->type == JSON_INTEGER && version->integer == 1;
}
static int compose_number(const struct json *doc, const char *key, uint64_t *value)
{
    const struct json *number = json_member(doc, key);
    if (!number || number->type != JSON_INTEGER || number->integer < 0)
        return -1;
    *value = (uint64_t)number->integer;
    return 0;
}
static int compose_order(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}
static int compose_tool(struct compose *c, const char *path)
{
    char *resolved = realpath(path, NULL);
    if (!resolved || c->count == COMPOSE_MAX) {
        free(resolved);
        return -1;
    }
    char **more = realloc(c->tools, (c->count + 1) * sizeof(*more));
    if (!more) {
        free(resolved);
        return -1;
    }
    c->tools = more;
    c->tools[c->count++] = resolved;
    return 0;
}
static int compose_options(struct compose *c, int argc, char **argv)
{
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc)
            return -1;
        if (!strcmp(argv[i], "--tool")) {
            if (compose_tool(c, argv[i + 1]))
                return -1;
            continue;
        }
        size_t k = 0;
        while (k < C_COUNT && strcmp(argv[i], compose_flags[k]))
            k++;
        if (k == C_COUNT || c->value[k])
            return -1;
        c->value[k] = argv[i + 1];
    }
    for (size_t i = 0; i <= C_PURPOSE; i++)
        if (!c->value[i])
            return -1;
    return !compose_hex(c->value[C_COMMIT], 40) || !eh_identity(c->value[C_RUN]) ||
                   !compose_hex(c->value[C_SOURCE_HASH], 64)
               ? -1
               : 0;
}
static int compose_relative(const char *path)
{
    if (!path || !*path || *path == '/' || strchr(path, '\\') || strchr(path, ':'))
        return 0;
    for (;;) {
        size_t n = strcspn(path, "/");
        if (!n || (n == 1 && path[0] == '.') || (n == 2 && !strncmp(path, "..", 2)))
            return 0;
        path += n;
        if (!*path)
            return 1;
        path++;
    }
}
static int compose_match(const char *path, const struct json *ref)
{
    struct stat st;
    char digest[65];
    uint64_t size;
    return compose_number(ref, "size_bytes", &size) || etude_context_digest(path, &st, digest) ||
                   size != (uint64_t)st.st_size || !compose_text(ref, "sha256", digest)
               ? -1
               : 0;
}
static int compose_source(struct compose *c)
{
    const char *path = c->value[C_SOURCE];
    char digest[65];
    struct stat st;
    if (etude_context_digest(path, &st, digest) || strcmp(digest, c->value[C_SOURCE_HASH]) ||
        routine_json(path, &c->source) || !compose_version(c->source) ||
        !compose_text(c->source, "commit", c->value[C_COMMIT]))
        return -1;
    const struct json *inputs = json_member(c->source, "inputs");
    if (!inputs || inputs->type != JSON_ARRAY || !inputs->count || inputs->count > COMPOSE_MAX)
        return -1;
    for (size_t i = 0; i < inputs->count; i++) {
        const char *name = json_text(inputs->item[i], "path");
        char full[COMPOSE_PATH];
        if (!compose_relative(name))
            return -1;
        for (size_t j = 0; j < i; j++)
            if (compose_text(inputs->item[j], "path", name))
                return -1;
        int n = snprintf(full, sizeof full, "%s/%s", c->tree, name);
        if (n < 0 || (size_t)n >= sizeof full || compose_match(full, inputs->item[i]))
            return -1;
    }
    return 0;
}
/* Resolve existing ancestors while retaining an absent observer stop path. */
static char *compose_future(const char *given)
{
    char *existing = realpath(given, NULL);
    if (existing || errno != ENOENT || strlen(given) >= COMPOSE_PATH)
        return existing;
    char *copy = strdup(given), *result = NULL;
    if (!copy)
        return NULL;
    for (char *p = copy; *p; p++)
        if (*p == '\\')
            *p = '/';
    char *leaf = strrchr(copy, '/');
    const char *name = leaf ? leaf + 1 : copy;
    if (!*name)
        goto done;
    char root[4];
    if (leaf)
        *leaf = 0;
    const char *directory = leaf ? (*copy ? copy : "/") : ".";
    if (leaf && strlen(copy) == 2 && copy[1] == ':') {
        snprintf(root, sizeof root, "%s/", copy);
        directory = root;
    }
    char *parent = compose_future(directory);
    if (parent) {
        if (!strcmp(name, "."))
            result = strdup(parent);
        else if (!strcmp(name, "..")) {
            char *end = strrchr(parent, '/');
            if (end == parent)
                parent[1] = 0;
            else if (end && end[-1] != ':')
                *end = 0;
            result = strdup(parent);
        } else {
            size_t size = strlen(parent) + strlen(name) + 2;
            result = malloc(size);
            if (result)
                snprintf(result, size, "%s%s%s", parent,
                         parent[strlen(parent) - 1] == '/' ? "" : "/", name);
        }
    }
    free(parent);
done:
    free(copy);
    return result;
}
static int compose_directory(struct compose *c)
{
    char *copy = strdup(c->value[C_OUTPUT]);
    if (!copy)
        return -1;
    for (char *p = copy; *p; p++)
        if (*p == '\\')
            *p = '/';
    size_t length = strlen(copy);
    while (length > 1 && copy[length - 1] == '/')
        copy[--length] = 0;
    int error = !length;
    size_t first = 1;
    if (copy[0] == '/' && copy[1] == '/') {
        const char *server = strchr(copy + 2, '/');
        const char *share = server ? strchr(server + 1, '/') : NULL;
        first = share ? (size_t)(share - copy) + 1 : strlen(copy);
    }
    for (size_t i = first; !error && copy[i]; i++) {
        if (copy[i] != '/' || (i == 2 && copy[1] == ':'))
            continue;
        copy[i] = 0;
        struct stat st;
        error =
            (mkdir(copy, 0700) != 0 && errno != EEXIST) || stat(copy, &st) || !S_ISDIR(st.st_mode);
        copy[i] = '/';
    }
    if (!error)
        error = mkdir(copy, 0700);
    if (!error)
        c->output = realpath(copy, NULL);
    free(copy);
    return error || !c->output ? -1 : 0;
}
static void compose_key(struct jbuf *b, const char *key)
{
    jbuf_puts(b, ",");
    routine_quote(b, key);
    jbuf_puts(b, ":");
}
static int compose_ref(struct jbuf *b, const char *path, int locations)
{
    struct stat st;
    char hash[65], numbers[180];
    char *resolved = realpath(path, NULL);
    int error = !resolved || etude_context_digest(resolved, &st, hash);
    if (!error) {
        snprintf(numbers, sizeof numbers, "{\"sha256\":\"%s\",\"size_bytes\":%llu,", hash,
                 (unsigned long long)st.st_size);
        jbuf_puts(b, numbers);
        jbuf_puts(b, locations ? "\"locations\":[" : "\"path\":");
        routine_quote(b, resolved);
        jbuf_puts(b, locations ? "]}" : "}");
    }
    free(resolved);
    return error ? -1 : 0;
}
static int compose_write(struct compose *c, const char *name, struct jbuf *bytes,
                         struct jbuf *reference)
{
    char path[COMPOSE_PATH], error[160];
    struct json *doc = NULL;
    struct jbuf canonical = {0};
    int n = snprintf(path, sizeof path, "%s/%s.json", c->output, name);
    int rc = n < 0 || (size_t)n >= sizeof path ||
             json_parse(bytes->data, bytes->len, 1, &doc, error, sizeof error);
    if (!rc) {
        json_canonical(doc, &canonical);
        jbuf_puts(&canonical, "\n");
        rc = routine_create(path, &canonical) || compose_ref(reference, path, 0);
    }
    json_free(doc);
    jbuf_free(&canonical);
    return rc ? -1 : 0;
}
static int compose_snapshot(struct compose *c, const char *key, const char *name,
                            struct jbuf *bytes)
{
    compose_key(&c->context, key);
    return compose_write(c, name, bytes, &c->context);
}
static int compose_basics(struct compose *c)
{
    struct jbuf b = {0};
    jbuf_puts(&c->context, "{\"format\":\"etude-measurement-context\",\"schema_version\":1");
    for (size_t i = C_COMMIT; i <= C_RUN; i++) {
        compose_key(&c->context, i == C_COMMIT ? "commit" : "run_id");
        routine_quote(&c->context, c->value[i]);
    }
    compose_key(&c->context, "purpose");
    routine_quote(&c->context, c->value[C_PURPOSE]);
    json_canonical(c->source, &b);
    int rc = compose_snapshot(c, "source", "source", &b);
    jbuf_free(&b);
    qsort(c->tools, c->count, sizeof(*c->tools), compose_order);
    jbuf_puts(&b, "{\"schema_version\":1,\"inputs\":[");
    for (size_t i = 0, written = 0; !rc && i < c->count; i++) {
        if (i && !strcmp(c->tools[i - 1], c->tools[i]))
            continue;
        if (written++)
            jbuf_puts(&b, ",");
        rc = compose_ref(&b, c->tools[i], 0);
    }
    jbuf_puts(&b, "]}");
    if (!rc)
        rc = compose_snapshot(c, "tools", "tools", &b);
    jbuf_free(&b);
    jbuf_puts(&b, "{\"schema_version\":1,\"policy\":");
    if (!rc)
        rc = compose_ref(&b, c->site, 0);
    jbuf_puts(&b, "}");
    if (!rc)
        rc = compose_snapshot(c, "site_profile", "site", &b);
    jbuf_free(&b);
    compose_key(&c->context, "artifacts");
    return rc || compose_ref(&c->context, c->record, 0) ? -1 : 0;
}

static int compose_preparation(struct compose *c)
{
    compose_key(&c->context, "preparation");
    if (!c->value[C_PREPARATION]) {
        jbuf_puts(&c->context, "null");
        return 0;
    }
    char hash[65];
    struct stat st;
    if (routine_json(c->value[C_PREPARATION], &c->receipt) ||
        etude_context_digest(c->record, &st, hash) || !compose_version(c->receipt) ||
        !compose_text(c->receipt, "format", "etude-preparation-receipt") ||
        !compose_text(c->receipt, "outcome", "pass") ||
        !compose_text(c->receipt, "cleanup", "confirmed") ||
        !compose_text(c->receipt, "run_id", c->value[C_RUN]) ||
        !compose_text(c->receipt, "commit", c->value[C_COMMIT]) ||
        !compose_text(c->receipt, "record_sha256", hash))
        return -1;
    return compose_ref(&c->context, c->value[C_PREPARATION], 0);
}
static int compose_states(const struct json *states, int *controlled)
{
    static const char *const names[] = {"artifacts", "build", "tools", "os"};
    if (!states || states->type != JSON_OBJECT || states->count != 4)
        return -1;
    *controlled = 1;
    for (size_t i = 0; i < 4; i++) {
        const struct json *value = json_member(states, names[i]);
        const char *state = json_text(value, "state"), *procedure = json_text(value, "procedure");
        if (!value || value->type != JSON_OBJECT || value->count != 2 || !state || !procedure ||
            strspn(procedure, " \t\r\n\v\f") == strlen(procedure) ||
            (strcmp(state, "cold") && strcmp(state, "warm") && strcmp(state, "unknown")))
            return -1;
        if (!strcmp(state, "unknown"))
            *controlled = 0;
    }
    return 0;
}
static int compose_absolute(const char *path)
{
#ifdef _WIN32
    return etude_win_absolute(path);
#else
    return path && path[0] == '/';
#endif
}
static int compose_evidence(const struct json *evidence)
{
    if (!evidence || evidence->type != JSON_ARRAY || !evidence->count || evidence->count > 16)
        return -1;
    for (size_t i = 0; i < evidence->count; i++) {
        const struct json *ref = evidence->item[i], *locations = json_member(ref, "locations");
        if (!locations || locations->type != JSON_ARRAY || !locations->count)
            return -1;
        int found = 0;
        for (size_t j = 0; j < locations->count; j++) {
            const struct json *path = locations->item[j];
            if (path->type != JSON_STRING || !compose_absolute(path->string))
                return -1;
            if (!compose_match(path->string, ref))
                found = 1;
        }
        if (!found)
            return -1;
    }
    return 0;
}
static int compose_cache(struct compose *c)
{
    struct json *protocol = NULL;
    struct jbuf b = {0};
    int rc = 0, controlled = 0;
    const struct json *states = NULL;
    compose_key(&c->context, "cache_evidence");
    if (c->value[C_CACHE]) {
        rc = !c->receipt || routine_json(c->value[C_CACHE], &protocol) ||
             !compose_version(protocol) ||
             !compose_text(protocol, "format", "etude-cache-protocol");
        static const char *const keys[] = {"run_id", "commit", "machine_id", "boot_id"};
        for (size_t i = 0; !rc && i < 4; i++)
            rc = !compose_text(protocol, keys[i], json_text(c->receipt, keys[i]));
        states = json_member(protocol, "states");
        const struct json *evidence = json_member(protocol, "evidence");
        if (!rc)
            rc = compose_states(states, &controlled) || compose_evidence(evidence);
        if (!rc) {
            json_canonical(protocol, &b);
            rc = compose_write(c, "cache-evidence", &b, &c->context);
        }
        jbuf_free(&b);
    } else
        jbuf_puts(&c->context, "null");
    jbuf_puts(&b, "{\"schema_version\":1,\"states\":");
    if (states)
        json_canonical(states, &b);
    else
        jbuf_puts(&b,
                  "{\"artifacts\":{\"state\":\"unknown\",\"procedure\":\"No cache conditioning "
                  "was recorded.\"},\"build\":{\"state\":\"unknown\",\"procedure\":\"No cache "
                  "conditioning was recorded.\"},\"tools\":{\"state\":\"unknown\",\"procedure\":"
                  "\"No cache conditioning was recorded.\"},\"os\":{\"state\":\"unknown\","
                  "\"procedure\":\"No cache conditioning was recorded.\"}}");
    jbuf_puts(&b, controlled ? ",\"controlled\":true" : ",\"controlled\":false");
    static const char *const keys[] = {"method", "ordinary_preparation", "one_time_production"};
    for (size_t i = 0; i < 3; i++) {
        compose_key(&b, keys[i]);
        routine_quote(&b, c->value[C_METHOD + i]);
    }
    jbuf_puts(&b, "}");
    if (!rc)
        rc = compose_snapshot(c, "cache_state", "cache", &b);
    jbuf_free(&b);
    json_free(protocol);
    return rc ? -1 : 0;
}
static void compose_hash(const char *bytes, size_t size, char hash[65])
{
    struct sha256 sha;
    unsigned char sum[32];
    sha256_init(&sha);
    sha256_update(&sha, (const unsigned char *)bytes, size);
    sha256_final(&sha, sum);
    sha256_hex(sum, hash);
}
static int compose_observation(struct compose *c, int physical)
{
    enum compose_option trace = physical ? C_PHYSICAL : C_OUTER;
    compose_key(&c->context, physical ? "physical_observation" : "outer_observation");
    if (!c->value[trace]) {
        jbuf_puts(&c->context, "null");
        return c->value[trace + 1] ? -1 : 0;
    }
    struct stat st;
    if (!c->value[trace + 1] || !lstat(c->value[trace + 1], &st) || errno != ENOENT)
        return -1;
    char *path = realpath(c->value[trace], NULL), *stop = compose_future(c->value[trace + 1]);
    FILE *file = path ? fopen(path, "rb") : NULL;
    struct json *header = NULL;
    char line[65536], error[160], hash[65];
    int rc = !stop || !file;
    if (!rc)
        rc = fstat(fileno(file), &st) || !S_ISREG(st.st_mode) || !fgets(line, sizeof line, file);
    size_t size = rc ? 0 : strlen(line);
    if (!rc)
        rc = !size || line[size - 1] != '\n' ||
             json_parse(line, size, 1, &header, error, sizeof error) || !compose_version(header) ||
             !compose_text(header, "kind", "start") ||
             !compose_text(header, "method", "native-load-v1") ||
             !compose_text(header, "run_id", c->value[C_RUN]);
    if (file && fclose(file))
        rc = 1;
    if (!rc) {
        compose_hash(line, size, hash);
        jbuf_puts(&c->context, "{\"trace\":");
        routine_quote(&c->context, path);
        jbuf_puts(&c->context, ",\"stop_request\":");
        routine_quote(&c->context, stop);
        jbuf_puts(&c->context, ",\"header_sha256\":");
        routine_quote(&c->context, hash);
        jbuf_puts(&c->context, "}");
    }
    json_free(header);
    free(path);
    free(stop);
    return rc ? -1 : 0;
}
static int compose_routine(struct compose *c)
{
    if (c->value[C_UPSTREAM] && !c->value[C_GRANT])
        return -1;
    static const char *const keys[] = {"routine_grant", "upstream_preparation"};
    for (size_t i = 0; i < 2; i++) {
        const char *path = c->value[C_GRANT + i];
        if (!path)
            continue;
        struct json *doc = NULL;
        int rc = routine_json(path, &doc) || !compose_text(doc, "run_id", c->value[C_RUN]) ||
                 !compose_text(doc, "commit", c->value[C_COMMIT]);
        json_free(doc);
        compose_key(&c->context, keys[i]);
        if (rc || compose_ref(&c->context, path, 1))
            return -1;
    }
    if (c->value[C_PROOFS]) {
        struct stat st;
        char *path = realpath(c->value[C_PROOFS], NULL);
        int rc = !c->value[C_GRANT] || !path || stat(path, &st) || !S_ISDIR(st.st_mode);
        if (!rc) {
            compose_key(&c->context, "routine_proofs");
            routine_quote(&c->context, path);
        }
        free(path);
        if (rc)
            return -1;
    }
    return 0;
}

static int compose_selection(struct compose *c, struct json **out, struct jbuf *manifest)
{
    struct json *export = NULL, *selection = NULL;
    const char *given = c->value[C_SELECTION];
    char hash[65], error[160], path[COMPOSE_PATH];
    uint64_t size;
    int rc = !given || routine_json(given, &export);
    const char *raw = json_text(export, "selection_bytes");
    if (!rc)
        rc = !raw || compose_number(export, "selection_size_bytes", &size) || strlen(raw) != size;
    if (!rc) {
        compose_hash(raw, (size_t)size, hash);
        rc = !compose_text(export, "selection_sha256", hash);
        for (size_t i = 0; !rc && i < size; i++)
            rc = (unsigned char)raw[i] > 127;
    }
    if (!rc)
        rc = json_parse(raw, (size_t)size, 1, &selection, error, sizeof error);
    if (!rc) {
        int n = snprintf(path, sizeof path, "%s/selection.json", c->output);
        struct jbuf bytes = {0};
        jbuf_put(&bytes, raw, (size_t)size);
        rc = n < 0 || (size_t)n >= sizeof path || routine_create(path, &bytes) ||
             compose_ref(manifest, path, 1);
        jbuf_free(&bytes);
    }
    json_free(selection);
    if (rc)
        json_free(export);
    else
        *out = export;
    return rc ? -1 : 0;
}
static int compose_attempts(struct compose *c, const struct json *export, struct jbuf *attempts)
{
    const struct json *items = json_member(export, "items");
    if (!items || items->type != JSON_ARRAY || !items->count || items->count > COMPOSE_MAX)
        return -1;
    char **seen = calloc(items->count, sizeof(*seen));
    int rc = !seen;
    jbuf_puts(attempts, "[");
    for (size_t i = 0; !rc && i < items->count; i++) {
        const struct json *item = items->item[i];
        const char *arch = json_text(item, "architecture"), *name = json_text(item, "case_id");
        uint64_t repetition;
        char identity[256];
        if (!arch || !name ||
            (strcmp(arch, "i686") && strcmp(arch, "x86_64") && strcmp(arch, "none")) ||
            compose_number(item, "repetition", &repetition)) {
            rc = 1;
            break;
        }
        int n = snprintf(identity, sizeof identity, "%s:%s:%s:r%llu:a0", c->value[C_RUN], name,
                         arch, (unsigned long long)repetition);
        if (n < 0 || (size_t)n >= sizeof identity || !eh_identity(identity) ||
            !(seen[i] = strdup(identity))) {
            rc = 1;
            break;
        }
        if (i)
            jbuf_puts(attempts, ",");
        routine_quote(attempts, identity);
    }
    jbuf_puts(attempts, "]");
    if (!rc) {
        qsort(seen, items->count, sizeof(*seen), compose_order);
        for (size_t i = 1; i < items->count; i++)
            if (!strcmp(seen[i - 1], seen[i]))
                rc = 1;
    }
    if (seen)
        for (size_t i = 0; i < items->count; i++)
            free(seen[i]);
    free(seen);
    return rc ? -1 : 0;
}
static int compose_exemption(struct compose *c)
{
    compose_key(&c->context, "exemption");
    if (strcmp(c->value[C_PURPOSE], "phase_first_run")) {
        jbuf_puts(&c->context, "null");
        return c->value[C_SELECTION] ? -1 : 0;
    }
    struct json *export = NULL;
    struct jbuf manifest = {0}, attempts = {0};
    int rc = compose_selection(c, &export, &manifest) || compose_attempts(c, export, &attempts);
    if (!rc) {
        jbuf_puts(&c->context,
                  "{\"reason\":\"milestone-62-first-runs\",\"phase_issue\":589,\"manifest\":");
        jbuf_put(&c->context, manifest.data, manifest.len);
        jbuf_puts(&c->context, ",\"run_ids\":[");
        routine_quote(&c->context, c->value[C_RUN]);
        jbuf_puts(&c->context, "],\"attempt_ids\":");
        jbuf_put(&c->context, attempts.data, attempts.len);
        jbuf_puts(&c->context, "}");
    }
    json_free(export);
    jbuf_free(&manifest);
    jbuf_free(&attempts);
    return rc ? -1 : 0;
}
static int compose_finish(struct compose *c)
{
    struct jbuf result = {0};
    jbuf_puts(&c->context, "}");
    jbuf_puts(&result, "{\"context\":");
    int rc = compose_write(c, "context", &c->context, &result);
    char path[COMPOSE_PATH], error[160];
    struct etude_measurement_context *context = NULL;
    const struct etude_context_inputs inputs = {c->tree, c->record, c->site, c->value[C_COMMIT],
                                                c->program};
    int n = snprintf(path, sizeof path, "%s/context.json", c->output);
    if (!rc)
        rc = n < 0 || (size_t)n >= sizeof path ||
             etude_context_open(path, &inputs, &context, error, sizeof error);
    etude_context_close(context);
    if (!rc) {
        jbuf_puts(&result, ",\"run_id\":");
        routine_quote(&result, c->value[C_RUN]);
        jbuf_puts(&result, ",\"runtime\":[\"etude\",\"run\",\"-I\",");
        routine_quote(&result, path);
        jbuf_puts(&result, ",\"-g\",");
        routine_quote(&result, c->value[C_COMMIT]);
        jbuf_puts(&result, "]}\n");
        rc = fwrite(result.data, 1, result.len, stdout) != result.len || fflush(stdout);
    }
    jbuf_free(&result);
    return rc ? -1 : 0;
}
static int compose_prepare(struct compose *c)
{
    c->tree = realpath(c->value[C_TREE], NULL);
    c->program = realpath(c->value[C_ETUDE], NULL);
    c->record = realpath(c->value[C_RECORD], NULL);
    c->site = realpath(c->value[C_SITE], NULL);
    if (!c->tree || !c->program || !c->record || !c->site || compose_tool(c, c->program) ||
        compose_source(c) || compose_directory(c))
        return -1;
    return compose_basics(c) || compose_preparation(c) || compose_cache(c) ||
                   compose_observation(c, 0) || compose_observation(c, 1) || compose_routine(c) ||
                   compose_exemption(c) || compose_finish(c)
               ? -1
               : 0;
}
int etude_prepare_context_main(int argc, char **argv)
{
    struct compose c = {0};
    int rc = compose_options(&c, argc, argv) || compose_prepare(&c);
    if (rc)
        fputs("etude prepare: measurement context refused or could not be written\n", stderr);
    for (size_t i = 0; i < c.count; i++)
        free(c.tools[i]);
    free(c.tools);
    free(c.tree);
    free(c.output);
    free(c.program);
    free(c.record);
    free(c.site);
    json_free(c.source);
    json_free(c.receipt);
    jbuf_free(&c.context);
    return rc ? 2 : 0;
}
