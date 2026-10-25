/* Native import and export of verified ordinary preparation products. */
#ifndef ETUDE_PREPARATION_IMPORT_H
#define ETUDE_PREPARATION_IMPORT_H
#include <signal.h>
#include "host_prepare.h"

struct pi_context {
    struct hp_context host;
    const char *tree, *commit, *record, *receipt, *etude, *output;
    const char *config, *request, *work, *provenance, *arches, *conditioning;
};
int etude_prepare_import_main(int argc, char **argv, volatile sig_atomic_t *stop);
int etude_prepare_export_main(int argc, char **argv, volatile sig_atomic_t *stop);

/* The fixed producer document shares these checks between both operations. */
int pi_same(const char *a, const char *b);
int pi_hex(const char *text, size_t length);
int pi_relative(const char *path);
char *pi_path(const char *tree, const char *name);
struct json *pi_document(const char *path);
struct json *pi_describe(const char *path);
int pi_verify(const char *path, const struct json *descriptor);
int pi_create(const char *path, const struct json *value);
struct json *pi_source(const char *tree, const char *commit);
struct json *pi_record(const char *text);
struct json *pi_goals(const char *tree);
const char *pi_role(const char *goal);
int pi_receipt(const struct json *receipt, const char *commit, const char *record);
int pi_clean(const struct json *receipt, const struct json *roles, const char *conditioning);
struct json *pi_nested(const struct json *inventories, const struct json *files,
                       const char *manifest_hash);
int pi_native_binary(const char *path);
int pi_native_data(const char *name);
struct json *pi_native(struct pi_context *c, const struct json *roles);
int pi_producer(const struct json *producer);
char *pi_metadata(struct pi_context *c, const struct json *producer);
int pi_transfer(struct pi_context *c, const struct json *producer, const struct json *imported);
struct json *pi_build(struct pi_context *c, const struct json *roles, const struct json *imported);
int pi_validator(struct pi_context *c, const struct json *producer, const struct json *metadata);
#endif
