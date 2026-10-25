/* Selected-site evidence ownership and readers of retained proof bytes. */
#ifndef ETUDE_RETENTION_H
#define ETUDE_RETENTION_H
#include <signal.h>
#include <stdint.h>
#include "json.h"

struct etude_retention_guard;
/* A successful unconfigured read has a NULL guard. Release only after use. */
int etude_retention_read_begin(const char *const *paths, int bundle, volatile sig_atomic_t *stop,
                               struct etude_retention_guard **guard);
void etude_retention_read_end(struct etude_retention_guard *guard);
int etude_retention_main(int argc, char **argv, volatile sig_atomic_t *stop);
int etude_reference_main(int argc, char **argv, volatile sig_atomic_t *stop);

/* Shared only by the existing retention and reference command modules. */
struct retention {
    char *root;
    struct json *policy;
    int lock;
    volatile sig_atomic_t *stop;
};
int ret_fail(const char *format, ...);
const char *ret_error(void);
char *ret_format(const char *format, ...);
char *ret_path(const char *path, int existing);
int ret_absolute(const char *path);
int ret_inside(const char *root, const char *path);
int ret_dirs(const char *path);
int ret_remove(const char *path);
/* Return the next native directory entry, retaining read failures in STATUS. */
void *ret_next(void *directory, int *status);
struct json *ret_new(enum json_type type);
struct json *ret_string(const char *text);
struct json *ret_integer(int64_t value);
struct json *ret_boolean(int value);
struct json *ret_clone(const struct json *value);
int ret_set(struct json *object, const char *key, struct json *value);
int ret_push(struct json *array, struct json *value);
int ret_has(const struct json *array, const char *text);
int ret_add_name(struct json *array, const char *text);
int64_t ret_number(const struct json *object, const char *name);
struct json *ret_document(const char *path);
int ret_write(const char *path, const struct json *value, int replace);
struct json *ret_descriptor(const char *path);
char *ret_checked(const struct json *ref);
struct json *ret_files(const char *root, uint64_t *size, int64_t *latest);
int ret_usage_tree(const char *root, uint64_t *size);
int64_t ret_now(void);
void ret_stamp(char out[64]);
int64_t ret_seconds(const char *text);
int64_t ret_timestamp_us(const char *text);
int64_t ret_now_us(void);
int ret_policy_check(const struct json *policy, const char *site);
int ret_registry_open(struct retention *r, const char *policy, const char *site,
                      volatile sig_atomic_t *stop);
void ret_registry_close(struct retention *r);
int ret_record(struct retention *r, const char *bundle, char **path, struct json **item);
struct json *ret_closure(const struct json *roots);
int ret_terminal(const char *bundle, const char *run);
int ret_issue_live(const struct json *snapshot, volatile sig_atomic_t *stop);
int ret_issue_snapshot(const char *repository, const char *mapping, const char *output,
                       volatile sig_atomic_t *stop);
void ret_python_json(const struct json *value, struct jbuf *out);
int ret_execute(char *const argv[], const char *cwd, const char *output, char **captured,
                volatile sig_atomic_t *stop, uint64_t deadline_us);
#endif
