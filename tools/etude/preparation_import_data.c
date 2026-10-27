/* Bind original producer records and the native destination's actual files. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
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
#include "preparation_import.h"
#include "measurement_context.h"
#include "routine_proof.h"
#include "history_json.h"
#include "sha256.h"
#include "collection_tools.h"

int pi_same(const char *a, const char *b)
{
    return a && b && !strcmp(a, b);
}

int pi_hex(const char *text, size_t length)
{
    return text && strlen(text) == length && strspn(text, "0123456789abcdef") == length;
}

int pi_relative(const char *path)
{
    if (!path || !*path || *path == '/' || strpbrk(path, "\\:\n\r"))
        return 0;
    for (const char *p = path; *p;) {
        size_t n = strcspn(p, "/");
        if (n == 2 && !strncmp(p, "..", 2))
            return 0;
        p += n;
        if (*p)
            p++;
    }
    return 1;
}

char *pi_path(const char *tree, const char *name)
{
    struct jbuf path = {0};
    jbuf_puts(&path, tree);
    jbuf_puts(&path, "/");
    jbuf_puts(&path, name);
    return path.data;
}

struct json *pi_document(const char *path)
{
    char *text = hp_read(path), why[256];
    struct json *document = NULL;
    if (text && json_parse_data(text, strlen(text), 1, &document, why, sizeof(why)))
        fprintf(stderr, "etude prepare import: %s: %s\n", path, why);
    free(text);
    return document;
}

static int same_status(const struct stat *a, const struct stat *b)
{
#ifdef __APPLE__
    const struct timespec *am = &a->st_mtimespec, *bm = &b->st_mtimespec;
    const struct timespec *ac = &a->st_ctimespec, *bc = &b->st_ctimespec;
#else
    const struct timespec *am = &a->st_mtim, *bm = &b->st_mtim;
    const struct timespec *ac = &a->st_ctim, *bc = &b->st_ctim;
#endif
    return S_ISREG(a->st_mode) && S_ISREG(b->st_mode) && a->st_dev == b->st_dev &&
           a->st_ino == b->st_ino && a->st_size == b->st_size && am->tv_sec == bm->tv_sec &&
           am->tv_nsec == bm->tv_nsec && ac->tv_sec == bc->tv_sec && ac->tv_nsec == bc->tv_nsec;
}

struct json *pi_describe(const char *path)
{
    struct stat before, st, after;
    char digest[65];
    if (lstat(path, &before) || !S_ISREG(before.st_mode) ||
        etude_context_digest(path, &st, digest) || lstat(path, &after) ||
        !same_status(&before, &st) || !same_status(&st, &after))
        return NULL;
    struct json *value = hp_value(JSON_OBJECT);
    hp_member(value, "sha256", hp_string(digest));
    hp_member(value, "size_bytes", hp_number(st.st_size));
    return value;
}

int pi_verify(const char *path, const struct json *descriptor)
{
    struct json *current = pi_describe(path);
    int bad = !current || !descriptor || !json_equal(current, descriptor);
    json_free(current);
    if (bad)
        fprintf(stderr, "etude prepare import: changed input: %s\n", path);
    return bad ? -1 : 0;
}

int pi_create(const char *path, const struct json *value)
{
    struct jbuf bytes = {0};
    json_canonical(value, &bytes);
    jbuf_puts(&bytes, "\n");
    int bad = routine_create(path, &bytes);
    jbuf_free(&bytes);
    return bad;
}

/* Preserve the existing path-keyed source field, using the shared capture. */
struct json *pi_source(const char *tree, const char *commit)
{
    struct jbuf bytes = {0};
    struct json *snapshot = NULL, *source = hp_value(JSON_OBJECT);
    char why[256];
    int bad = etude_context_source_build(tree, commit, NULL, &bytes) ||
              json_parse(bytes.data, bytes.len, 1, &snapshot, why, sizeof(why));
    const struct json *inputs = json_member(snapshot, "inputs");
    if (!inputs || inputs->type != JSON_ARRAY)
        bad = 1;
    for (size_t i = 0; !bad && i < inputs->count; i++) {
        const char *name = json_text(inputs->item[i], "path");
        const struct json *size = json_member(inputs->item[i], "size_bytes");
        const char *digest = json_text(inputs->item[i], "sha256");
        if (!pi_relative(name) || !size || size->type != JSON_INTEGER || !pi_hex(digest, 64)) {
            bad = 1;
            break;
        }
        struct json *value = hp_value(JSON_OBJECT);
        hp_member(value, "sha256", hp_string(digest));
        hp_member(value, "size_bytes", hp_clone(size));
        hp_member(source, name, value);
    }
    jbuf_free(&bytes);
    json_free(snapshot);
    if (bad) {
        json_free(source);
        return NULL;
    }
    return source;
}

static int record_number(char **cursor, unsigned long long *value, int sign)
{
    char *at = *cursor, *end;
    if (sign && *at == '-')
        at++;
    if (!isdigit((unsigned char)*at))
        return -1;
    errno = 0;
    *value = strtoull(at, &end, 10);
    if (errno || *end != ' ')
        return -1;
    *cursor = end + 1;
    return 0;
}

static int record_line(struct json *files, char *line)
{
    if (!strncmp(line, "tree ", 5) || !strncmp(line, "arch ", 5) || !strncmp(line, "kernel ", 7))
        return 0;
    if (strncmp(line, "file ", 5) || strlen(line) < 70 || line[69] != ' ')
        return -1;
    char digest[65];
    memcpy(digest, line + 5, 64);
    digest[64] = 0;
    char *name = line + 70;
    unsigned long long size, ignored;
    if (!pi_hex(digest, 64) || record_number(&name, &size, 0) ||
        record_number(&name, &ignored, 0) || record_number(&name, &ignored, 1) ||
        size > JSON_INTEGER_MAX || !pi_relative(name) || json_member(files, name))
        return -1;
    struct json *value = hp_value(JSON_OBJECT);
    hp_member(value, "sha256", hp_string(digest));
    hp_member(value, "size_bytes", hp_number((long long)size));
    hp_member(files, name, value);
    return 0;
}

struct json *pi_record(const char *text)
{
    static const char header[] = "etude-prepare 1\n";
    if (!text || strncmp(text, header, sizeof(header) - 1))
        return NULL;
    struct json *files = hp_value(JSON_OBJECT);
    char *copy = strdup(text + sizeof(header) - 1), *line = copy;
    int bad = 0;
    while (line && *line && !bad) {
        char *end = strchr(line, '\n');
        if (end)
            *end++ = 0;
        bad = record_line(files, line);
        line = end;
    }
    free(copy);
    if (bad || !files->count) {
        json_free(files);
        return NULL;
    }
    return files;
}

const char *pi_role(const char *goal)
{
    size_t n = strlen(goal);
    if (strstr(goal, "/host-suite/") && n >= 15 && !strcmp(goal + n - 15, ".inventory.json"))
        return "inventory";
    return etude_collection_native_goal(goal) ? "native" : "portable";
}

static int case_goals(const struct json *row, struct json *goals)
{
    const struct json *arches = json_member(row, "architectures");
    const struct json *prepare = json_member(row, "prepare");
    int none = arches && arches->type == JSON_STRING && pi_same(arches->string, "none");
    if ((!none && (!arches || arches->type != JSON_ARRAY)) ||
        (prepare && prepare->type != JSON_ARRAY))
        return -1;
    for (size_t i = 0; prepare && i < prepare->count; i++) {
        const char *node = json_text(prepare->item[i], "node");
        const struct json *per_arch = json_member(prepare->item[i], "per_architecture");
        if (!pi_relative(node) || !per_arch || per_arch->type != JSON_BOOLEAN)
            return -1;
        for (size_t j = 0; j < (none ? 1 : arches->count); j++) {
            const char *arch = none ? "i686" : arches->item[j]->string;
            const char *base =
                per_arch->integer && pi_same(arch, "x86_64") ? "build/x86_64" : "build";
            char *goal = pi_path(base, node);
            hp_member(goals, goal, hp_value(JSON_NULL));
            free(goal);
        }
    }
    return 0;
}

struct json *pi_goals(const char *tree)
{
    char *path = pi_path(tree, "tests/cases");
    DIR *directory = opendir(path);
    free(path);
    struct json *goals = hp_value(JSON_OBJECT);
    struct dirent *entry;
    int bad = !directory;
    while (!bad && (entry = readdir(directory))) {
        size_t n = strlen(entry->d_name);
        if (n < 5 || strcmp(entry->d_name + n - 5, ".json"))
            continue;
        char *base = pi_path(tree, "tests/cases"), *name = pi_path(base, entry->d_name);
        struct json *manifest = hp_document(name);
        const struct json *cases = json_member(manifest, "cases");
        bad = !cases || cases->type != JSON_ARRAY;
        for (size_t i = 0; !bad && i < cases->count; i++)
            bad = case_goals(cases->item[i], goals);
        free(base);
        free(name);
        json_free(manifest);
    }
    if (directory)
        closedir(directory);
    if (bad) {
        json_free(goals);
        return NULL;
    }
    hp_member(goals, "build/rach2/kernel", hp_value(JSON_NULL));
    hp_member(goals, "build/x86_64/rach2/kernel", hp_value(JSON_NULL));
    return goals;
}

static int receipt_reading(const struct json *value, long long *reading)
{
    const char *utc = json_text(value, "utc");
    const struct json *mono = json_member(value, "monotonic_us");
    const struct json *resolution = json_member(value, "resolution_us");
    if (!utc || !eh_utc(utc) || !mono || mono->type != JSON_INTEGER || mono->integer < 0 ||
        !resolution || resolution->type != JSON_INTEGER || resolution->integer <= 0)
        return -1;
    *reading = mono->integer;
    return 0;
}

int pi_receipt(const struct json *receipt, const char *commit, const char *record)
{
    struct sha256 hash;
    unsigned char bytes[32];
    char digest[65];
    sha256_init(&hash);
    sha256_update(&hash, (const unsigned char *)record, strlen(record));
    sha256_final(&hash, bytes);
    sha256_hex(bytes, digest);
    const struct json *version = json_member(receipt, "schema_version");
    if (!pi_same(json_text(receipt, "format"), "etude-preparation-receipt") || !version ||
        version->type != JSON_INTEGER || version->integer != 1 ||
        !pi_same(json_text(receipt, "outcome"), "pass") ||
        !pi_same(json_text(receipt, "cleanup"), "confirmed") ||
        !pi_same(json_text(receipt, "commit"), commit) ||
        !pi_same(json_text(receipt, "record_sha256"), digest))
        return -1;
    const char *keys[] = {"run_id", "machine_id", "boot_id"};
    for (size_t i = 0; i < 3; i++) {
        const char *identity = json_text(receipt, keys[i]);
        if (!identity || strlen(identity) > (i ? 127 : 255) || !eh_identity(identity))
            return -1;
    }
    long long start, end;
    return receipt_reading(json_member(receipt, "start"), &start) ||
                   receipt_reading(json_member(receipt, "end"), &end) || end < start
               ? -1
               : 0;
}

int pi_clean(const struct json *receipt, const struct json *roles, const char *conditioning)
{
    if (!pi_same(conditioning, "clean"))
        return 0;
    int portable = 0;
    for (size_t i = 0; i < roles->count; i++)
        portable |= pi_same(roles->item[i]->string, "portable");
    if (!portable)
        return 0;
    const struct json *profile = json_member(receipt, "conditioning");
    const struct json *provenance = json_member(profile, "provenance");
    return !pi_same(json_text(profile, "inhouse"), "clean") ||
                   !pi_same(json_text(profile, "backend"), "ordinary") || !provenance ||
                   provenance->type != JSON_NULL
               ? -1
               : 0;
}

static int nested_check(const struct json *check, struct json *nested)
{
    const char *id = json_text(check, "assertion_id");
    int exfat = pi_same(id, "exfat");
    int alignment = pi_same(id, "e2fsprogs_alignment") || pi_same(id, "e2fsprogs_alignment64");
    if (!exfat && !alignment)
        return 0;
    const struct json *inputs = json_member(check, "inputs");
    if (!inputs || inputs->type != JSON_ARRAY)
        return -1;
    for (size_t i = 0; i < inputs->count; i++) {
        char decoded[4096];
        const char *path = hp_input_path(inputs->item[i], decoded);
        if (!pi_relative(path))
            return -1;
        size_t n = strlen(path);
        int independent =
            exfat && (pi_same(path, "build/tests/host/exfat_writer") ||
                      !strncmp(path, "build/tests/host-suite/tools/exfat_external/",
                               sizeof("build/tests/host-suite/tools/exfat_external/") - 1));
        int patched =
            alignment && n > 2 && path[n - 2] == '.' &&
            (path[n - 1] == 'c' || path[n - 1] == 'h') &&
            (!strncmp(path, "vendor/e2fsprogs/e2fsprogs-1.47.4/lib/",
                      sizeof("vendor/e2fsprogs/e2fsprogs-1.47.4/lib/") - 1) ||
             !strncmp(path, "build/x86_64/vendor-work/e2fsprogs/e2fsprogs-1.47.4/lib/",
                      sizeof("build/x86_64/vendor-work/e2fsprogs/e2fsprogs-1.47.4/lib/") - 1));
        if (!independent && !patched)
            continue;
        const char *digest = json_text(inputs->item[i], "sha256");
        const struct json *old = json_member(nested, path);
        if (!pi_hex(digest, 64) || (old && !pi_same(old->string, digest)))
            return -1;
        hp_member(nested, path, hp_string(digest));
    }
    return 0;
}

struct json *pi_nested(const struct json *inventories, const struct json *files,
                       const char *manifest_hash)
{
    struct json *nested = hp_value(JSON_OBJECT);
    if (!inventories || inventories->type != JSON_OBJECT)
        goto failed;
    for (size_t i = 0; i < inventories->count; i++) {
        const struct json *item = inventories->item[i], *anchor = json_member(files, item->name);
        const struct json *size = json_member(anchor, "size_bytes");
        const char *digest = json_text(anchor, "sha256");
        struct sha256 hash;
        unsigned char raw[32];
        char actual[65], why[256];
        struct json *inventory = NULL;
        if (item->type != JSON_STRING || !anchor || !size || size->type != JSON_INTEGER ||
            !pi_same(pi_role(item->name), "inventory"))
            goto failed;
        sha256_init(&hash);
        sha256_update(&hash, (const unsigned char *)item->string, strlen(item->string));
        sha256_final(&hash, raw);
        sha256_hex(raw, actual);
        if (!pi_same(digest, actual) || (size_t)size->integer != strlen(item->string) ||
            json_parse(item->string, strlen(item->string), 1, &inventory, why, sizeof(why)))
            goto failed;
        const struct json *checks = json_member(inventory, "checks");
        int bad = !pi_same(json_text(inventory, "manifest_sha256"), manifest_hash) || !checks ||
                  checks->type != JSON_ARRAY;
        for (size_t j = 0; !bad && j < checks->count; j++)
            bad = nested_check(checks->item[j], nested);
        json_free(inventory);
        if (bad)
            goto failed;
    }
    return nested;
failed:
    json_free(nested);
    return NULL;
}

int pi_native_binary(const char *path)
{
    unsigned char head[4];
    FILE *file = fopen(path, "rb");
    size_t n = file ? fread(head, 1, sizeof(head), file) : 0;
    if (file)
        fclose(file);
#ifdef _WIN32
    int native = n >= 2 && !memcmp(head, "MZ", 2);
#elif defined(__APPLE__)
    int native =
        n == 4 && (!memcmp(head, "\317\372\355\376", 4) || !memcmp(head, "\376\355\372\317", 4) ||
                   !memcmp(head, "\312\376\272\276", 4));
#else
    int native = n == 4 && !memcmp(head, "\177ELF", 4);
#endif
    return native ? 0 : -1;
}

int pi_native_data(const char *name)
{
    const char *style = strstr(name, "/tests/style/");
    return strstr(name, "/sanitizer/tools.json") != NULL ||
           (style && (pi_same(style + 13, "sparse.json") || pi_same(style + 13, "sizes.json") ||
                      pi_same(style + 13, "manual.json")));
}

static int native_style(struct pi_context *c, const char *name)
{
    const char *leaf = strrchr(name, '/');
    size_t n = leaf ? strlen(leaf + 1) : 0;
    if (n < 6 || n >= 80)
        return -1;
    char id[80];
    memcpy(id, leaf + 1, n - 5);
    id[n - 5] = 0;
    struct hp_words environment = {0};
    for (size_t i = 0; environ[i]; i++)
        if (strncmp(environ[i], "ARCH=", 5) && strncmp(environ[i], "ETUDE_TREE=", 11))
            hp_add(&environment, environ[i]);
    hp_add(&environment, !strncmp(name, "build/x86_64/", 13) || pi_same(c->arches, "x86_64")
                             ? "ARCH=x86_64"
                             : "ARCH=i686");
    struct jbuf tree = {0};
    jbuf_puts(&tree, "ETUDE_TREE=");
    jbuf_puts(&tree, c->tree);
    hp_add(&environment, tree.data);
    char *output = pi_path(c->work, "native-style.out"),
         *error = pi_path(c->work, "native-style.err");
    char *program = pi_path(c->tree, "build/tests/cases/host/style");
    char *argv[] = {program, id, NULL};
    char *const *saved = c->host.environment;
    c->host.environment = environment.v;
    int bad = hp_run(&c->host, argv, output, error);
    c->host.environment = saved;
    free(output);
    free(error);
    free(program);
    jbuf_free(&tree);
    hp_words_free(&environment);
    return bad;
}

static int input_digest(const char *tree, const char *name, const char *expected)
{
    if (!name || !expected)

        return -1;

    char *path = tree ? pi_path(tree, name) : strdup(name);
    struct json *actual = pi_describe(path);
    int bad = !actual || !pi_hex(expected, 64) || !pi_same(json_text(actual, "sha256"), expected);
    if (bad)
        fprintf(stderr, "etude prepare import: changed native input: %s\n", path);
    free(path);
    json_free(actual);
    return bad ? -1 : 0;
}

/*
 * Describe the prepared atos at `path`. On macOS preparation places a
 * symbolic link to the sealed system's atos instead of a copy
 * (host_prepare_tools.c), and only a link whose text is that program's
 * resolved path is described, by the program it names.
 */
static struct json *describe_atos(const char *path)
{
#ifdef __APPLE__
    struct stat st;
    char target[4096];
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode)) {
        char *native = realpath("/usr/bin/atos", NULL);
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n > 0)
            target[n] = 0;
        struct json *value = n > 0 && pi_same(target, native) ? pi_describe(target) : NULL;
        free(native);
        return value;
    }
#endif
    return pi_describe(path);
}

static int sanitizer_source(const char *tree, const char *parent)
{
#ifdef __APPLE__
    char *snapshot_name = pi_path(parent, "atos"),
         *proof_name = pi_path(parent, "atos.provenance.json");
    char *snapshot = pi_path(tree, snapshot_name), *proof_path = pi_path(tree, proof_name);
    struct json *proof = pi_document(proof_path), *copied = describe_atos(snapshot);
    const char *source = json_text(proof, "source");
    char *native = realpath("/usr/bin/atos", NULL),
         *resolved = source ? realpath(source, NULL) : NULL;
    int bad = !proof || proof->count != 2 || !native || !resolved || !pi_same(source, resolved) ||
              !pi_same(native, resolved) || !pi_hex(json_text(proof, "sha256"), 64) ||
              !pi_same(json_text(proof, "sha256"), json_text(copied, "sha256")) ||
              pi_verify(native, copied) || pi_native_binary(snapshot);
    free(snapshot_name);
    free(proof_name);
    free(snapshot);
    free(proof_path);
    free(native);
    free(resolved);
    json_free(proof);
    json_free(copied);
    return bad ? -1 : 0;
#else
    (void)tree;
    (void)parent;
    return 0;
#endif
}

/*
 * Check the sanitizer input `input` below `tree`, the atos program when
 * `matched` is 0 and its provenance when it is 1, against the digest
 * `expected`, and add its descriptor to `artifacts`. Give 0, or -1 after a
 * message.
 */
static int sanitizer_input(const char *tree, const char *input, int matched, const char *expected,
                           struct json *artifacts)
{
    char *native = pi_path(tree, input);
    struct json *value = matched ? pi_describe(native) : describe_atos(native);
    int bad = !value || !pi_hex(expected, 64) || !pi_same(json_text(value, "sha256"), expected);
    if (bad) {
        fprintf(stderr, "etude prepare import: changed native input: %s\n", native);
        json_free(value);
    } else
        hp_member(artifacts, input, value);
    free(native);
    return bad ? -1 : 0;
}

static int sanitizer_manifest(const char *tree, const char *name, struct json *artifacts)
{
    char *path = pi_path(tree, name), *parent = strdup(name);
    char *slash = parent ? strrchr(parent, '/') : NULL;
    if (!slash) {
        free(path);
        free(parent);
        return -1;
    }
    *slash = 0;
    struct json *manifest = pi_document(path), *descriptor = pi_describe(path);
    const struct json *inputs = json_member(manifest, "inputs"),
                      *version = json_member(manifest, "schema_version");
#ifdef __APPLE__
    const char *platform = "darwin";
    size_t expected = 2;
#elif defined(_WIN32)
    const char *platform = "win32";
    size_t expected = 0;
#else
    const char *platform = "linux";
    size_t expected = 0;
#endif
    int bad = !manifest || manifest->count != 3 || !version || version->type != JSON_INTEGER ||
              version->integer != 1 || !pi_same(json_text(manifest, "platform"), platform) ||
              !inputs || inputs->type != JSON_ARRAY || inputs->count != expected || !descriptor;
    const char *leaves[] = {"atos", "atos.provenance.json"};
    unsigned seen = 0;
    for (size_t i = 0; !bad && i < inputs->count; i++) {
        const char *input = json_text(inputs->item[i], "path");
        int matched = -1;
        for (size_t j = 0; j < 2; j++) {
            char *wanted = pi_path(parent, leaves[j]);
            if (pi_same(input, wanted))
                matched = (int)j;
            free(wanted);
        }
        bad = inputs->item[i]->count != 2 || matched < 0 || (seen & (1u << matched));
        if (!bad) {
            seen |= 1u << matched;
            bad = sanitizer_input(tree, input, matched, json_text(inputs->item[i], "sha256"),
                                  artifacts);
        }
    }
    if (!bad)
        bad = sanitizer_source(tree, parent);
    if (!bad) {
        hp_member(artifacts, name, descriptor);
        descriptor = NULL;
    }
    json_free(descriptor);
    json_free(manifest);
    free(parent);
    free(path);
    return bad ? -1 : 0;
}

static int native_check(const char *tree, const struct json *check)
{
    const struct json *external = json_member(check, "compiler_inputs");

    if (external && external->type != JSON_ARRAY)
        return -1;
    for (size_t i = 0; external && i < external->count; i++)
        if (input_digest(NULL, json_text(external->item[i], "path"),
                         json_text(external->item[i], "sha256")))
            return -1;
    const struct json *inputs = json_member(check, "inputs");
    if (!inputs || inputs->type != JSON_ARRAY)
        return -1;
    for (size_t i = 0; i < inputs->count; i++) {
        char decoded[4096];
        const char *input = hp_input_path(inputs->item[i], decoded);
        if (!pi_relative(input) || input_digest(tree, input, json_text(inputs->item[i], "sha256")))
            return -1;
    }
    if (!pi_same(json_text(check, "backend"), "native"))
        return 0;
    const struct json *unsupported = json_member(check, "unsupported");
    const char *program = json_text(check, "program");
    if (!pi_relative(program) || (unsupported && unsupported->type != JSON_NULL) ||
        input_digest(tree, program, json_text(check, "sha256")))
        return -1;
    char *path = pi_path(tree, program);
    int bad = pi_native_binary(path);
    free(path);
    return bad;
}

static int native_inventory(struct pi_context *c, const char *name)
{
    char *path = pi_path(c->tree, name), *manifest = pi_path(c->tree, "tests/cases/host.host.json");
    struct json *inventory = hp_document(path), *descriptor = pi_describe(manifest);
    free(path);
    free(manifest);
    const struct json *compiler = json_member(inventory, "compiler"),
                      *checks = json_member(inventory, "checks");
    const char *program = json_text(compiler, "executable");
    int bad = !inventory || !descriptor || !checks || checks->type != JSON_ARRAY || !program ||
              !pi_same(json_text(inventory, "manifest_sha256"), json_text(descriptor, "sha256")) ||
              input_digest(NULL, program, json_text(compiler, "sha256"));
    for (size_t i = 0; !bad && i < checks->count; i++)
        bad = native_check(c->tree, checks->item[i]);
    json_free(inventory);
    json_free(descriptor);
    return bad;
}

struct json *pi_native(struct pi_context *c, const struct json *roles)
{
    struct json *artifacts = hp_value(JSON_OBJECT);
    int bad = 0;
    for (size_t i = 0; !bad && i < roles->count; i++) {
        const struct json *role = roles->item[i];
        char *path = pi_path(c->tree, role->name);
        if (pi_same(role->string, "inventory"))
            bad = native_inventory(c, role->name);
        else if (pi_same(role->string, "native")) {
            if (strstr(role->name, "/sanitizer/tools.json"))
                bad = sanitizer_manifest(c->tree, role->name, artifacts);
            else if (pi_native_data(role->name))
                bad = native_style(c, role->name);
            else
                bad = pi_native_binary(path);
        }
        struct json *value = !bad ? pi_describe(path) : NULL;
        if (!value)
            bad = 1;
        else
            hp_member(artifacts, role->name, value);
        free(path);
    }
    if (bad) {
        json_free(artifacts);
        return NULL;
    }
    return artifacts;
}
