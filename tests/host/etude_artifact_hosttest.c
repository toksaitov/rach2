/**
 * Check etude's artifact store and preparation graph (tools/etude/posix/
 * store.c and prepare.c) with the POSIX executor, this program run as the
 * recipes, with `--recipe MODE COUNT TAG OUT ARG...`.
 *
 * host-deps: tools/etude/posix/store.c
 * host-deps: tests/host/compat/artifact_prepare_host.c
 * host-deps: tools/etude/capture.c tools/etude/posix/exec.c
 * host-deps: tools/etude/posix/keeper.c tools/etude/posix/process.c
 * host-deps: utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude/posix
 * host-cflags: -Wall -Wextra -Werror
 * host-cflags: -D_FILE_OFFSET_BITS=64
 *
 * An object is published read-only under its digest, once, and refused
 * when its digest is not the one expected. A sparse file is published and
 * copied with its holes. A writer killed while it copies leaves no object,
 * and its partial file is swept, while a writer that runs keeps its own and
 * publishes. A graph builds its nodes in the order of
 * their inputs and reuses each product by its key; a changed source makes
 * its dependents again, a changed key its own node alone, and a fresh node
 * is made every time. A recipe that fails, is killed, runs past its bound,
 * makes no product or makes a link publishes nothing, keeps its work as
 * evidence and blocks its dependents. A damaged object or record is made
 * again. Six processes prepare one node at once: one runs the recipe, the
 * others reuse its product, and a reader never sees a partial one. A
 * controller killed while its recipe runs leaves nothing, and its lock
 * goes with it. An overlay of an object is an empty qcow2 file over it, in
 * each cluster size QEMU opens, and one QEMU cannot open is refused. A
 * digest taken on the processor's SHA instructions is the portable code's,
 * a table of digests is written once, and a digest kept by the writer of a
 * file is used until the file changes; two writers of one file at once keep
 * the digest of the bytes there.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "compat/pathmax_host.h"
#include "compat/fixdir_host.c"
#include "etude/artifact.h"
#include "posix.h"
#include "sha256.h"
#include "store.h"

static int fails;

/* A host test build of store.c's settling time, after which a file's digest
 * may be kept, and its counts of the reads that hashed a file and of the
 * copies it made. */
extern long long etude_store_settle_ns;
extern unsigned long etude_store_hashed, etude_store_copied;

/* Nonzero makes store.c hash on the portable code of utils/sha256.c alone,
 * and not on the processor's SHA instructions. */
extern int etude_store_portable;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                    \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            printf("etude-artifact-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), \
                   #cond);                                                                  \
            fails++;                                                                        \
        }                                                                                   \
    } while (0)

/* The bound of a recipe that must not run out of it. */
#define LONG_BOUND 60000000u

/* The bound of the recipe that hangs. */
#define SHORT_BOUND 300000u

static uint64_t (*preparation_clock)(void);
static char hang_ready[PATH_MAX];
static uint64_t hang_clock_at;

/** The real monotonic clock, except while the bound row controls its time. */
uint64_t etude_artifact_prep_now_us(void)
{
    return preparation_clock ? preparation_clock() : posix_now_us();
}

/** Expire the unchanged bound only after the recipe made its partial file. */
static uint64_t hang_clock(void)
{
    return hang_clock_at + (access(hang_ready, F_OK) == 0 ? SHORT_BOUND + 1 : 0);
}

/* The processes that prepare one node at once. */
#define RACERS 6

/* The size of the race's product, written in pieces. */
#define RACE_BYTES (4u << 20)

/* The size of each half of a file copied from a FIFO. */
#define HALF (1u << 20)

/* The size of the sparse file, the offset of its data and their length. The
 * data begin past 16 MiB, since APFS fills with zeros a shorter gap that a
 * write past the end of a file leaves. */
#define SPARSE_BYTES (128u << 20)
#define SPARSE_AT    ((32u << 20) + 7u)
#define SPARSE_DATA  ((1u << 20) + 100u)

/* A list of a recipe's arguments, ended by NULL. */
#define ARGS(...) ((const char *const[]){__VA_ARGS__, NULL})

/* This program, by an absolute path. */
static char self[PATH_MAX];

/* ---- Files. ---- */

/** Give the digest of `n` bytes at `data` in `hex`. */
static void sha_hex(const void *data, size_t n, char hex[ETUDE_DIGEST_HEX + 1])
{
    unsigned char sum[32];
    struct sha256 h;

    sha256_init(&h);
    sha256_update(&h, data, n);
    sha256_final(&h, sum);
    sha256_hex(sum, hex);
}

/** Write all of `buf`; 0 or -1. */
static int put_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    ssize_t w;

    while (n > 0) {
        w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/** Make file `path` hold `text`; 0 or -1. */
static int write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int r;

    if (fd < 0)
        return -1;
    r = put_all(fd, text, strlen(text));
    if (close(fd) != 0)
        r = -1;
    return r;
}

/**
 * Read up to `max - 1` bytes of `path` into `buf`, ended by NUL, and give
 * the length, or -1.
 */
static long read_text(const char *path, char *buf, size_t max)
{
    size_t n = 0;
    ssize_t r;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return -1;
    while (n + 1 < max && (r = read(fd, buf + n, max - 1 - n)) > 0)
        n += (size_t)r;
    close(fd);
    buf[n] = '\0';
    return (long)n;
}

/** Count the entries of directory `path` but `.` and `..`, or -1. */
static int entries(const char *path)
{
    struct dirent *d;
    DIR *dir = opendir(path);
    int n = 0;

    if (dir == NULL)
        return -1;
    while ((d = readdir(dir)) != NULL)
        if (strcmp(d->d_name, ".") != 0 && strcmp(d->d_name, "..") != 0)
            n++;
    closedir(dir);
    return n;
}

/** Count the entries of `sub` in store directory `dir`. */
static int in_store(const char *dir, const char *sub)
{
    char path[PATH_MAX];

    fix_format(path, sizeof(path), "%s/%s", dir, sub);
    return entries(path);
}

/** Read object `digest` of store `s` into `buf`, as read_text does. */
static long read_object(const struct etude_store *s, const char *digest, char *buf, size_t max)
{
    char path[PATH_MAX];

    if (etude_store_path(s, digest, path, sizeof(path)) != 0)
        return -1;
    return read_text(path, buf, max);
}

/** Count the lines of `path` that are `tag`. */
static int lines_of(const char *path, const char *tag)
{
    char buf[4096], *line, *save = NULL;
    int n = 0;

    if (read_text(path, buf, sizeof(buf)) < 0)
        return 0;
    for (line = strtok_r(buf, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save))
        n += strcmp(line, tag) == 0;
    return n;
}

/** Fill `buf` with the race's pattern. */
static void pattern(unsigned char *buf, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++)
        buf[i] = (unsigned char)(i * 131u + (i >> 12));
}

/** Lock all of `fd` for `type`, waiting when `wait`; 0 or -1. */
static int lock_file(int fd, short type, int wait)
{
    struct flock fl;

    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    return fcntl(fd, wait ? F_SETLKW : F_SETLK, &fl);
}

/** Wait for child `pid`, giving its status; -1 when it did not exit. */
static int reap(pid_t pid)
{
    int status;

    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ---- The recipes. ---- */

/** Signal through FIFO `path` that the recipe has begun. */
static void say_started(const char *path)
{
    int fd = open(path, O_WRONLY);

    if (fd < 0 || write(fd, "s", 1) != 1)
        _exit(120);
    close(fd);
}

/** Wait until the test releases its lock of `path`. */
static void wait_go(const char *path)
{
    int fd = open(path, O_RDONLY);

    if (fd < 0 || lock_file(fd, F_RDLCK, 1) != 0)
        _exit(121);
    close(fd);
}

/** Append `tag` and a newline to file `count`, unless it is `-`; 0 or -1. */
static int count_run(const char *count, const char *tag)
{
    int fd, r;

    if (strcmp(count, "-") == 0)
        return 0;
    fd = open(count, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0)
        return -1;
    r = dprintf(fd, "%s\n", tag) < 0 ? -1 : 0;
    close(fd);
    return r;
}

/** Make `out` of the `n` files `args`, and then `tag`, as cat(1) would. */
static int recipe_cat(const char *out, const char *tag, char **args, int n)
{
    char buf[4096];
    long got;
    int fd, i;

    fd = open(out, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
        return 123;
    for (i = 0; i < n; i++) {
        got = read_text(args[i], buf, sizeof(buf));
        if (got < 0 || put_all(fd, buf, (size_t)got) != 0)
            return 124;
    }
    if (put_all(fd, tag, strlen(tag)) != 0 || close(fd) != 0)
        return 125;
    return 0;
}

/** Leave a partial `out`, then exit 3, be killed or hang, as `mode` says. */
static int recipe_partial(const char *mode, const char *out, const char *ready)
{
    if (write_text(out, "partial") != 0)
        return 126;
    fprintf(stderr, "recipe: %s: on purpose\n", mode);
    if (strcmp(mode, "fail") == 0)
        return 3;
    if (strcmp(mode, "kill") == 0)
        raise(SIGKILL);
    if (ready == NULL || write_text(ready, "") != 0)
        return 126;
    for (;;)
        pause();
}

/** Say so on `started`, wait for `go`, and write the race's product. */
static int recipe_gate(const char *out, const char *started, const char *go)
{
    unsigned char *big = malloc(RACE_BYTES);
    size_t at;
    int fd;

    say_started(started);
    wait_go(go);
    fd = open(out, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (big == NULL || fd < 0)
        return 128;
    pattern(big, RACE_BYTES);
    for (at = 0; at < RACE_BYTES; at += 65536)
        if (put_all(fd, big + at, 65536) != 0)
            return 129;
    free(big);
    return close(fd) == 0 ? 0 : 130;
}

/**
 * Write the whole product once file `flag` exists; before that, make the
 * flag, leave a partial `out`, say so on `started` and wait for `go`.
 */
static int recipe_once(const char *out, const char *flag, const char *started, const char *go)
{
    if (access(flag, F_OK) == 0)
        return write_text(out, "whole product\n") == 0 ? 0 : 131;
    if (write_text(flag, "") != 0 || write_text(out, "partial") != 0)
        return 132;
    say_started(started);
    wait_go(go);
    return 1;
}

/**
 * Run recipe MODE: append TAG to COUNT, unless it is `-`, and make OUT as
 * the mode says from ARGS.
 */
static int recipe(int argc, char **argv)
{
    const char *mode = argv[2], *out = argv[5];
    char **args = argv + 6;
    int n = argc - 6;

    if (count_run(argv[3], argv[4]) != 0)
        return 122;
    if (strcmp(mode, "cat") == 0)
        return recipe_cat(out, argv[4], args, n);
    if (strcmp(mode, "fail") == 0 || strcmp(mode, "kill") == 0 || strcmp(mode, "hang") == 0)
        return recipe_partial(mode, out, n == 1 ? args[0] : NULL);
    if (strcmp(mode, "none") == 0)
        return 0;
    if (strcmp(mode, "link") == 0 && n == 1)
        return symlink(args[0], out) == 0 ? 0 : 127;
    if (strcmp(mode, "gate") == 0 && n == 2)
        return recipe_gate(out, args[0], args[1]);
    if (strcmp(mode, "once") == 0 && n == 3)
        return recipe_once(out, args[0], args[1], args[2]);
    return 133;
}

/* ---- Graphs. ---- */

/**
 * Give in `v` the argv of recipe `mode` with COUNT, TAG and `args`, a list
 * of at most four ended by NULL, or NULL for none.
 */
static char *const *argv_of(char *v[11], const char *mode, const char *count, const char *tag,
                            const char *const *args)
{
    int i;

    v[0] = self;
    v[1] = (char *)"--recipe";
    v[2] = (char *)mode;
    v[3] = (char *)count;
    v[4] = (char *)tag;
    v[5] = (char *)"${WORK}/out";
    for (i = 0; args != NULL && args[i] != NULL && i < 4; i++)
        v[6 + i] = (char *)args[i];
    v[6 + i] = NULL;
    return v;
}

/** Give a recipe node of `name`, `argv` and `inputs`, which may be NULL. */
static struct etude_prep_node node_of(const char *name, char *const *argv,
                                      const char *const *inputs)
{
    struct etude_prep_node n;

    memset(&n, 0, sizeof(n));
    n.name = name;
    n.argv = argv;
    n.output = "out";
    n.inputs = inputs;
    n.bound_us = LONG_BOUND;
    return n;
}

/** Add a recipe node to `g`, as node_of makes it; 0 or -1. */
static int add(struct etude_prep *g, const char *name, char *const *argv, const char *const *inputs)
{
    struct etude_prep_node n = node_of(name, argv, inputs);

    return etude_prep_add(g, &n);
}

/** Add a node for file `path` of the tree to `g`; 0 or -1. */
static int add_file(struct etude_prep *g, const char *name, const char *path)
{
    struct etude_prep_node n;

    memset(&n, 0, sizeof(n));
    n.name = name;
    n.output = path;
    n.bound_us = LONG_BOUND;
    return etude_prep_add(g, &n);
}

/** Run graph `g` in store `s`, `jobs` recipes at once, work in `scratch`. */
static int run(struct etude_prep *g, struct etude_store *s, const char *scratch, unsigned jobs)
{
    struct etude_prep_opts o;
    struct etude_exec *ex = etude_exec_new();
    int r;

    if (ex == NULL)
        return -2;
    memset(&o, 0, sizeof(o));
    o.scratch = scratch;
    o.jobs = jobs;
    o.output_max = 4096;
    o.grace_us = 100000;
    o.attempt = "artifact-test";
    r = etude_prep_run(g, s, ex, &o);
    etude_exec_free(ex);
    return r;
}

/**
 * Say whether node `name` of `g` ended in `state`, with its recipe run or
 * not as `ran` says.
 */
static int is(const struct etude_prep *g, const char *name, enum etude_prep_state state, int ran)
{
    const struct etude_prep_result *r = etude_prep_result(g, name);

    if (r == NULL)
        return 0;
    if (r->state != state || r->ran != ran)
        printf("etude-artifact-test: %s: state %d, ran %d: %s\n", name, (int)r->state, r->ran,
               r->why);
    return r->state == state && r->ran == ran;
}

/** Say whether node `name` of `g` was reused, its recipe not run. */
static int reused(const struct etude_prep *g, const char *name)
{
    return is(g, name, ETUDE_PREP_REUSED, 0);
}

/** Say whether node `name` of `g` was built, its recipe run. */
static int built(const struct etude_prep *g, const char *name)
{
    return is(g, name, ETUDE_PREP_BUILT, 1);
}

/* ---- The rows. ---- */

/** Publish objects, refuse a wrong digest, and give paths. */
static void test_put(void)
{
    const char *dir = fix_path("put");
    const char *src = fix_path("put-src");
    char d[ETUDE_DIGEST_HEX + 1], want[ETUDE_DIGEST_HEX + 1], other[ETUDE_DIGEST_HEX + 1];
    char path[PATH_MAX], buf[64];
    struct etude_store *s = etude_store_open(dir);
    struct stat a, b;
    uint64_t size = 0;

    CHECK(s != NULL, "put");
    if (s == NULL)
        return;
    sha_hex("hello\n", 6, want);
    sha_hex("other\n", 6, other);
    CHECK(write_text(src, "hello\n") == 0, "put");
    CHECK(etude_store_put(s, src, NULL, d, &size) == 0 && strcmp(d, want) == 0 && size == 6, "put");
    CHECK(etude_store_path(s, d, path, sizeof(path)) == 0 && stat(path, &a) == 0 &&
              (a.st_mode & 0222) == 0 && S_ISREG(a.st_mode),
          "put: read-only");
    CHECK(read_object(s, d, buf, sizeof(buf)) == 6 && strcmp(buf, "hello\n") == 0, "put");
    CHECK(etude_store_put(s, src, want, d, NULL) == 0 && stat(path, &b) == 0 &&
              a.st_ino == b.st_ino,
          "put: again");
    CHECK(etude_store_put(s, src, other, d, NULL) == -1 && errno == EBADMSG, "put: expected");
    CHECK(in_store(dir, "objects") == 1 && in_store(dir, "tmp") == 0, "put: expected");
    CHECK(etude_store_put(s, fix_path("put-absent"), NULL, d, NULL) == -1 && errno == ENOENT,
          "put: absent");
    CHECK(etude_store_check(s, want, &size) == 0 && size == 6, "put: check");
    CHECK(etude_store_check(s, other, NULL) == -1 && errno == ENOENT, "put: check");
    CHECK(etude_store_path(s, "xyz", path, sizeof(path)) == -1 && errno == EINVAL, "put: path");
    CHECK(etude_store_path(s, want, path, 8) == -1 && errno == ERANGE, "put: path");
    etude_store_close(s);
}

/** Give the times of the last modification and the last change of `st`. */
static void times_of(const struct stat *st, struct timespec *m, struct timespec *c)
{
#if defined(__APPLE__)
    *m = st->st_mtimespec;
    *c = st->st_ctimespec;
#else
    *m = st->st_mtim;
    *c = st->st_ctim;
#endif
}

/**
 * Write `text`, as long as the file's content, over the file at `path` in
 * place and give it back the modification time of `*was`, again until its
 * change time differs from `*was`'s, since two writes in one tick of the
 * file system's clock may leave it as it was, and at most a million times;
 * 0 when the file then differs from `*was` in its change time alone, or -1.
 */
static int overwrite(const char *path, const char *text, const struct stat *was)
{
    struct timespec times[2], m, c, was_m, was_c;
    struct stat st;
    int tries, fd, r;

    times_of(was, &was_m, &was_c);
    times[0].tv_sec = 0;
    times[0].tv_nsec = UTIME_OMIT;
    times[1] = was_m;
    for (tries = 0; tries < 1000000; tries++) {
        if ((fd = open(path, O_WRONLY)) < 0)
            return -1;
        r = put_all(fd, text, strlen(text));
        if (close(fd) != 0 || r != 0 || utimensat(AT_FDCWD, path, times, 0) != 0 ||
            stat(path, &st) != 0)
            return -1;
        times_of(&st, &m, &c);
        if (c.tv_sec != was_c.tv_sec || c.tv_nsec != was_c.tv_nsec)
            return st.st_ino == was->st_ino && st.st_size == was->st_size &&
                           m.tv_sec == was_m.tv_sec && m.tv_nsec == was_m.tv_nsec
                       ? 0
                       : -1;
    }
    return -1;
}

/** Say whether the file at `path` has a line that ends in ` NAME`. */
static int has_line(const char *path, const char *name)
{
    char buf[8192], *line, *save = NULL;
    size_t n = strlen(name), k;
    int found = 0;

    if (read_text(path, buf, sizeof(buf)) < 0)
        return 0;
    for (line = strtok_r(buf, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
        k = strlen(line);
        found |= k > n && line[k - n - 1] == ' ' && strcmp(line + k - n, name) == 0;
    }
    return found;
}

/**
 * Keep the digests of files in a digests file: a file hashed once is not read
 * again while its status stands, and a write that keeps its size and its
 * modification time has it hashed again.
 */
static void test_digests(void)
{
    const char *cache = fix_path("digests"), *src = fix_path("digests-src");
    char d[ETUDE_DIGEST_HEX + 1], first[ETUDE_DIGEST_HEX + 1], second[ETUDE_DIGEST_HEX + 1];
    unsigned long hashed = etude_store_hashed;
    struct stat st, was;

    sha_hex("first\n", 6, first);
    sha_hex("again\n", 6, second);
    etude_store_settle_ns = 0;
    CHECK(write_text(src, "first\n") == 0, "digests");
    CHECK(etude_digest_file(cache, src, &was, d) == 0 && strcmp(d, first) == 0 &&
              etude_store_hashed == hashed + 1,
          "digests: hashed");
    CHECK(etude_digest_file(cache, src, &st, d) == 0 && strcmp(d, first) == 0 &&
              etude_store_hashed == hashed + 1 && st.st_size == 6,
          "digests: reused");
    CHECK(overwrite(src, "again\n", &was) == 0,
          "digests: a write that keeps the size and the time");
    CHECK(etude_digest_file(cache, src, &st, d) == 0 && strcmp(d, second) == 0 &&
              etude_store_hashed == hashed + 2,
          "digests: a write that keeps the size and the time is hashed again");
    CHECK(etude_digest_file(NULL, src, &st, d) == 0 && strcmp(d, second) == 0 &&
              etude_store_hashed == hashed + 3,
          "digests: without a digests file, read every time");
    CHECK(etude_digest_file(cache, fix_path("put"), &st, d) == -1 && errno == EISDIR,
          "digests: a directory");
}

/** Keep no digest of a file changed less than the settling time before its
 * read, which is then hashed at every read. */
static void test_digests_settle(void)
{
    const char *cache = fix_path("digests"), *fresh = fix_path("digests-fresh");
    char d[ETUDE_DIGEST_HEX + 1];
    unsigned long hashed = etude_store_hashed;
    struct stat st;

    etude_store_settle_ns = 3600000000000LL;
    CHECK(write_text(fresh, "fresh\n") == 0, "digests");
    CHECK(etude_digest_file(cache, fresh, &st, d) == 0 &&
              etude_digest_file(cache, fresh, &st, d) == 0 && etude_store_hashed == hashed + 2,
          "digests: a file changed just before its read is hashed again");
    CHECK(!has_line(cache, fresh), "digests: a file changed just before its read is not kept");
    etude_store_settle_ns = 0;
}

/**
 * A store with a digests file publishes a file whose digest is kept without
 * hashing it, and not at all once its object is published.
 */
static void test_digests_put(void)
{
    const char *cache = fix_path("digests"), *src = fix_path("digests-src");
    char d[ETUDE_DIGEST_HEX + 1], other[ETUDE_DIGEST_HEX + 1], wrong[ETUDE_DIGEST_HEX + 1];
    char buf[64];
    struct etude_store *s = etude_store_open(fix_path("digests-store"));
    struct etude_store *s2 = etude_store_open(fix_path("digests-store2"));
    unsigned long hashed = etude_store_hashed, copied = etude_store_copied;
    uint64_t size = 0;

    CHECK(s != NULL && s2 != NULL && etude_store_digests(s, cache) == 0 &&
              etude_store_digests(s2, cache) == 0 && write_text(src, "other\n") == 0,
          "digests: store");
    if (s == NULL || s2 == NULL) {
        etude_store_close(s);
        etude_store_close(s2);
        return;
    }
    sha_hex("other\n", 6, other);
    sha_hex("first\n", 6, wrong);
    CHECK(etude_store_put(s, src, NULL, d, &size) == 0 && strcmp(d, other) == 0 && size == 6 &&
              etude_store_hashed == hashed + 1 && etude_store_copied == copied + 1,
          "digests: put: copied and hashed");
    size = 0;
    CHECK(etude_store_put(s, src, other, d, &size) == 0 && strcmp(d, other) == 0 && size == 6 &&
              etude_store_hashed == hashed + 1 && etude_store_copied == copied + 1,
          "digests: put: a kept file whose object is kept is not copied");
    CHECK(etude_store_put(s, src, wrong, d, NULL) == -1 && errno == EBADMSG,
          "digests: put: expected");
    CHECK(etude_store_put(s2, src, NULL, d, NULL) == 0 && strcmp(d, other) == 0 &&
              etude_store_hashed == hashed + 1 && etude_store_copied == copied + 2 &&
              read_object(s2, other, buf, sizeof(buf)) == 6 && strcmp(buf, "other\n") == 0,
          "digests: put: a kept file is copied to another store without being hashed");
    etude_store_close(s);
    etude_store_close(s2);
}

/**
 * A store with a digests file checks and copies a kept object without
 * reading it to hash it; a damaged object is read again and refused, and
 * the line of a file gone or changed is dropped.
 */
static void test_digests_objects(void)
{
    const char *cache = fix_path("digests"), *src = fix_path("digests-src");
    const char *copy = fix_path("digests-copy");
    char d[ETUDE_DIGEST_HEX + 1], other[ETUDE_DIGEST_HEX + 1], path[PATH_MAX], buf[64];
    struct etude_store *s = etude_store_open(fix_path("digests-store"));
    unsigned long hashed = etude_store_hashed;
    uint64_t size = 0;
    struct stat st;

    CHECK(s != NULL && etude_store_digests(s, cache) == 0, "digests: objects");
    if (s == NULL)
        return;
    sha_hex("other\n", 6, other);
    CHECK(etude_store_check(s, other, &size) == 0 && size == 6 && etude_store_hashed == hashed,
          "digests: check: a kept object is not read");
    CHECK(etude_store_copy(s, other, copy) == 0 && etude_store_hashed == hashed &&
              read_text(copy, buf, sizeof(buf)) == 6 && strcmp(buf, "other\n") == 0,
          "digests: copy: a kept object is copied without being hashed");
    CHECK(etude_store_path(s, other, path, sizeof(path)) == 0 && stat(path, &st) == 0 &&
              chmod(path, 0644) == 0 && overwrite(path, "OTHER\n", &st) == 0,
          "digests: an object damaged");
    CHECK(etude_store_check(s, other, NULL) == -1 && errno == EBADMSG &&
              etude_store_hashed == hashed + 1,
          "digests: check: a damaged object is read again and refused");
    CHECK(unlink(copy) == 0 && unlink(src) == 0 && write_text(src, "first\n") == 0 &&
              etude_digest_file(cache, src, &st, d) == 0 && has_line(cache, src) &&
              !has_line(cache, path),
          "digests: the line of a file changed or gone is dropped");
    etude_store_close(s);
}

/** A fresh object's digest is not kept, even though it was made read-only. */
static void test_digests_object_settle(void)
{
    const char *cache = fix_path("settle-cache"), *src = fix_path("settle-source");
    struct etude_store *s = etude_store_open(fix_path("settle-store"));
    char digest[ETUDE_DIGEST_HEX + 1], path[PATH_MAX] = "";

    etude_store_settle_ns = 3600000000000LL;
    CHECK(s != NULL, "digests: fresh object store");
    if (s != NULL) {
        CHECK(etude_store_digests(s, cache) == 0 && write_text(src, "fresh\n") == 0 &&
                  etude_store_put(s, src, NULL, digest, NULL) == 0 &&
                  etude_store_path(s, digest, path, sizeof(path)) == 0,
              "digests: publish fresh object");
        CHECK(!has_line(cache, path), "digests: fresh object is not kept");
        CHECK(chmod(path, 0644) == 0 && write_text(path, "OTHER\n") == 0 &&
                  etude_store_check(s, digest, NULL) == -1 && errno == EBADMSG,
              "digests: immediate object damage is refused");
        etude_store_close(s);
    }
    etude_store_settle_ns = 0;
}

/** Entries made before objects obeyed the settling rule are read again. */
static void test_digests_old_format(void)
{
    const char *cache = fix_path("old-cache"), *src = fix_path("old-source");
    char digest[ETUDE_DIGEST_HEX + 1], line[8192] = "";
    struct stat st;
    unsigned long hashed;

    CHECK(write_text(src, "old\n") == 0 && etude_digest_file(cache, src, &st, digest) == 0 &&
              read_text(cache, line, sizeof(line)) > 2 && strncmp(line, "2 ", 2) == 0,
          "digests: versioned entry");
    if (strncmp(line, "2 ", 2))
        return;
    hashed = etude_store_hashed;
    CHECK(write_text(cache, line + 2) == 0 && etude_digest_file(cache, src, &st, digest) == 0 &&
              etude_store_hashed == hashed + 1,
          "digests: old entry is read again");
}

/** Fill the `n` bytes of `buf` with a sequence that repeats no block. */
static void fill(unsigned char *buf, size_t n)
{
    unsigned long x = 12345;
    size_t i;

    for (i = 0; i < n; i++) {
        x = x * 1103515245UL + 12345UL;
        buf[i] = (unsigned char)(x >> 16);
    }
}

/**
 * Hash on the processor's SHA instructions what the portable code hashes
 * to the same digest: files of lengths at the edges of a block, of the
 * padding and of a chunk read, each read with the instructions and without.
 */
static void test_digests_portable(void)
{
    static const size_t lengths[] = {0,
                                     1,
                                     55,
                                     56,
                                     63,
                                     64,
                                     65,
                                     119,
                                     120,
                                     127,
                                     128,
                                     129,
                                     4095,
                                     65536,
                                     65537,
                                     1048575,
                                     1048576,
                                     1048641,
                                     3 * 1048576 + 7};
    const char *src = fix_path("portable-src");
    char want[ETUDE_DIGEST_HEX + 1], hw[ETUDE_DIGEST_HEX + 1], sw[ETUDE_DIGEST_HEX + 1];
    unsigned char *buf = malloc(3 * 1048576 + 7);
    struct stat st;
    size_t i;
    int fd, r;

    CHECK(buf != NULL, "portable: memory");
    if (buf == NULL)
        return;
    fill(buf, 3 * 1048576 + 7);
    for (i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        sha_hex(buf, lengths[i], want);
        fd = open(src, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        r = fd >= 0 && put_all(fd, buf, lengths[i]) == 0;
        if (fd >= 0 && close(fd) != 0)
            r = 0;
        etude_store_portable = 0;
        r = r && etude_digest_file(NULL, src, &st, hw) == 0;
        etude_store_portable = 1;
        r = r && etude_digest_file(NULL, src, &st, sw) == 0;
        etude_store_portable = 0;
        CHECK(r && strcmp(hw, want) == 0 && strcmp(sw, want) == 0,
              "portable: a file's digest is the portable one");
        if (!r || strcmp(hw, want) != 0 || strcmp(sw, want) != 0)
            printf("etude-artifact-test: length %lu: %s %s %s\n", (unsigned long)lengths[i], want,
                   hw, sw);
    }
    free(buf);
}

/**
 * Keep the digests of one process in a table, read once and written once:
 * a digest it reads is found in the table without another read and is in
 * the file only once the table is closed, with the line another process
 * wrote meanwhile, and a file written since keeps no digest.
 */
static void test_digests_table(void)
{
    const char *cache = fix_path("table-digests"), *a = fix_path("table-a");
    const char *b = fix_path("table-b"), *other = fix_path("table-other");
    char d[ETUDE_DIGEST_HEX + 1], ha[ETUDE_DIGEST_HEX + 1], hb[ETUDE_DIGEST_HEX + 1];
    struct etude_digests *t;
    unsigned long hashed;
    struct stat st, was;

    etude_store_settle_ns = 0;
    sha_hex("table a\n", 8, ha);
    sha_hex("table b\n", 8, hb);
    CHECK(write_text(a, "table a\n") == 0 && write_text(b, "table b\n") == 0 &&
              write_text(other, "other\n") == 0,
          "table: files");
    t = etude_digests_open(cache);
    CHECK(t != NULL, "table: open");
    if (t == NULL)
        return;
    hashed = etude_store_hashed;
    CHECK(etude_digests_find(t, a, &st, d) == 1, "table: an empty table keeps nothing");
    CHECK(etude_digests_file(t, a, &st, d) == 0 && strcmp(d, ha) == 0 &&
              etude_store_hashed == hashed + 1,
          "table: read");
    CHECK(etude_digests_file(t, a, &st, d) == 0 && strcmp(d, ha) == 0 &&
              etude_store_hashed == hashed + 1,
          "table: a digest it keeps is not read again");
    CHECK(!has_line(cache, a), "table: the file is not written before the table closes");
    CHECK(etude_digest_file(cache, other, &st, d) == 0 && has_line(cache, other),
          "table: another process keeps a digest meanwhile");
    CHECK(etude_digests_file(t, b, &st, d) == 0 && strcmp(d, hb) == 0, "table: a second file");
    etude_digests_close(t);
    CHECK(has_line(cache, a) && has_line(cache, b) && has_line(cache, other),
          "table: closing writes its digests and keeps the other's");
    hashed = etude_store_hashed;
    CHECK(etude_digest_file(cache, b, &st, d) == 0 && strcmp(d, hb) == 0 &&
              etude_store_hashed == hashed,
          "table: a digest written is used");
    CHECK(stat(a, &was) == 0 && overwrite(a, "TABLE A\n", &was) == 0,
          "table: a write that keeps the size and the time");
    t = etude_digests_open(cache);
    CHECK(t != NULL && etude_digests_find(t, a, &st, d) == 1,
          "table: the digest of a file written since is not used");
    etude_digests_close(t);
}

/**
 * Write a file with etude_digest_write from a pipe, in pieces that leave a
 * block part filled: the file has what was written, its digest is the
 * portable one and is kept without the file being read, and a write of the
 * file since has it read again.
 */
static void test_digests_write(void)
{
    const char *cache = fix_path("write-digests"), *dst = fix_path("write-image");
    char want[ETUDE_DIGEST_HEX + 1], d[ETUDE_DIGEST_HEX + 1];
    size_t n = 3 * 1048576 + 1001, at, k;
    unsigned char *buf = calloc(1, n);
    unsigned long hashed;
    struct stat st, was;
    char *real;
    int fds[2], status = -1;
    pid_t pid;

    CHECK(buf != NULL && pipe(fds) == 0, "write: pipe");
    if (buf == NULL)
        return;
    fill(buf, 1048576);
    fill(buf + 2 * 1048576, 1001);
    sha_hex(buf, n, want);
    if ((pid = fork()) == 0) {
        close(fds[0]);
        for (at = 0; at < n; at += k) {
            k = n - at < 1000 ? n - at : 1000;
            if (put_all(fds[1], buf + at, k) != 0)
                _exit(1);
        }
        _exit(0);
    }
    close(fds[1]);
    CHECK(pid > 0 && etude_digest_write(cache, fds[0], dst, d) == 0 && strcmp(d, want) == 0,
          "write: the digest of what was written");
    close(fds[0]);
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && status == 0, "write: the writer");
    /* The writer keeps the file under its real path; /tmp is a link on macOS. */
    real = realpath(dst, NULL);
    CHECK(stat(dst, &st) == 0 && (size_t)st.st_size == n && real != NULL && has_line(cache, real),
          "write: the file and its line");
    free(real);
    hashed = etude_store_hashed;
    CHECK(etude_digest_file(cache, dst, &st, d) == 0 && strcmp(d, want) == 0 &&
              etude_store_hashed == hashed,
          "write: its digest is used without a read");
    CHECK(stat(dst, &was) == 0 && overwrite(dst, "changed", &was) == 0 &&
              etude_digest_file(cache, dst, &st, d) == 0 && strcmp(d, want) != 0 &&
              etude_store_hashed == hashed + 1,
          "write: a file written since is read again");
    free(buf);
}

/** Start a child that writes what pipe `in` gives to `dst` with
 * etude_digest_write and exits 0 when the digest is `want`; its pid. */
static pid_t digest_writer(const char *cache, int in[2], int other, const char *dst,
                           const char *want)
{
    char d[ETUDE_DIGEST_HEX + 1];
    pid_t pid = fork();

    if (pid == 0) {
        close(in[1]);
        close(other);
        _exit(etude_digest_write(cache, in[0], dst, d) == 0 && strcmp(d, want) == 0 ? 0 : 1);
    }
    close(in[0]);
    return pid;
}

/**
 * Write one file with two etude_digest_write calls at once, fed in turn
 * through pipes larger than a pipe holds, so that each has its scratch file
 * before either renames: both succeed, the file has the bytes of the writer
 * that renamed last and the digests file gives that writer's digest, and no
 * scratch file is left.
 */
static void test_digests_two_writers(void)
{
    const char *cache = fix_path("two-digests"), *dst = fix_path("two-image");
    char want[2][ETUDE_DIGEST_HEX + 1], d[ETUDE_DIGEST_HEX + 1];
    size_t n[2] = {1048576, 1048576 + 7}, half = 262144, k = strlen("two-image.");
    unsigned char *buf[2];
    unsigned long hashed;
    int fds[2][2], status[2] = {-1, -1}, i, left = 0;
    struct dirent *e;
    struct stat st;
    pid_t pid[2];
    DIR *dir;

    buf[0] = malloc(n[0]);
    buf[1] = malloc(n[1]);
    CHECK(buf[0] != NULL && buf[1] != NULL && pipe(fds[0]) == 0 && pipe(fds[1]) == 0,
          "two writers: pipes");
    if (buf[0] == NULL || buf[1] == NULL) {
        free(buf[0]);
        free(buf[1]);
        return;
    }
    fill(buf[0], n[0]);
    memset(buf[1], 'b', n[1]);
    for (i = 0; i < 2; i++)
        sha_hex(buf[i], n[i], want[i]);
    pid[0] = digest_writer(cache, fds[0], fds[1][1], dst, want[0]);
    pid[1] = digest_writer(cache, fds[1], fds[0][1], dst, want[1]);
    CHECK(pid[0] > 0 && pid[1] > 0 && put_all(fds[0][1], buf[0], half) == 0 &&
              put_all(fds[1][1], buf[1], half) == 0,
          "two writers: both under way");
    for (i = 0; i < 2; i++) {
        CHECK(put_all(fds[i][1], buf[i] + half, n[i] - half) == 0, "two writers: the rest");
        close(fds[i][1]);
        CHECK(pid[i] > 0 && waitpid(pid[i], &status[i], 0) == pid[i] && status[i] == 0,
              "two writers: each writes its file and gives its digest");
    }
    hashed = etude_store_hashed;
    CHECK(stat(dst, &st) == 0 && (size_t)st.st_size == n[1] &&
              etude_digest_file(cache, dst, &st, d) == 0 && strcmp(d, want[1]) == 0 &&
              etude_store_hashed == hashed,
          "two writers: the file and the digest kept are the last writer's");
    if ((dir = opendir(fix_dir())) != NULL) {
        while ((e = readdir(dir)) != NULL)
            left |= strncmp(e->d_name, "two-image.", k) == 0;
        closedir(dir);
    }
    CHECK(dir != NULL && !left, "two writers: no scratch file is left");
    free(buf[0]);
    free(buf[1]);
}

/** Give the allocation of `path`, requiring its expected length; 0 or -1. */
static int disk_bytes(const char *path, uint64_t length, uint64_t *bytes)
{
    struct stat st;

    if (stat(path, &st) != 0 || st.st_size < 0 || (uint64_t)st.st_size != length)
        return -1;
    *bytes = (uint64_t)st.st_blocks * 512u;
    return 0;
}

/**
 * Give in `hex` the digest of the sparse file, zeros with `data` at
 * SPARSE_AT, hashed from memory.
 */
static void sparse_hex(const unsigned char *data, char hex[ETUDE_DIGEST_HEX + 1])
{
    static const unsigned char zero[4096];
    unsigned char sum[32];
    struct sha256 h;
    size_t n;

    sha256_init(&h);
    for (n = 0; n < SPARSE_AT; n += sizeof(zero))
        sha256_update(&h, zero, SPARSE_AT - n < sizeof(zero) ? SPARSE_AT - n : sizeof(zero));
    sha256_update(&h, data, SPARSE_DATA);
    for (n = SPARSE_AT + SPARSE_DATA; n < SPARSE_BYTES; n += sizeof(zero))
        sha256_update(&h, zero, SPARSE_BYTES - n < sizeof(zero) ? SPARSE_BYTES - n : sizeof(zero));
    sha256_final(&h, sum);
    sha256_hex(sum, hex);
}

/**
 * Publish and copy a sparse file whose data begin inside a chunk and which
 * ends in a hole: the object and the copy keep its holes, its size and its
 * digest.
 */
static void test_sparse(void)
{
    const char *dir = fix_path("sparse");
    const char *src = fix_path("sparse-src");
    const char *copy = fix_path("sparse-copy");
    unsigned char *data = malloc(SPARSE_DATA);
    char d[ETUDE_DIGEST_HEX + 1], want[ETUDE_DIGEST_HEX + 1], path[PATH_MAX];
    struct etude_store *s = etude_store_open(dir);
    uint64_t size = 0, bytes = 0;
    int fd = -1;

    CHECK(data != NULL && s != NULL, "sparse");
    if (data == NULL || s == NULL) {
        free(data);
        etude_store_close(s);
        return;
    }
    pattern(data, SPARSE_DATA);
    sparse_hex(data, want);
    fd = open(src, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0 && lseek(fd, SPARSE_AT, SEEK_SET) == SPARSE_AT &&
              put_all(fd, data, SPARSE_DATA) == 0 && ftruncate(fd, SPARSE_BYTES) == 0 &&
              close(fd) == 0,
          "sparse");
    CHECK(disk_bytes(src, SPARSE_BYTES, &bytes) == 0 && bytes < SPARSE_BYTES / 8,
          "sparse: the fixture's file system keeps holes");
    CHECK(etude_store_put(s, src, NULL, d, &size) == 0 && strcmp(d, want) == 0 &&
              size == SPARSE_BYTES,
          "sparse: put: the digest of the whole file");
    CHECK(etude_store_path(s, want, path, sizeof(path)) == 0 &&
              disk_bytes(path, SPARSE_BYTES, &bytes) == 0,
          "sparse: put: whole");
    CHECK(bytes < SPARSE_BYTES / 8, "sparse: put: the object keeps the holes");
    CHECK(etude_store_copy(s, want, copy) == 0 && disk_bytes(copy, SPARSE_BYTES, &bytes) == 0,
          "sparse: copy: whole");
    CHECK(bytes < SPARSE_BYTES / 8, "sparse: copy: the copy keeps the holes");
    CHECK(etude_store_put(s, copy, want, d, NULL) == 0, "sparse: copy: the same content");
    etude_store_close(s);
    free(data);
}

/** A rejected range leaves both an existing copy and its source unchanged. */
static void range_refused(const char *src, const char *copy, uint64_t offset, uint64_t length)
{
    const char *bad = fix_path("range-bad");
    struct stat st;
    char buf[9];
    int fd;
    CHECK(store_copy_range(src, offset + length - 1, 2, bad) < 0 &&
              store_copy_range(src, UINT64_MAX, 1, bad) < 0 && access(bad, F_OK) != 0,
          "range: out-of-bounds requests create nothing");
    CHECK(store_copy_range(src, offset, length, copy) < 0 &&
              read_text(copy, buf, sizeof(buf)) == 8 && !memcmp(buf, "abc\0\0\0\0\0", 8),
          "range: an existing destination is unchanged");
    fd = open(src, O_RDONLY);
    CHECK(fd >= 0 && lseek(fd, (off_t)offset, SEEK_SET) == (off_t)offset && read(fd, buf, 3) == 3 &&
              !memcmp(buf, "abc", 3) && fstat(fd, &st) == 0 &&
              (uint64_t)st.st_size == offset + length,
          "range: the source disk is unchanged");
    if (fd >= 0)
        CHECK(close(fd) == 0, "range: close source");
}

/** Copy a bounded sparse volume past 4 GiB, without its surrounding disk.
 * Its trailing gap exceeds APFS's small-gap zero fill, as in test_sparse. */
static void test_range(void)
{
    const char *src = fix_path("range-src"), *copy = fix_path("range-copy");
    uint64_t offset = UINT64_C(4294967296) + 7, length = 64u << 20, bytes;
    struct stat st;
    char buf[9];
    int fd = open(src, O_WRONLY | O_CREAT | O_EXCL, 0600), ready;

    CHECK(fd >= 0, "range: create private source");
    if (fd < 0)
        return;
    ready = lseek(fd, (off_t)offset, SEEK_SET) == (off_t)offset && put_all(fd, "abc", 3) == 0 &&
            ftruncate(fd, (off_t)(offset + length)) == 0;
    CHECK(close(fd) == 0 && ready, "range: data past 4 GiB and a trailing hole");
    if (!ready)
        return;
    CHECK(store_copy_range(src, offset, length, copy) == 0 && stat(copy, &st) == 0 &&
              (uint64_t)st.st_size == length && read_text(copy, buf, sizeof(buf)) == 8 &&
              !memcmp(buf, "abc\0\0\0\0\0", 8),
          "range: exact offset, bytes and length");
    CHECK(disk_bytes(copy, length, &bytes) == 0 && bytes < length / 2,
          "range: the private volume keeps its trailing hole");
    range_refused(src, copy, offset, length);
}

/** Give the `n` bytes at `p` as a number, most significant first. */
static uint64_t get_be(const unsigned char *p, int n)
{
    uint64_t v = 0;

    while (n-- > 0)
        v = v << 8 | *p++;
    return v;
}

/* An overlay row: its disk's size, its cluster size, the entries of its L1
 * table and its clusters in all, and its base's format, `raw` or `qcow2`. */
struct overlay_want {
    uint64_t size;
    uint32_t cluster;
    uint64_t l1, clusters;
    const char *format;
    const char *row;
};

/**
 * Write `path` as the first 1000 bytes of a qcow2 image of `version`, whose
 * backing file's name begins at `backing`, or 0 for none, and whose disk has
 * `size` bytes, with the rest of the header zero.
 */
static int write_qcow2(const char *path, uint32_t version, uint64_t backing, uint64_t size)
{
    unsigned char head[1000];
    int fd, r, i;

    memset(head, 0, sizeof(head));
    memcpy(head, "QFI\xfb", 4);
    for (i = 0; i < 4; i++)
        head[4 + i] = (unsigned char)(version >> (24 - 8 * i));
    for (i = 0; i < 8; i++) {
        head[8 + i] = (unsigned char)(backing >> (56 - 8 * i));
        head[24 + i] = (unsigned char)(size >> (56 - 8 * i));
    }
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    r = put_all(fd, head, sizeof(head));
    if (close(fd) != 0)
        r = -1;
    return r;
}

/**
 * Check the overlay `buf` of `len` bytes over the object at `real` as `w`
 * says: its header, its backing file and that file's format, an empty L1
 * table, and its length, each cluster counted once by its refcounts.
 */
static void overlay_check(const unsigned char *buf, uint64_t len, const char *real,
                          const struct overlay_want *w)
{
    uint64_t size = w->size, l1 = w->l1, clusters = w->clusters;
    uint32_t cluster = w->cluster;
    const char *row = w->row;
    uint64_t n = strlen(real), table = get_be(buf + 48, 8), l1_at = get_be(buf + 40, 8), i;
    uint64_t block = table + 8 <= len ? get_be(buf + table, 8) : len;

    CHECK(len == clusters * cluster, row);
    CHECK(memcmp(buf, "QFI\xfb", 4) == 0 && get_be(buf + 4, 4) == 3, row);
    CHECK(get_be(buf + 20, 4) < 32 && ((uint64_t)1 << get_be(buf + 20, 4)) == cluster &&
              get_be(buf + 24, 8) == size,
          row);
    CHECK(get_be(buf + 16, 4) == n && memcmp(buf + get_be(buf + 8, 8), real, n) == 0, row);
    CHECK(get_be(buf + 104, 4) == 0xe2792acau && get_be(buf + 108, 4) == strlen(w->format) &&
              memcmp(buf + 112, w->format, strlen(w->format)) == 0 &&
              get_be(buf + 112 + strlen(w->format), 8 - strlen(w->format)) == 0 &&
              get_be(buf + 120, 8) == 0,
          row);
    CHECK(get_be(buf + 36, 4) == l1 && l1_at % cluster == 0 && l1_at + l1 * 8 <= len, row);
    CHECK(get_be(buf + 96, 4) == 4 && get_be(buf + 100, 4) == 104 && get_be(buf + 56, 4) >= 1, row);
    for (i = 0; i < l1 && l1_at + l1 * 8 <= len; i++)
        CHECK(get_be(buf + l1_at + i * 8, 8) == 0, row);
    for (i = 0; block + (i + 1) * 2 <= len && i < cluster / 2; i++)
        CHECK(get_be(buf + block + i * 2, 2) == (i < clusters), row);
}

/**
 * Store a raw disk of `w->size` bytes, or the header of a qcow2 image of a
 * disk of that size, make an overlay of it, and check the overlay as
 * overlay_check does, and that the object stays read-only.
 */
static void overlay_row(struct etude_store *s, const struct overlay_want *w)
{
    const char *row = w->row;
    const char *src = fix_path("overlay-src"), *dest = fix_path("overlay.qcow2");
    char d[ETUDE_DIGEST_HEX + 1], path[PATH_MAX], real[PATH_MAX];
    unsigned char *buf = NULL;
    struct stat st;
    long got;
    int fd;

    unlink(dest);
    if (strcmp(w->format, "qcow2") == 0) {
        CHECK(write_qcow2(src, 3, 0, w->size) == 0, row);
    } else {
        fd = open(src, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        CHECK(fd >= 0 && ftruncate(fd, (off_t)w->size) == 0 && put_all(fd, "base", 4) == 0, row);
        if (fd >= 0)
            close(fd);
    }
    if (etude_store_put(s, src, NULL, d, NULL) != 0 || etude_store_path(s, d, path, sizeof(path)) ||
        realpath(path, real) == NULL || etude_store_overlay(s, d, w->cluster, dest) != 0 ||
        stat(dest, &st) != 0 || (buf = malloc((size_t)st.st_size)) == NULL ||
        (fd = open(dest, O_RDONLY)) < 0) {
        printf("etude-artifact-test: fail: %s: no overlay: %s\n", row, strerror(errno));
        fails++;
        free(buf);
        return;
    }
    got = read(fd, buf, (size_t)st.st_size);
    close(fd);
    CHECK(got == (long)st.st_size, row);
    if (got == (long)st.st_size && st.st_size >= 128)
        overlay_check(buf, (uint64_t)st.st_size, real, w);
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0222) == 0, row);
    free(buf);
}

/**
 * An overlay of an object is an empty qcow2 file whose backing file is the
 * object's path, of the object's size, in each cluster size from QEMU's
 * least to its greatest, or of the virtual size of a qcow2 object, whose
 * format it names; and the overlays QEMU cannot open, of a cluster size it
 * lacks, of a disk not a whole number of sectors, over an object absent, a
 * qcow2 object of another version or with a backing file of its own, or over
 * a `dest` that exists, are refused.
 */
static void test_overlay(void)
{
    static const struct overlay_want rows[] = {
        {1u << 20, 4096, 1, 4, "raw", "overlay: 1 MiB in 4 KiB clusters"},
        {(4u << 20) + 512, 4096, 3, 4, "raw", "overlay: past an L2 table's span"},
        {4u << 20, 512, 128, 5, "raw", "overlay: 512-byte clusters, two of L1"},
        {8u << 20, 2u << 20, 1, 4, "raw", "overlay: 2 MiB clusters"},
        {UINT64_C(250059350016), 4096, 119238, 236, "qcow2", "overlay: a qcow2 base of 250 GB"}};
    const char *src = fix_path("overlay-src"), *dest = fix_path("overlay.qcow2");
    char d[ETUDE_DIGEST_HEX + 1];
    struct etude_store *s = etude_store_open(fix_path("overlay"));
    struct stat st;
    size_t i;

    CHECK(s != NULL, "overlay");
    if (s == NULL)
        return;
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
        overlay_row(s, &rows[i]);
    CHECK(etude_store_put(s, src, NULL, d, NULL) == 0 &&
              etude_store_overlay(s, d, 4096, dest) == -1 && errno == EEXIST,
          "overlay: exists");
    CHECK(unlink(dest) == 0, "overlay");
    CHECK(etude_store_overlay(s, d, 1000, dest) == -1 && errno == EINVAL, "overlay: 1000 bytes");
    CHECK(etude_store_overlay(s, d, 256, dest) == -1 && errno == EINVAL, "overlay: 256 bytes");
    CHECK(etude_store_overlay(s, d, 4u << 20, dest) == -1 && errno == EINVAL, "overlay: 4 MiB");
    CHECK(etude_store_overlay(s, "xyz", 4096, dest) == -1 && errno == EINVAL, "overlay: digest");
    sha_hex("absent", 6, d);
    CHECK(etude_store_overlay(s, d, 4096, dest) == -1 && errno == ENOENT, "overlay: absent");
    CHECK(write_text(src, "not a sector\n") == 0 && etude_store_put(s, src, NULL, d, NULL) == 0 &&
              etude_store_overlay(s, d, 4096, dest) == -1 && errno == EINVAL,
          "overlay: a partial sector");
    CHECK(write_qcow2(src, 1, 0, 1u << 20) == 0 && etude_store_put(s, src, NULL, d, NULL) == 0 &&
              etude_store_overlay(s, d, 4096, dest) == -1 && errno == EINVAL,
          "overlay: a qcow2 base of version 1");
    CHECK(write_qcow2(src, 3, 512, 1u << 20) == 0 && etude_store_put(s, src, NULL, d, NULL) == 0 &&
              etude_store_overlay(s, d, 4096, dest) == -1 && errno == EINVAL,
          "overlay: a qcow2 base with a backing file");
    CHECK(write_qcow2(src, 2, 0, (1u << 20) + 100) == 0 &&
              etude_store_put(s, src, NULL, d, NULL) == 0 &&
              etude_store_overlay(s, d, 4096, dest) == -1 && errno == EINVAL,
          "overlay: a qcow2 base of a partial sector");
    CHECK(stat(dest, &st) == -1 && errno == ENOENT, "overlay: a refused one is not left");
    etude_store_close(s);
}

/**
 * Kill one writer while it copies from a FIFO and keep another waiting: the
 * first leaves no object and its partial file is swept, and the second
 * keeps its partial file and publishes. A write to a FIFO returns once its
 * writer has read most of it, so its partial file has data by then.
 */
static void test_interrupt(void)
{
    const char *dir = fix_path("interrupt");
    const char *fifo[2] = {fix_path("interrupt-a"), fix_path("interrupt-b")};
    unsigned char *data = malloc(2 * HALF);
    char d[ETUDE_DIGEST_HEX + 1], want[ETUDE_DIGEST_HEX + 1];
    struct etude_store *s = etude_store_open(dir);
    size_t removed = 99;
    pid_t kid[2];
    int fd[2], i;

    CHECK(data != NULL && s != NULL, "interrupt");
    etude_store_close(s);
    if (data == NULL || s == NULL) {
        free(data);
        return;
    }
    pattern(data, 2 * HALF);
    for (i = 0; i < 2; i++) {
        CHECK(mkfifo(fifo[i], 0600) == 0, "interrupt");
        kid[i] = fork();
        if (kid[i] == 0) {
            struct etude_store *own = etude_store_open(dir);

            _exit(own != NULL && etude_store_put(own, fifo[i], NULL, d, NULL) == 0 ? 0 : 1);
        }
        fd[i] = open(fifo[i], O_WRONLY);
        CHECK(fd[i] >= 0 && put_all(fd[i], data, HALF) == 0, "interrupt");
    }
    kill(kid[0], SIGKILL);
    reap(kid[0]);
    close(fd[0]);
    CHECK(in_store(dir, "tmp") == 2 && in_store(dir, "objects") == 0, "interrupt: partial");
    s = etude_store_open(dir);
    CHECK(s != NULL && etude_store_sweep(s, &removed) == 0 && removed == 0, "interrupt: sweep");
    CHECK(in_store(dir, "tmp") == 1 && in_store(dir, "objects") == 0,
          "interrupt: the writer that runs keeps its file");
    CHECK(put_all(fd[1], data + HALF, HALF) == 0, "interrupt");
    close(fd[1]);
    CHECK(reap(kid[1]) == 0, "interrupt: the writer that runs publishes");
    sha_hex(data, 2 * HALF, want);
    CHECK(s != NULL && etude_store_check(s, want, NULL) == 0, "interrupt: whole");
    CHECK(in_store(dir, "tmp") == 0 && in_store(dir, "objects") == 1, "interrupt: whole");
    etude_store_close(s);
    free(data);
}

/* The fixtures of test_graph and the keys its first run gave. */
struct graph_row {
    struct etude_store *s;
    const char *dir, *src, *count, *scratch;
    char bkey[ETUDE_DIGEST_HEX + 1];
    char ckey[ETUDE_DIGEST_HEX + 1];
};

/**
 * Build the graph of test_graph, with `bkey` as the key text of node b: a
 * file, a and b on it, c on both, and a fresh node f.
 */
static struct etude_prep *graph_basic(const struct graph_row *t, const char *bkey)
{
    static const char *const on_src[] = {"src", NULL};
    static const char *const on_ab[] = {"a", "b", NULL};
    const char *key[] = {bkey, NULL};
    struct etude_prep *g = etude_prep_new();
    char *va[11], *vb[11], *vc[11], *vf[11];
    struct etude_prep_node b, f;

    b = node_of("b", argv_of(vb, "cat", t->count, "b", ARGS("${IN:src}")), on_src);
    b.key = key;
    f = node_of("f", argv_of(vf, "cat", t->count, "f", NULL), NULL);
    f.fresh = 1;
    if (g == NULL || add_file(g, "src", t->src) != 0 ||
        add(g, "a", argv_of(va, "cat", t->count, "a", ARGS("${IN:src}")), on_src) != 0 ||
        etude_prep_add(g, &b) != 0 ||
        add(g, "c", argv_of(vc, "cat", t->count, "c", ARGS("${IN:a}", "${IN:b}")), on_ab) != 0 ||
        etude_prep_add(g, &f) != 0) {
        etude_prep_free(g);
        return NULL;
    }
    return g;
}

/** Build the graph for the first time, and keep the keys of b and c. */
static void graph_first(struct graph_row *t)
{
    struct etude_prep *g = graph_basic(t, "recipe 1");
    const struct etude_prep_result *b, *c;
    char buf[256];

    CHECK(g != NULL && run(g, t->s, t->scratch, 3) == 0, "graph: first");
    if (g == NULL)
        return;
    CHECK(is(g, "src", ETUDE_PREP_BUILT, 0) && built(g, "a") && built(g, "b") && built(g, "c") &&
              built(g, "f"),
          "graph: first");
    b = etude_prep_result(g, "b");
    c = etude_prep_result(g, "c");
    CHECK(read_object(t->s, c->digest, buf, sizeof(buf)) > 0 &&
              strcmp(buf, "source one\nasource one\nbc") == 0 && c->work == NULL,
          "graph: first: c");
    CHECK(lines_of(t->count, "a") == 1 && lines_of(t->count, "b") == 1 &&
              lines_of(t->count, "c") == 1 && lines_of(t->count, "f") == 1,
          "graph: first: each recipe ran once");
    CHECK(entries(t->scratch) == 0 && in_store(t->dir, "tmp") == 0, "graph: first: work removed");
    memcpy(t->bkey, b->key, sizeof(t->bkey));
    memcpy(t->ckey, c->key, sizeof(t->ckey));
    etude_prep_free(g);
}

/** Reuse every node of the graph but the fresh one. */
static void graph_again(const struct graph_row *t)
{
    struct etude_prep *g = graph_basic(t, "recipe 1");

    CHECK(g != NULL && run(g, t->s, t->scratch, 3) == 0, "graph: again");
    CHECK(g != NULL && reused(g, "a") && reused(g, "b") && reused(g, "c") && built(g, "f"),
          "graph: again");
    CHECK(lines_of(t->count, "a") == 1 && lines_of(t->count, "c") == 1 &&
              lines_of(t->count, "f") == 2,
          "graph: again: only the fresh node ran");
    etude_prep_free(g);
}

/** Build again every node on a source whose content changed. */
static void graph_source(const struct graph_row *t)
{
    struct etude_prep *g;

    CHECK(write_text(t->src, "source two\n") == 0, "graph");
    g = graph_basic(t, "recipe 1");
    CHECK(g != NULL && run(g, t->s, t->scratch, 3) == 0, "graph: source");
    CHECK(g != NULL && built(g, "a") && built(g, "b") && built(g, "c"), "graph: source");
    etude_prep_free(g);
}

/**
 * Build again the node whose key text changed, and reuse its dependent,
 * whose inputs have the same content.
 */
static void graph_key(const struct graph_row *t)
{
    struct etude_prep *g;

    CHECK(write_text(t->src, "source one\n") == 0, "graph");
    g = graph_basic(t, "recipe 2");
    CHECK(g != NULL && run(g, t->s, t->scratch, 3) == 0, "graph: key");
    if (g == NULL)
        return;
    CHECK(reused(g, "a") && built(g, "b") && reused(g, "c"), "graph: key");
    CHECK(strcmp(etude_prep_result(g, "b")->key, t->bkey) != 0, "graph: key");
    CHECK(strcmp(etude_prep_result(g, "c")->key, t->ckey) == 0,
          "graph: key: c's inputs have the same content");
    etude_prep_free(g);
}

/**
 * Build a graph, reuse it, and make again what a changed source or key
 * changes.
 */
static void test_graph(void)
{
    struct graph_row t;

    memset(&t, 0, sizeof(t));
    t.dir = fix_path("graph");
    t.src = fix_path("graph-src");
    t.count = fix_path("graph-count");
    t.scratch = fix_path("graph-work");
    t.s = etude_store_open(t.dir);
    CHECK(t.s != NULL && mkdir(t.scratch, 0755) == 0, "graph");
    if (t.s == NULL)
        return;
    CHECK(write_text(t.src, "source one\n") == 0, "graph");
    graph_first(&t);
    graph_again(&t);
    graph_source(&t);
    graph_key(&t);
    etude_store_close(t.s);
}

/** Refuse a name given twice, a missing input, a cycle and an unknown input. */
static void test_refused(void)
{
    static const char *const on_y[] = {"y", NULL};
    static const char *const on_x[] = {"x", NULL};
    static const char *const on_z[] = {"z", NULL};
    const char *count = fix_path("refused-count");
    struct etude_store *s = etude_store_open(fix_path("refused"));
    char *v[11];
    struct etude_prep *g = etude_prep_new();

    CHECK(s != NULL && g != NULL, "refused");
    if (s == NULL || g == NULL) {
        etude_prep_free(g);
        etude_store_close(s);
        return;
    }
    argv_of(v, "cat", count, "x", NULL);
    CHECK(add(g, "x", v, NULL) == 0, "refused");
    CHECK(add(g, "x", v, NULL) == -1 && errno == EEXIST, "refused: twice");
    CHECK(add(g, "a name", v, NULL) == -1 && errno == EINVAL, "refused: name");
    CHECK(add(g, "w", v, on_z) == 0, "refused");
    CHECK(run(g, s, fix_dir(), 1) == -1 && errno == EINVAL, "refused: missing input");
    etude_prep_free(g);

    g = etude_prep_new();
    CHECK(g != NULL && add(g, "x", v, on_y) == 0 && add(g, "y", v, on_x) == 0, "refused");
    CHECK(g != NULL && run(g, s, fix_dir(), 1) == -1 && errno == ELOOP, "refused: cycle");
    etude_prep_free(g);

    g = etude_prep_new();
    argv_of(v, "cat", count, "x", ARGS("${IN:y}"));
    CHECK(g != NULL && add(g, "x", v, NULL) == 0, "refused");
    CHECK(g != NULL && run(g, s, fix_dir(), 1) == -1 && errno == EINVAL, "refused: unknown input");
    etude_prep_free(g);
    CHECK(access(count, F_OK) != 0, "refused: nothing ran");
    etude_store_close(s);
}

/**
 * Say whether node `name` failed with `why` in its reason and kept its
 * work, with a partial product in it when `partial` is set.
 */
static int failed_so(const struct etude_prep *g, const char *name, const char *why, int partial)
{
    const struct etude_prep_result *r = etude_prep_result(g, name);
    char path[PATH_MAX], buf[64];

    if (r == NULL || r->state != ETUDE_PREP_FAILED || strstr(r->why, why) == NULL ||
        r->work == NULL) {
        printf("etude-artifact-test: %s: state %d: %s\n", name, r ? (int)r->state : -1,
               r ? r->why : "no result");
        return 0;
    }
    if (!partial)
        return 1;
    fix_format(path, sizeof(path), "%s/out", r->work);
    return read_text(path, buf, sizeof(buf)) == 7 && strcmp(buf, "partial") == 0;
}

/**
 * Give the graph of test_failures: a recipe that fails each way, a good
 * one, and two nodes that depend on a failed one; NULL when it cannot.
 */
static struct etude_prep *graph_failures(const char *count)
{
    static const char *const on_bad[] = {"bad-exit", "good", NULL};
    static const char *const on_dep[] = {"dep", NULL};
    static const char *const modes[] = {"fail", "kill", "none"};
    static const char *const names[] = {"bad-exit", "bad-kill", "bad-none"};
    struct etude_prep *g = etude_prep_new();
    struct etude_prep_node hang;
    char *v[8][11];
    int i, r = g == NULL ? -1 : 0;

    for (i = 0; i < 3 && r == 0; i++)
        r = add(g, names[i], argv_of(v[i], modes[i], count, names[i], NULL), NULL);
    hang = node_of("bad-hang", argv_of(v[3], "hang", count, "bad-hang", ARGS(hang_ready)), NULL);
    hang.bound_us = SHORT_BOUND;
    if (r != 0 || etude_prep_add(g, &hang) != 0 ||
        add(g, "bad-link", argv_of(v[4], "link", count, "bad-link", ARGS(count)), NULL) != 0 ||
        add(g, "good", argv_of(v[5], "cat", count, "good", NULL), NULL) != 0 ||
        add(g, "dep", argv_of(v[6], "cat", count, "dep", ARGS("${IN:bad-exit}", "${IN:good}")),
            on_bad) != 0 ||
        add(g, "dep2", argv_of(v[7], "cat", count, "dep2", ARGS("${IN:dep}")), on_dep) != 0) {
        etude_prep_free(g);
        return NULL;
    }
    return g;
}

/** Keep startup outside the bound comparison, then cancel the real recipe. */
static int run_failures(struct etude_prep *g, struct etude_store *s, const char *scratch)
{
    int result;

    unlink(hang_ready);
    hang_clock_at = posix_now_us();
    preparation_clock = hang_clock;
    result = run(g, s, scratch, 4);
    preparation_clock = NULL;
    unlink(hang_ready);
    return result;
}

/** Fail each way a recipe can, publish nothing, and block the dependents. */
static void test_failures(void)
{
    const char *dir = fix_path("fail");
    const char *count = fix_path("fail-count");
    const char *scratch = fix_path("fail-work");
    struct etude_store *s = etude_store_open(dir);
    struct etude_prep *g;
    char partial[ETUDE_DIGEST_HEX + 1], path[PATH_MAX];
    const struct etude_prep_result *r;

    fix_format(hang_ready, sizeof(hang_ready), "%s", fix_path("hang-ready"));
    g = graph_failures(count);
    CHECK(s != NULL && g != NULL && mkdir(scratch, 0755) == 0, "failures");
    if (s == NULL || g == NULL) {
        etude_prep_free(g);
        etude_store_close(s);
        return;
    }
    CHECK(run_failures(g, s, scratch) == 1, "failures: the run fails");
    CHECK(failed_so(g, "bad-exit", "exited 3", 1), "failures: bad-exit");
    r = etude_prep_result(g, "bad-exit");
    CHECK(r->err.data != NULL && strstr(r->err.data, "fail: on purpose") != NULL,
          "failures: bad-exit: its standard error");
    CHECK(failed_so(g, "bad-kill", "signal 9", 1), "failures: bad-kill");
    CHECK(failed_so(g, "bad-hang", "past its bound", 1), "failures: bad-hang");
    CHECK(etude_prep_result(g, "bad-hang")->end == ETUDE_END_STOPPED, "failures: bad-hang");
    CHECK(failed_so(g, "bad-none", "made no product", 0), "failures: bad-none");
    CHECK(failed_so(g, "bad-link", "not a regular file", 0), "failures: bad-link");
    CHECK(built(g, "good"), "failures: good");
    CHECK(is(g, "dep", ETUDE_PREP_BLOCKED, 0) && is(g, "dep2", ETUDE_PREP_BLOCKED, 0),
          "failures: blocked");
    CHECK(lines_of(count, "dep") == 0 && lines_of(count, "dep2") == 0,
          "failures: a blocked recipe never runs");
    sha_hex("partial", 7, partial);
    CHECK(etude_store_path(s, partial, path, sizeof(path)) == 0 && access(path, F_OK) != 0,
          "failures: no partial product is an object");
    CHECK(in_store(dir, "objects") == 1 && in_store(dir, "nodes") == 1 && in_store(dir, "tmp") == 0,
          "failures: only the good node is published");
    CHECK(run_failures(g, s, scratch) == 1 && lines_of(count, "bad-exit") == 2 && reused(g, "good"),
          "failures: a failure is not kept");
    etude_prep_free(g);
    etude_store_close(s);
}

/** Damage the object or the record of node a, and make it again. */
static void damage_and_build(struct etude_prep *g, struct etude_store *s, const char *dir,
                             const char *count)
{
    const struct etude_prep_result *r = etude_prep_result(g, "a");
    char path[PATH_MAX], d[ETUDE_DIGEST_HEX + 1];
    uint64_t size;

    memcpy(d, r->digest, sizeof(d));
    CHECK(etude_store_path(s, d, path, sizeof(path)) == 0 && chmod(path, 0644) == 0 &&
              write_text(path, "X") == 0,
          "damage");
    CHECK(etude_store_check(s, d, NULL) == -1 && errno == EBADMSG, "damage: check");
    CHECK(etude_store_copy(s, d, fix_path("damage-copy")) == -1 && errno == EBADMSG &&
              access(fix_path("damage-copy"), F_OK) != 0,
          "damage: copy");
    CHECK(run(g, s, fix_dir(), 2) == 0, "damage: object");
    CHECK(built(g, "a") && reused(g, "c"), "damage: object: made again");
    CHECK(etude_store_check(s, d, NULL) == 0, "damage: object: repaired");

    r = etude_prep_result(g, "a");
    fix_format(path, sizeof(path), "%s/nodes/%s", dir, r->key);
    CHECK(chmod(path, 0644) == 0 && write_text(path, "etude-artifact 1\nkey") == 0, "damage");
    CHECK(run(g, s, fix_dir(), 2) == 0, "damage: record");
    CHECK(built(g, "a") && lines_of(count, "a") == 3, "damage: record");
    CHECK(store_record_read(s, r->key, d, &size) == 0 && size == 1, "damage: record: repaired");
}

/** Make again a node whose object or record was damaged, and copy objects. */
static void test_damage(void)
{
    static const char *const on_a[] = {"a", NULL};
    const char *dir = fix_path("damage");
    const char *count = fix_path("damage-count");
    const char *copy = fix_path("damage-copy");
    struct etude_store *s = etude_store_open(dir);
    char *va[11], *vc[11], buf[64];
    const struct etude_prep_result *r;
    struct etude_prep *g = etude_prep_new();
    struct stat st;

    CHECK(s != NULL && g != NULL, "damage");
    if (s == NULL || g == NULL || add(g, "a", argv_of(va, "cat", count, "a", NULL), NULL) != 0 ||
        add(g, "c", argv_of(vc, "cat", count, "c", ARGS("${IN:a}")), on_a) != 0 ||
        run(g, s, fix_dir(), 2) != 0 || !built(g, "a")) {
        CHECK(0, "damage");
        etude_prep_free(g);
        etude_store_close(s);
        return;
    }
    damage_and_build(g, s, dir, count);
    r = etude_prep_result(g, "a");
    CHECK(etude_store_copy(s, r->digest, copy) == 0 && read_text(copy, buf, sizeof(buf)) == 1 &&
              strcmp(buf, "a") == 0 && stat(copy, &st) == 0 && (st.st_mode & S_IWUSR) != 0,
          "damage: copy: private and writable");
    CHECK(etude_store_copy(s, r->digest, copy) == -1 && errno == EEXIST, "damage: copy: exists");
    etude_prep_free(g);
    etude_store_close(s);
}

/**
 * Count the entries of directory `path` that `check` refuses, and add the
 * number of entries to `*seen`.
 */
static long scan(struct etude_store *s, const char *path,
                 int (*check)(struct etude_store *, const char *), long *seen)
{
    struct dirent *e;
    DIR *dd = opendir(path);
    long bad = 0;

    while (dd != NULL && (e = readdir(dd)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        (*seen)++;
        bad += check(s, e->d_name) != 0;
    }
    if (dd != NULL)
        closedir(dd);
    return bad;
}

/** Check object `name` against its digest. */
static int object_whole(struct etude_store *s, const char *name)
{
    return etude_store_check(s, name, NULL);
}

/** Check that record `name` has an object of the race's size that checks. */
static int record_whole(struct etude_store *s, const char *name)
{
    char d[ETUDE_DIGEST_HEX + 1];
    uint64_t size;

    return store_lookup(s, name, d, &size) != 0 || size != RACE_BYTES;
}

/**
 * Read every object and record of store `dir` until `stop` closes, and
 * write on `out` the scans, the partial artifacts found and the entries.
 */
static void race_reader(const char *dir, int stop, int out)
{
    struct etude_store *s = etude_store_open(dir);
    char objects[PATH_MAX], nodes[PATH_MAX];
    struct pollfd p = {stop, POLLIN, 0};
    long scans = 0, bad = 0, seen = 0;

    if (s == NULL)
        _exit(1);
    fix_format(objects, sizeof(objects), "%s/objects", dir);
    fix_format(nodes, sizeof(nodes), "%s/nodes", dir);
    while (poll(&p, 1, 0) == 0) {
        bad += scan(s, objects, object_whole, &seen);
        bad += scan(s, nodes, record_whole, &seen);
        scans++;
    }
    dprintf(out, "%ld %ld %ld\n", scans, bad, seen);
    etude_store_close(s);
    _exit(0);
}

/* The fixtures of test_race. */
struct race {
    const char *dir, *go, *started;
    char want[ETUDE_DIGEST_HEX + 1];
    int ready[2];  /* each racer's byte as it begins */
    int result[2]; /* each racer's outcome: b built, u reused, x neither */
    int sfd;       /* the read end of `started`, a byte for each recipe begun */
};

/** Prepare the race's node as one of the racers, number `i`; never returns. */
static void race_worker(const struct race *t, int i)
{
    struct etude_store *own = etude_store_open(t->dir);
    struct etude_prep *g = etude_prep_new();
    const struct etude_prep_result *r;
    char *v[11], work[PATH_MAX], c = 'r';
    int rc;

    fix_format(work, sizeof(work), "%s-work%d", t->dir, i);
    if (own == NULL || g == NULL || mkdir(work, 0755) != 0 ||
        add(g, "race", argv_of(v, "gate", "-", "race", ARGS(t->started, t->go)), NULL) != 0 ||
        write(t->ready[1], &c, 1) != 1)
        _exit(1);
    rc = run(g, own, work, 1);
    r = etude_prep_result(g, "race");
    c = 'x';
    if (rc == 0 && strcmp(r->digest, t->want) == 0 && r->state == ETUDE_PREP_BUILT && r->ran)
        c = 'b';
    if (rc == 0 && strcmp(r->digest, t->want) == 0 && r->state == ETUDE_PREP_REUSED && !r->ran)
        c = 'u';
    if (write(t->result[1], &c, 1) != 1)
        _exit(1);
    etude_prep_free(g);
    etude_store_close(own);
    _exit(0);
}

/**
 * Wait until a recipe has begun, give the other racers a second to begin
 * one too, and give the count begun.
 */
static int race_starts(const struct race *t)
{
    struct pollfd p = {t->sfd, POLLIN, 0};
    int starts = 0;
    char c;

    if (poll(&p, 1, -1) == 1 && read(t->sfd, &c, 1) == 1)
        starts++;
    CHECK(starts == 1, "race: the recipe began");
    while (poll(&p, 1, 1000) == 1 && read(t->sfd, &c, 1) == 1)
        starts++;
    return starts;
}

/**
 * Make the fixtures of the race: the digest of its product, the lock on
 * `go` that keeps its recipe waiting, and the FIFO and pipes it reports on;
 * 0, or -1 after a failed check.
 */
static int race_setup(struct race *t, int *gofd, int *sfd_w)
{
    unsigned char *big = malloc(RACE_BYTES);
    struct etude_store *s = etude_store_open(t->dir);

    etude_store_close(s);
    if (big != NULL) {
        pattern(big, RACE_BYTES);
        sha_hex(big, RACE_BYTES, t->want);
        free(big);
    }
    *gofd = open(t->go, O_RDWR | O_CREAT, 0644);
    CHECK(*gofd >= 0 && lock_file(*gofd, F_WRLCK, 0) == 0 && mkfifo(t->started, 0600) == 0, "race");
    t->sfd = open(t->started, O_RDONLY | O_NONBLOCK);
    *sfd_w = open(t->started, O_WRONLY | O_NONBLOCK);
    if (big == NULL || s == NULL || *gofd < 0 || t->sfd < 0 || *sfd_w < 0 || pipe(t->ready) != 0 ||
        pipe(t->result) != 0) {
        CHECK(0, "race");
        return -1;
    }
    return 0;
}

/** Read the reader's report from `fd` and wait for it. */
static void race_report(pid_t reader, int fd)
{
    long scans = 0, bad = -1, seen = 0;
    char report[128];
    ssize_t n = read(fd, report, sizeof(report) - 1);

    if (n > 0) {
        report[n] = '\0';
        sscanf(report, "%ld %ld %ld", &scans, &bad, &seen);
    }
    reap(reader);
    CHECK(scans > 0 && bad == 0, "race: no reader saw a partial artifact");
}

/**
 * Let six processes prepare one node at once: one runs its recipe, which
 * waits until the test lets it go, and the others reuse its product, while
 * a reader reads the store throughout.
 */
static void test_race(void)
{
    struct race t;
    int gofd, sfd_w, stop[2], back[2], i, starts, outcome[3] = {0, 0, 0};
    pid_t kids[RACERS], reader;
    char c;

    memset(&t, 0, sizeof(t));
    t.dir = fix_path("race");
    t.go = fix_path("race-go");
    t.started = fix_path("race-started");
    if (race_setup(&t, &gofd, &sfd_w) != 0 || pipe(stop) != 0 || pipe(back) != 0)
        return;
    reader = fork();
    if (reader == 0) {
        close(stop[1]);
        race_reader(t.dir, stop[0], back[1]);
    }
    close(stop[0]);
    close(back[1]);
    for (i = 0; i < RACERS; i++) {
        kids[i] = fork();
        if (kids[i] == 0)
            race_worker(&t, i);
    }
    close(t.ready[1]);
    close(t.result[1]);
    for (i = 0; i < RACERS && read(t.ready[0], &c, 1) == 1; i++) {
    }
    CHECK(i == RACERS, "race: every racer began");
    starts = race_starts(&t);
    close(gofd);
    for (i = 0; i < RACERS && read(t.result[0], &c, 1) == 1; i++)
        outcome[c == 'b' ? 0 : c == 'u' ? 1 : 2]++;
    for (i = 0; i < RACERS; i++)
        reap(kids[i]);
    while (read(t.sfd, &c, 1) == 1)
        starts++;
    close(stop[1]);
    race_report(reader, back[0]);
    CHECK(starts == 1, "race: the recipe ran once");
    CHECK(outcome[0] == 1 && outcome[1] == RACERS - 1 && outcome[2] == 0,
          "race: one built, the others reused");
    CHECK(in_store(t.dir, "tmp") == 0 && in_store(t.dir, "objects") == 1, "race: one object");
    close(t.sfd);
    close(sfd_w);
    close(t.ready[0]);
    close(t.result[0]);
    close(back[0]);
}

/**
 * Kill a controller while its recipe runs: nothing is published, the key's
 * lock goes with the controller, and the next preparation makes the node.
 */
static void test_killed(void)
{
    const char *dir = fix_path("killed");
    const char *started = fix_path("killed-started");
    const char *go = fix_path("killed-go");
    const char *scratch = fix_path("killed-work");
    char want[ETUDE_DIGEST_HEX + 1], *v[11], c;
    struct etude_store *s;
    struct etude_prep *g;
    struct pollfd p;
    int gofd, sfd, sfd_w;
    pid_t kid;

    sha_hex("whole product\n", 14, want);
    gofd = open(go, O_RDWR | O_CREAT, 0644);
    CHECK(mkdir(scratch, 0755) == 0 && gofd >= 0 && lock_file(gofd, F_WRLCK, 0) == 0 &&
              mkfifo(started, 0600) == 0,
          "killed");
    sfd = open(started, O_RDONLY | O_NONBLOCK);
    sfd_w = open(started, O_WRONLY | O_NONBLOCK);
    argv_of(v, "once", "-", "once", ARGS(fix_path("killed-flag"), started, go));
    kid = fork();
    if (kid == 0) {
        s = etude_store_open(dir);
        g = etude_prep_new();
        if (s == NULL || g == NULL || add(g, "once", v, NULL) != 0)
            _exit(1);
        run(g, s, scratch, 1);
        _exit(0);
    }
    p.fd = sfd;
    p.events = POLLIN;
    CHECK(poll(&p, 1, -1) == 1 && read(sfd, &c, 1) == 1, "killed: the recipe began");
    kill(kid, SIGKILL);
    reap(kid);
    CHECK(in_store(dir, "nodes") == 0 && in_store(dir, "objects") == 0 && in_store(dir, "tmp") == 0,
          "killed: nothing published");
    s = etude_store_open(dir);
    g = etude_prep_new();
    CHECK(s != NULL && g != NULL && add(g, "once", v, NULL) == 0 && run(g, s, scratch, 1) == 0,
          "killed: again");
    CHECK(g != NULL && built(g, "once") && strcmp(etude_prep_result(g, "once")->digest, want) == 0,
          "killed: again: the whole product");
    etude_prep_free(g);
    etude_store_close(s);
    close(gofd);
    close(sfd);
    close(sfd_w);
}

int main(int argc, char **argv)
{
    if (argc >= 6 && strcmp(argv[1], "--recipe") == 0)
        return recipe(argc, argv);
    if (realpath(argv[0], self) == NULL) {
        printf("etude-artifact-test: fail: %s cannot be found\n", argv[0]);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    test_put();
    test_digests();
    test_digests_settle();
    test_digests_put();
    test_digests_objects();
    test_digests_object_settle();
    test_digests_old_format();
    test_digests_portable();
    test_digests_table();
    test_digests_write();
    test_digests_two_writers();
    test_sparse();
    test_range();
    test_overlay();
    test_interrupt();
    test_graph();
    test_refused();
    test_failures();
    test_damage();
    test_race();
    test_killed();
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_artifact_hosttest: pass\n");
    return 0;
}
