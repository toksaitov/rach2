/* Export a real ordinary producer, or import it before native Make targets. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#endif
#include "preparation_import.h"
#include "measurement_context.h"
#include "etude/legacy.h"

static const char *const auxiliary[] = {
    "vendor/musl/musl-1.2.5/src/crypt/crypt_sha512.c",
    "build/x86_64/vendor-work/musl/musl-1.2.5/src/crypt/crypt_sha512.c",
    "vendor/musl/install/lib/libc.a", "vendor/musl/.stamp"};

static int verify_files(const char *tree, const struct json *files)
{
    if (!files || files->type != JSON_OBJECT)
        return -1;
    for (size_t i = 0; i < files->count; i++) {
        if (!pi_relative(files->item[i]->name))
            return -1;
        char *path = pi_path(tree, files->item[i]->name);
        int bad = pi_verify(path, files->item[i]);
        free(path);
        if (bad)
            return -1;
    }
    return 0;
}

static int declared(const struct json *files, const struct json *goals)
{
    if (!files || !goals)
        return -1;
    for (size_t i = 0; i < files->count; i++)
        if (!json_member(goals, files->item[i]->name))
            return -1;
    return 0;
}

static struct json *inventories(const char *tree, const struct json *files)
{
    struct json *texts = hp_value(JSON_OBJECT);
    for (size_t i = 0; i < files->count; i++) {
        const char *name = files->item[i]->name;
        if (!pi_same(pi_role(name), "inventory"))
            continue;
        char *path = pi_path(tree, name), *text = hp_read(path);
        free(path);
        if (!text) {
            json_free(texts);
            return NULL;
        }
        hp_member(texts, name, hp_string(text));
        free(text);
    }
    return texts;
}

static struct json *describe_nested(const char *tree, const struct json *hashes)
{
    if (!hashes)
        return NULL;
    struct json *nested = hp_value(JSON_OBJECT);
    for (size_t i = 0; i < hashes->count; i++) {
        char *path = pi_path(tree, hashes->item[i]->name);
        struct json *value = pi_describe(path);
        free(path);
        if (!value || !pi_same(json_text(value, "sha256"), hashes->item[i]->string)) {
            json_free(value);
            json_free(nested);
            return NULL;
        }
        hp_member(nested, hashes->item[i]->name, value);
    }
    return nested;
}

static struct json *auxiliary_files(const char *tree)
{
    struct json *files = hp_value(JSON_OBJECT);
    for (size_t i = 0; i < sizeof(auxiliary) / sizeof(*auxiliary); i++) {
        char *path = pi_path(tree, auxiliary[i]);
        struct stat st;
        int present = !stat(path, &st);
        struct json *value = present ? pi_describe(path) : NULL;
        free(path);
        if (present && !value) {
            json_free(files);
            return NULL;
        }
        if (value)
            hp_member(files, auxiliary[i], value);
    }
    return files;
}

static int source_equal(const struct pi_context *c, const struct json *before)
{
    struct json *after = pi_source(c->tree, c->commit);
    int bad = !after || !before || !json_equal(before, after);
    json_free(after);
    return bad ? -1 : 0;
}

static int export_producer(struct pi_context *c)
{
#ifndef __linux__
    (void)c;
    fputs("etude prepare export: the ordinary producer must be Linux\n", stderr);
    return -1;
#endif
    struct json *source = pi_source(c->tree, c->commit), *goals = pi_goals(c->tree);
    char *text = hp_read(c->record), *manifest = pi_path(c->tree, "tests/cases/host.host.json");
    struct json *files = pi_record(text), *receipt = hp_document(c->receipt);
    struct json *validator = pi_describe(c->etude), *manifest_descriptor = pi_describe(manifest);
    struct json *texts = files ? inventories(c->tree, files) : NULL;
    struct json *hashes = pi_nested(texts, files, json_text(manifest_descriptor, "sha256"));
    struct json *nested = describe_nested(c->tree, hashes), *aux = auxiliary_files(c->tree);
    int bad = !source || !files || !receipt || !validator || !nested || !aux ||
              declared(files, goals) || pi_receipt(receipt, c->commit, text) ||
              etude_legacy_check_tree(c->record, c->tree, 1, stdout) ||
              verify_files(c->tree, files);
    struct json *data = hp_value(JSON_OBJECT);
    if (!bad) {
        hp_member(data, "format", hp_string("etude-preparation-producer"));
        hp_member(data, "schema_version", hp_number(1));
        hp_member(data, "commit", hp_string(c->commit));
        hp_member(data, "source", hp_clone(source));
        hp_member(data, "record", hp_string(text));
        hp_member(data, "receipt", hp_clone(receipt));
        struct json *tool = hp_clone(validator);
        hp_member(tool, "path", hp_string(c->etude));
        hp_member(data, "validator", tool);
        hp_member(data, "auxiliary", hp_clone(aux));
        hp_member(data, "nested", hp_clone(nested));
        hp_member(data, "inventories", hp_clone(texts));
        char *after = hp_read(c->record);
        bad = !after || strcmp(after, text) || source_equal(c, source) ||
              pi_verify(c->etude, validator);
        free(after);
        if (!bad)
            bad = pi_create(c->output, data);
        if (!bad) {
            struct json *output = pi_describe(c->output);
            if (!output)
                bad = 1;
            else {
                hp_member(output, "metadata", hp_string(c->output));
                struct jbuf bytes = {0};
                json_canonical(output, &bytes);
                printf("%s\n", bytes.data);
                jbuf_free(&bytes);
                json_free(output);
            }
        }
    }
    free(text);
    free(manifest);
    json_free(source);
    json_free(goals);
    json_free(files);
    json_free(receipt);
    json_free(validator);
    json_free(manifest_descriptor);
    json_free(texts);
    json_free(hashes);
    json_free(nested);
    json_free(aux);
    json_free(data);
    return bad ? -1 : 0;
}

static char *new_output(const char *path)
{
    char *parent = strdup(path), *slash = strrchr(parent, '/'), *output = NULL;
    struct stat st;
    if (!slash || !slash[1] || !lstat(path, &st) || errno != ENOENT)
        goto done;
    char *leaf = strdup(slash + 1);
    if (slash == parent || (slash == parent + 2 && parent[1] == ':'))
        slash[1] = 0;
    else
        *slash = 0;
    char *resolved = realpath(parent, NULL);
    if (resolved)
        output = pi_path(resolved, leaf);
    free(resolved);
    free(leaf);
done:
    free(parent);
    return output;
}

static int config_location(struct pi_context *c, char **config, char **output)
{
    *config = realpath(c->config, NULL);
    *output = new_output(c->provenance);
    if (!*config || !*output)
        return -1;
    size_t root = strlen(c->tree);
    const char *one = strrchr(*config, '/'), *two = strrchr(*output, '/');
    return !one || !two || one - *config != two - *output ||
                   strncmp(*config, *output, (size_t)(one - *config)) ||
                   strncmp(*output, c->tree, root) || (*output)[root] != '/'
               ? -1
               : 0;
}

static int config_check(const struct pi_context *c, const struct json *config)
{
    const struct json *version = json_member(config, "schema_version");
    const char *commit = json_text(config, "commit");
    return !config || config->type != JSON_OBJECT || config->count != 4 ||
                   !pi_same(json_text(config, "format"), "etude-preparation-import") || !version ||
                   version->type != JSON_INTEGER || version->integer != 1 || !pi_hex(commit, 40) ||
                   (c->commit && *c->commit && !pi_same(c->commit, commit)) ||
                   pi_producer(json_member(config, "producer"))
               ? -1
               : 0;
}

static int member_order(const void *left, const void *right)
{
    const struct json *const *a = left, *const *b = right;
    return strcmp((*a)->name, (*b)->name);
}

static void ordered_members(struct json *object)
{
    if (object->count > 1)
        qsort(object->item, object->count, sizeof(*object->item), member_order);
}

static struct json *requested_roles(struct pi_context *c)
{
    char *text = hp_read(c->request), *line = text;
    struct json *known = pi_goals(c->tree), *roles = hp_value(JSON_OBJECT);
    int bad = !text || !known;
    while (line && *line && !bad) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        size_t length = strlen(line);
        if (length && line[length - 1] == '\r')
            line[--length] = 0;
        if (!pi_relative(line) || !json_member(known, line))
            bad = 1;
        else
            hp_member(roles, line, hp_string(pi_role(line)));
        line = next;
    }
    free(text);
    json_free(known);
    if (bad || !roles->count) {
        json_free(roles);
        return NULL;
    }
    ordered_members(roles);
    return roles;
}

static int metadata_check(struct pi_context *c, const struct json *metadata,
                          const struct json *source, const struct json *roles)
{
    const struct json *version = json_member(metadata, "schema_version");
    const char *record = json_text(metadata, "record");
    struct json *files = pi_record(record);
    int bad =
        !metadata || !version || version->type != JSON_INTEGER || version->integer != 1 ||
        !pi_same(json_text(metadata, "format"), "etude-preparation-producer") ||
        !pi_same(json_text(metadata, "commit"), c->commit) || !json_member(metadata, "source") ||
        !json_equal(json_member(metadata, "source"), source) || !files || declared(roles, files) ||
        pi_receipt(json_member(metadata, "receipt"), c->commit, record) ||
        pi_clean(json_member(metadata, "receipt"), roles, c->conditioning);
    json_free(files);
    return bad ? -1 : 0;
}

static struct json *imported_files(struct pi_context *c, const struct json *metadata,
                                   const struct json *roles, const struct json *source)
{
    struct json *files = pi_record(json_text(metadata, "record")),
                *imported = hp_value(JSON_OBJECT);
    for (size_t i = 0; i < roles->count; i++)
        if (pi_same(roles->item[i]->string, "portable"))
            hp_member(imported, roles->item[i]->name,
                      hp_clone(json_member(files, roles->item[i]->name)));
    const struct json *aux = json_member(metadata, "auxiliary");
    int bad = !aux || aux->type != JSON_OBJECT;
    for (size_t i = 0; !bad && i < aux->count; i++) {
        int allowed = 0;
        for (size_t j = 0; j < sizeof(auxiliary) / sizeof(*auxiliary); j++)
            allowed |= pi_same(aux->item[i]->name, auxiliary[j]);
        if (!allowed)
            bad = 1;
        else
            hp_member(imported, aux->item[i]->name, hp_clone(aux->item[i]));
    }
    char *manifest = pi_path(c->tree, "tests/cases/host.host.json");
    struct json *bound = pi_describe(manifest);
    struct json *hashes =
        pi_nested(json_member(metadata, "inventories"), files, json_text(bound, "sha256"));
    const struct json *nested = json_member(metadata, "nested");
    bad =
        bad || !hashes || !nested || nested->type != JSON_OBJECT || hashes->count != nested->count;
    int inventory = 0;
    for (size_t i = 0; i < roles->count; i++)
        inventory |= pi_same(roles->item[i]->string, "inventory");
    for (size_t i = 0; !bad && i < hashes->count; i++) {
        const struct json *value = json_member(nested, hashes->item[i]->name);
        if (!pi_same(json_text(value, "sha256"), hashes->item[i]->string))
            bad = 1;
#ifndef _WIN32
        else if (inventory)
            hp_member(imported, hashes->item[i]->name, hp_clone(value));
#endif
    }
#ifdef _WIN32
    (void)inventory;
#endif
    for (size_t i = 0; !bad && i < imported->count; i++)
        bad = !pi_relative(imported->item[i]->name) ||
              json_member(source, imported->item[i]->name) != NULL;
    free(manifest);
    json_free(bound);
    json_free(files);
    json_free(hashes);
    if (bad) {
        json_free(imported);
        return NULL;
    }
    ordered_members(imported);
    return imported;
}

static int metadata_digest(const char *path, const struct json *producer)
{
    char digest[65];
    return !path || hp_hash(path, digest) || !pi_same(digest, json_text(producer, "sha256")) ? -1
                                                                                             : 0;
}

static int final_inputs(struct pi_context *c, const struct json *source, const char *config_path,
                        const char *config_bytes, const char *metadata_path,
                        const struct json *producer)
{
    char *after = hp_read(config_path), digest[65];
    int bad = !after || strcmp(config_bytes, after) || source_equal(c, source) ||
              hp_hash(metadata_path, digest) || !pi_same(digest, json_text(producer, "sha256"));
    free(after);
    return bad ? -1 : 0;
}

static int import_producer(struct pi_context *c)
{
    char *config_path = NULL, *output = NULL;
    if (config_location(c, &config_path, &output)) {
        free(config_path);
        free(output);
        return -1;
    }
    char *config_bytes = hp_read(config_path);
    struct json *config = hp_document(config_path);
    int bad = !config_bytes || config_check(c, config);
    const struct json *producer = json_member(config, "producer");
    if (!bad)
        c->commit = json_text(config, "commit");
    struct json *source = !bad ? pi_source(c->tree, c->commit) : NULL;
    struct json *roles = source ? requested_roles(c) : NULL;
    char *metadata_path = roles ? pi_metadata(c, producer) : NULL;
    bad = bad || metadata_digest(metadata_path, producer);
    struct json *metadata = !bad ? pi_document(metadata_path) : NULL;
    bad = bad || !metadata || metadata_check(c, metadata, source, roles) ||
          pi_validator(c, producer, metadata);
    struct json *imported = !bad ? imported_files(c, metadata, roles, source) : NULL;
    bad = bad || !imported || pi_transfer(c, producer, imported) || verify_files(c->tree, imported);
    struct json *commands = !bad ? pi_build(c, roles, imported) : NULL;
    struct json *artifacts = commands ? pi_native(c, roles) : NULL;
    bad = bad || !commands || !artifacts || verify_files(c->tree, imported) ||
          final_inputs(c, source, config_path, config_bytes, metadata_path, producer);
    if (!bad) {
        struct json *provenance = hp_value(JSON_OBJECT);
        hp_member(provenance, "schema_version", hp_number(1));
        hp_member(provenance, "format", hp_string("etude-preparation-import-provenance"));
        hp_member(provenance, "commit", hp_string(c->commit));
        hp_member(provenance, "producer_sha256", hp_string(json_text(producer, "sha256")));
        hp_member(provenance, "ordinary_production", hp_clone(json_member(metadata, "receipt")));
        hp_member(provenance, "imported", hp_clone(imported));
        hp_member(provenance, "roles", hp_clone(roles));
        hp_member(provenance, "native_build", hp_clone(commands));
        hp_member(provenance, "artifacts", hp_clone(artifacts));
        bad = pi_create(output, provenance);
        json_free(provenance);
    }
    free(config_path);
    free(output);
    free(config_bytes);
    free(metadata_path);
    json_free(config);
    json_free(source);
    json_free(roles);
    json_free(metadata);
    json_free(imported);
    json_free(commands);
    json_free(artifacts);
    return bad ? -1 : 0;
}

static int import_options(struct pi_context *c)
{
    if (!c->conditioning)
        c->conditioning = "retained";
    return !c->config || !c->request || !c->work || !c->provenance || !c->arches ||
                   (!pi_same(c->arches, "i686") && !pi_same(c->arches, "x86_64") &&
                    !pi_same(c->arches, "i686,x86_64")) ||
                   (!pi_same(c->conditioning, "retained") && !pi_same(c->conditioning, "clean"))
               ? -1
               : 0;
}

static int options(struct pi_context *c, int argc, char **argv, int exporting)
{
    struct {
        const char *name;
        const char **value;
    } fields[] = {{"tree", &c->tree},     {"commit", &c->commit},
                  {"record", &c->record}, {"receipt", &c->receipt},
                  {"etude", &c->etude},   {"output", &c->output},
                  {"config", &c->config}, {"request", &c->request},
                  {"work", &c->work},     {"provenance", &c->provenance},
                  {"arches", &c->arches}, {"conditioning", &c->conditioning}};
    for (int i = 1; i < argc; i++) {
        int matched = 0;
        if (strncmp(argv[i], "--", 2) || i + 1 == argc)
            return -1;
        for (size_t j = 0; j < sizeof(fields) / sizeof(*fields); j++)
            if ((exporting ? j < 6 : (j < 2 || j >= 6)) && pi_same(argv[i] + 2, fields[j].name)) {
                if (*fields[j].value)
                    return -1;
                *fields[j].value = argv[++i];
                matched = 1;
                break;
            }
        if (!matched)
            return -1;
    }
    if (!c->tree || !c->commit)
        return -1;
    if (exporting)
        return !c->record || !c->receipt || !c->etude || !c->output ? -1 : 0;
    return import_options(c);
}

static int import_main(int argc, char **argv, volatile sig_atomic_t *stop, int exporting)
{
    struct pi_context c;
    memset(&c, 0, sizeof(c));
    c.host.stop = stop;
    if (options(&c, argc, argv, exporting)) {
        fprintf(stderr, "etude prepare %s: invalid fixed preparation operands\n",
                exporting ? "export" : "import");
        return 2;
    }
    char *tree = realpath(c.tree, NULL);
    if (!tree || strlen(tree) >= sizeof(c.host.tree)) {
        free(tree);
        return 1;
    }
    strcpy(c.host.tree, tree);
    c.tree = tree;
    int bad = exporting ? export_producer(&c) : import_producer(&c);
    if (bad)
        fprintf(stderr, "etude prepare %s: refused or failed; inputs and logs retained\n",
                exporting ? "export" : "import");
    free(tree);
    return bad ? 1 : 0;
}

int etude_prepare_import_main(int argc, char **argv, volatile sig_atomic_t *stop)
{
    return import_main(argc, argv, stop, 0);
}

int etude_prepare_export_main(int argc, char **argv, volatile sig_atomic_t *stop)
{
    return import_main(argc, argv, stop, 1);
}
