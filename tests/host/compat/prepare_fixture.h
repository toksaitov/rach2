/*
 * Fixture trees, commands and JSON for the rows of etude_legacy_hosttest.c
 * that run etude's host preparation and its import of a producer's goals.
 */
#ifndef RACH2_PREPARE_FIXTURE_H
#define RACH2_PREPARE_FIXTURE_H

#include <stddef.h>

#include "../../../tools/etude/json.h"

/* The room for a path. */
#define PF_PATH 4096

/* The failed checks of these rows. */
extern int pf_fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define PF_CHECK(cond, row)                                                                        \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("etude-prepare-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), #cond); \
            pf_fails++;                                                                            \
        }                                                                                          \
    } while (0)

/* The source tree, this program, etude and the C compiler, by absolute
 * paths, which pf_start sets. */
extern char pf_repo[PF_PATH], pf_self[PF_PATH], pf_etude[PF_PATH], pf_cc[PF_PATH];

/* One producer and the native tree that imports its goals. */
struct pf_import {
    char tree[PF_PATH], producer[PF_PATH];
    char commit[41];
    char **goals; /* the request, in order */
    size_t ngoals;
    struct json *files;    /* each goal's producer file: its sha256 and size_bytes */
    struct json *metadata; /* the producer's metadata, whose receipt rows change */
    char *record;          /* the producer's artifact record as made */
    int count;             /* imports run */
    /* The next imports' conditioning, architectures and request line end;
     * NULL for retained, i686 and a newline. */
    const char *conditioning, *arches, *newline;
};

int pf_standin(int argc, char **argv);
int pf_start(const char *argv0);

const char *pf_at(const char *dir, const char *name);
int pf_mkdirs(const char *path);
int pf_write(const char *path, const void *bytes, size_t size);
int pf_write_text(const char *path, const char *text);
int pf_append_text(const char *path, const char *text);
char *pf_read(const char *path, size_t *size);
int pf_exists(const char *path);
int pf_copy(const char *from, const char *to);
int pf_same_file(const char *a, const char *b);
void pf_sha_bytes(const void *bytes, size_t size, char hex[65]);
int pf_sha_file(const char *path, char hex[65]);
int pf_which(const char *name, char out[PF_PATH]);
void pf_scratch(const char *name, char path[PF_PATH]);
void pf_cleanup(const char *path);
size_t pf_count(const char *text, const char *needle);

int pf_run(const char *cwd, const char *const env[], const char *const argv[], struct jbuf *out);
int pf_status(const char *program);

struct json *pf_jparse(const char *text);
struct json *pf_jload(const char *path);
struct json *pf_jstr(const char *text);
struct json *pf_jint(long long value);
struct json *pf_jbool(int truth);
struct json *pf_jnull(void);
struct json *pf_jobj(void);
struct json *pf_jarr(void);
struct json *pf_jclone(const struct json *value);
void pf_jadd(struct json *array, struct json *value);
void pf_jset(struct json *object, const char *name, struct json *value);
void pf_jdel(struct json *object, const char *name);
struct json *pf_jget(struct json *object, const char *name);
char *pf_jtext(const struct json *value);
int pf_jsave(const char *path, const struct json *value);
struct json *pf_describe(const char *path);
int pf_jsame(const struct json *a, const char *b_path);

void pf_native_tree(const char *tree);
void pf_native_source(const char *tree, const char *name, const char *metadata, const char *body);
void pf_windows_tree(const char *tree);
struct json *pf_native_prepare(const char *tree, const char *group, const char *const args[],
                               const char *const env[], int expected, struct jbuf *out);

void pf_import_fixture(struct pf_import *s, const char *work, const char *const goals[],
                       size_t ngoals, const struct json *checks);
void pf_import_source(struct pf_import *s);
struct json *pf_import_run(struct pf_import *s, int expected, const struct json *producer,
                           const char *const env[]);
void pf_import_free(struct pf_import *s);
void pf_rerecord(struct pf_import *s, const char *goal, const char *text);
const char *pf_split(const char *text, size_t *count, char ***words);
void pf_words_free(char **words, size_t count);

void pf_host_rows(void);
void pf_import_rows(void);

#endif
