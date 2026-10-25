/* host_prepare.h: native inventories of Make-built host checks. */
#ifndef ETUDE_HOST_PREPARE_H
#define ETUDE_HOST_PREPARE_H
#include <signal.h>
#include "json.h"

struct hp_words {
    char **v;
    size_t n;
};

struct hp_context {
    char tree[4096], work[4096];
    const char *arch, *group, *output, *only, *previous, *origin;
    int gnu, print;
    volatile sig_atomic_t *stop;
    struct hp_words compiler;
    char *compiler_requested;
    struct json *identity, *old;
};

struct hp_check {
    struct json *entry;
    struct hp_words sources, flags, libraries, data, tools;
    int first, reused, objects;
};

int etude_host_prepare_main(int argc, char **argv, volatile sig_atomic_t *stop);
void hp_add(struct hp_words *words, const char *word);
int hp_split(struct hp_words *words, const char *text);
void hp_words_free(struct hp_words *words);
int hp_hash(const char *path, char hex[65]);
char *hp_read(const char *path);
int hp_mkdir(const char *path);
int hp_write(const char *path, const void *bytes, size_t size);
struct json *hp_document(const char *path);
struct json *hp_value(enum json_type type);
struct json *hp_string(const char *text);
struct json *hp_number(long long value);
void hp_member(struct json *object, const char *name, struct json *value);
void hp_item(struct json *array, struct json *value);
struct json *hp_clone(const struct json *value);
struct json *hp_array(const struct hp_words *words);
int hp_publish(const char *path, const struct json *value);
int hp_run(struct hp_context *c, char *const argv[], const char *out, const char *err);
char *hp_program(const char *name);
int hp_compiler(struct hp_context *c);
int hp_build(struct hp_context *c, struct hp_check *check, const char *program, int system);
int hp_input(struct hp_context *c, const char *path, struct json *inputs, int external);
void hp_sort_inputs(struct json *inputs);
int hp_inputs_valid(struct hp_context *c, const struct json *inputs, const char *origin);
int hp_files(struct hp_words *files, const char *root);
int hp_tools(struct hp_context *c, struct hp_check *check);
int hp_scripts(struct hp_context *c, struct json *checks, const char *base);
int hp_style(struct hp_context *c);
int hp_tool_main(struct hp_context *c);
void hp_common_flags(struct hp_words *flags);
void hp_windows_flags(struct hp_words *flags);
int hp_source_list(struct hp_context *c, struct hp_words *sources, const char *name);
char *hp_make_words(const struct hp_words *words);
int hp_windows_host(void);
const char *hp_input_path(const struct json *value, char path[4096]);
int hp_identity_equal(struct hp_context *c, const struct json *a, const struct json *b);
int hp_preprocess(struct hp_context *c, struct hp_check *check, const char *program);
#endif
