/*
 * artifact.h: etude's store of immutable artifacts, each a file named by the
 * SHA-256 of its content, and the preparation graph whose nodes make them.
 * An artifact is published whole or not at all. tools/etude/Readme.md
 * describes the store and the graph.
 */
#ifndef ETUDE_ARTIFACT_H
#define ETUDE_ARTIFACT_H

#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#include "../../win/files.h"
#endif

#include "etude/exec.h"

/* The length of a digest in hex, without its terminator. */
#define ETUDE_DIGEST_HEX 64

/* A store of artifacts in one directory, open in one process. */
struct etude_store;

struct stat;

/**
 * Give the SHA-256 of the regular file at `path` in `digest`, and its status
 * as it was opened in `*st`.
 *
 * `cache` is the path of a digests file, or NULL for none. A digest kept
 * there for a file of the same device, inode, size and times of
 * modification and change is given without reading the file. Otherwise the
 * file is read whole, and its digest is kept there unless the file changed
 * while it was read, or less than two seconds before the read began, since
 * a write in the same tick of the file system's clock could leave its status
 * as it was. tools/etude/Readme.md describes the file.
 *
 * @return 0, or -1 with errno set, EISDIR for a directory and EINVAL for
 *         anything else but a regular file.
 */
int etude_digest_file(const char *cache, const char *path, struct stat *st,
                      char digest[ETUDE_DIGEST_HEX + 1]);

/**
 * Open the store in directory `dir`, made with its subdirectories if they
 * are missing, and remove the partial files of writers that have ended.
 *
 * The handle is used from one thread, and a process opens one handle on a
 * store, since on POSIX systems closing any descriptor of a file releases
 * the process's locks on it.
 *
 * @return the store, or NULL with errno set.
 */
struct etude_store *etude_store_open(const char *dir);

/** Close the store; what it published stays. */
void etude_store_close(struct etude_store *s);

/**
 * Make the store use the digests file `cache`, as etude_digest_file does,
 * for the files it publishes and for its objects, so that a file whose
 * digest is kept is not read to be hashed. An object the store publishes is
 * kept with its digest at once.
 *
 * @return 0, or -1 with errno ENOMEM.
 */
int etude_store_digests(struct etude_store *s, const char *cache);

/**
 * Publish a copy of the file at `path` as an object, and give its digest in
 * `digest` and its size in `*size` when `size` is not NULL.
 *
 * The copy is written and synchronized under a partial name, and only then
 * given the object's name, so that the name never shows a partial file. A
 * chunk of zeros in the file is left a hole in the object, so that a sparse
 * file stays sparse. An object already published with the same content is
 * kept, and one whose content differs from its name is replaced. When
 * `expect` is not NULL, a copy whose digest differs from it is refused, and
 * nothing is published.
 *
 * With a digests file, a file whose digest is kept is copied without being
 * hashed, and is not copied at all when its object is published and kept;
 * a file that changes while it is copied so is copied again and hashed.
 *
 * @return 0, or -1 with errno set: EBADMSG for a digest other than `expect`,
 *         or an error of the system.
 */
int etude_store_put(struct etude_store *s, const char *path, const char *expect,
                    char digest[ETUDE_DIGEST_HEX + 1], uint64_t *size);

/**
 * Write into `buf` the path of object `digest`, which is read-only.
 *
 * @return 0, or -1 with errno EINVAL for a malformed digest or ERANGE when
 *         `size` is too small.
 */
int etude_store_path(const struct etude_store *s, const char *digest, char *buf, size_t size);

/**
 * Read object `digest` whole and check its content against its name, giving
 * its size in `*size` when `size` is not NULL. With a digests file that
 * keeps the object's digest, the object is not read.
 *
 * @return 0, or -1 with errno ENOENT for no such object, EBADMSG for one
 *         whose content differs from its name, or an error of the system.
 */
int etude_store_check(const struct etude_store *s, const char *digest, uint64_t *size);

/**
 * Make `dest`, a file that must not exist, a private writable copy of object
 * `digest`, checked by the digest as it is copied, unless a digests file
 * keeps the object's digest and the object does not change while it is
 * copied. A chunk of zeros in the object is left a hole in `dest`, as
 * etude_store_put leaves it.
 *
 * @return 0, or -1 with errno set as etude_store_check gives, EEXIST for a
 *         `dest` that exists, or an error of the system; `dest` is then
 *         removed.
 */
int etude_store_copy(const struct etude_store *s, const char *digest, const char *dest);

/**
 * Make `dest`, a file that must not exist, an empty qcow2 overlay over
 * object `digest`, whose absolute path it names as its raw backing file, in
 * clusters of `cluster_bytes`, a power of two from 512 to 2 MiB.
 *
 * The overlay has the object's size and allocates no cluster, so that
 * QEMU's writes go to the overlay alone and `qemu-img map` lists what they
 * changed. The object is not read, and the caller checks it by its digest.
 *
 * @return 0, or -1 with errno set: EINVAL for a malformed digest, a cluster
 *         size out of range, or an object that is empty or not a whole number
 *         of 512-byte sectors; EFBIG for an object whose L1 table would pass
 *         QEMU's 32 MiB; ENAMETOOLONG for an object path that does not fit
 *         the first cluster; EEXIST for a `dest` that exists; or an error of
 *         the system, after which `dest` is removed.
 */
int etude_store_overlay(const struct etude_store *s, const char *digest, uint32_t cluster_bytes,
                        const char *dest);

/**
 * Remove each partial file whose writer has ended, and give their count in
 * `*removed` when it is not NULL.
 *
 * A writer holds a lock on its partial file from its making until its
 * removal, so that a partial file of a writer that runs is never removed.
 *
 * @return 0, or -1 with errno set.
 */
int etude_store_sweep(struct etude_store *s, size_t *removed);

/* A preparation graph: its nodes, their recipes and their order. */
struct etude_prep;

/* One node of a graph, as the caller adds it; etude_prep_add copies it. */
struct etude_prep_node {
    const char *name;          /* the manifest's `node`, unique in the graph */
    char *const *argv;         /* the recipe, or NULL for a file of the tree */
    char *const *envp;         /* the recipe's whole environment */
    const char *output;        /* the product: a path in the node's work
                                  directory, or for a file of the tree its
                                  path */
    const char *const *inputs; /* the nodes it uses, ended by NULL; may be NULL */
    const char *const *key;    /* further text the product depends on, such
                                  as a commit; ended by NULL; may be NULL */
    int fresh;                 /* nonzero: always made again, never taken
                                  from an earlier preparation */
    uint64_t bound_us;         /* the bound of the node's prepare stage */
};

/* What became of a node in a run. */
enum etude_prep_state {
    ETUDE_PREP_PENDING, /* not reached: the run refused the graph */
    ETUDE_PREP_REUSED,  /* published by an earlier preparation, and checked */
    ETUDE_PREP_BUILT,   /* made and published by this run */
    ETUDE_PREP_FAILED,  /* its recipe or its publication failed */
    ETUDE_PREP_BLOCKED  /* an input failed or was blocked; its recipe never ran */
};

/* A node's result, valid until the graph is freed or run again. */
struct etude_prep_result {
    const char *name;
    enum etude_prep_state state;
    char key[ETUDE_DIGEST_HEX + 1];    /* "" for a file of the tree */
    char digest[ETUDE_DIGEST_HEX + 1]; /* the product's, once published */
    uint64_t size;
    const char *why;    /* what failed, or "" */
    const char *work;   /* the work directory kept after a failure, or NULL */
    enum etude_end end; /* how the recipe ended, when `ran` */
    int value;
    int ran;                 /* nonzero when this run started the recipe */
    struct etude_output err; /* the recipe's standard error, as kept */
    uint64_t started_us;     /* monotonic times: the node was reached, */
    uint64_t ended_us;       /* and its state was decided */
};

/* How a run makes the products. */
struct etude_prep_opts {
    const char *scratch;   /* where the work directories are made */
    unsigned jobs;         /* recipes run at once, at least one */
    size_t output_max;     /* bytes kept of a recipe's output */
    uint64_t grace_us;     /* from a recipe's termination signal to its kill */
    const char *attempt;   /* the prefix of the recipes' attempt identities */
    etude_register_fn reg; /* as etude_exec_spec's, for the recipes */
    etude_child_fn child;
    void *reg_ctx;
};

/** Make an empty graph, or NULL when memory ran out. */
struct etude_prep *etude_prep_new(void);

/** Free a graph and its results. */
void etude_prep_free(struct etude_prep *g);

/**
 * Add node `n` to the graph.
 *
 * In a recipe's arguments and environment, `${WORK}` is the node's work
 * directory, in which the recipe runs, and `${IN:NAME}` the read-only path
 * of input NAME's product.
 *
 * @return 0, or -1 with errno EEXIST for a name the graph has, EINVAL for a
 *         node without a name or an output, or ENOMEM.
 */
int etude_prep_add(struct etude_prep *g, const struct etude_prep_node *n);

/**
 * Make the product of every node, in the order of its inputs, and publish
 * each in store `s`, running the recipes through executor `ex`.
 *
 * A node's key is the digest of its name, recipe, output, `key` text and
 * inputs' digests. A node that is not fresh is reused when the store has a
 * record of its key whose object checks; otherwise its recipe runs in a
 * private work directory, and its product is published only when the
 * recipe exited 0 and its cleanup was confirmed, and is then recorded under
 * its key. Of several processes that prepare one key at once, one runs the
 * recipe and the others wait for it and reuse its product. A recipe that
 * runs past its bound is cancelled. The work directory is removed after a
 * success and kept after a failure.
 *
 * @return 0 when every node was reused or built; 1 when any failed or was
 *         blocked; -1 with errno EINVAL for an input the graph lacks, ELOOP
 *         for a cycle, or an error of the system, with nothing run.
 */
int etude_prep_run(struct etude_prep *g, struct etude_store *s, struct etude_exec *ex,
                   const struct etude_prep_opts *o);

/**
 * Give node `name`'s result from the last run.
 *
 * @return the result, or NULL for a name the graph lacks.
 */
const struct etude_prep_result *etude_prep_result(const struct etude_prep *g, const char *name);

#endif
