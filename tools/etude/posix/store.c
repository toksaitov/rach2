/*
 * store.c: etude's store of immutable artifacts on POSIX systems. An object
 * is written under a partial name, synchronized, and then linked to the name
 * of its digest; a record of a node's key names the object it published. A
 * digests file keeps the digests of files that have not changed since they
 * were read, so that a file is not read again to be hashed.
 */
#define _FILE_OFFSET_BITS 64
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#include "../win/files.h"
#endif

#include "etude/artifact.h"
#include "sha256.h"
#include "store.h"

/* The prefix of a partial file's name in tmp/. */
#define PART_PREFIX "part."

/* The tries at a partial file that a sweep removed before it was locked. */
#define PART_TRIES 8

/* The largest record read. */
#define RECORD_MAX 4096

/* The first line of a record of this format. */
#define RECORD_MAGIC "etude-artifact 1"

/* The buffer of a copy; a chunk read that is all zeros is left a hole. */
#define COPY_CHUNK (1u << 20)

/* A file whose change time is less than this before a read of it began may
 * be written again within the same tick of the file system's clock, and so
 * keep the status it had when it was read; its digest is not kept. */
#define SETTLE_NS 2000000000LL

/* The most lines a digests file keeps, and the longest line read from one. */
#define DIGESTS_MAX  16384
#define DIGESTS_LINE 8192

#ifdef RACH2_HOSTTEST
/* In a host test, the settling time, which a test may shorten, and the count
 * of the reads that hashed a file and of the copies made. */
long long etude_store_settle_ns = SETTLE_NS;
unsigned long etude_store_hashed, etude_store_copied;
#define SETTLE         etude_store_settle_ns
#define COUNT(counter) ((counter)++)
#else
#define SETTLE         SETTLE_NS
#define COUNT(counter) ((void)0)
#endif

/* An overlay's qcow2 header: its magic, "QFI\xfb"; the length of version 3's
 * header; the type of the extension that gives the backing file's format;
 * where the backing file's name begins, past that extension and the end of
 * the extensions; 16-bit refcounts; and the largest L1 table QEMU opens. A
 * base's header is read as far as the end of its size, QCOW_PROBE bytes. */
#define QCOW_MAGIC          0x514649fbu
#define QCOW_PROBE          32
#define QCOW_HEADER         104
#define QCOW_EXT_FORMAT     0xe2792acau
#define QCOW_NAME           (QCOW_HEADER + 16 + 8)
#define QCOW_REFCOUNT_ORDER 4
#define QCOW_L1_MAX         (UINT64_C(32) << 20)

struct etude_store {
    char *dir;
    char *objects; /* DIGEST: the objects, read-only */
    char *nodes;   /* KEY: the records of the nodes' products */
    char *locks;   /* KEY: the locks of preparations under way */
    char *tmp;     /* part.XXXXXX: partial files, each locked by its writer */
    char *digests; /* the digests file, or NULL for none */
};

/* What tells that a file has not changed since its digest was taken. */
struct status {
    unsigned long long dev, ino, size;
    long long mtime, ctime; /* nanoseconds since the epoch */
};

/* A whole read of a file: its status when the read began and when it ended,
 * and the time it began, in nanoseconds since the epoch. */
struct reading {
    struct status before, after;
    long long began;
};

/* ---- Names and files. ---- */

char *store_join(const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b);
    char *s = malloc(na + nb + 2);

    if (s == NULL)
        return NULL;
    memcpy(s, a, na);
    s[na] = '/';
    memcpy(s + na + 1, b, nb + 1);
    return s;
}

int store_is_digest(const char *s)
{
    size_t n;

    for (n = 0; s[n] != '\0'; n++)
        if (n == ETUDE_DIGEST_HEX ||
            !((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f')))
            return 0;
    return n == ETUDE_DIGEST_HEX;
}

/** Make directory `path` unless it is there; 0 or -1 with errno set. */
static int dir_make(const char *path)
{
    struct stat st;

    if (mkdir(path, 0755) == 0)
        return 0;
    if (errno != EEXIST)
        return -1;
    if (stat(path, &st) != 0)
        return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
}

/** Synchronize directory `path`, so that a name made in it is durable. */
static int dir_sync(const char *path)
{
#ifdef _WIN32
    struct stat st;
    /* Publication uses MoveFileExW with MOVEFILE_WRITE_THROUGH. */
    if (stat(path, &st) != 0)
        return -1;
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    return 0;
#else
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    int e;

    if (fd < 0)
        return -1;
    if (fsync(fd) != 0 && errno != EINVAL) {
        e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    close(fd);
    return 0;
#endif
}

/** Write all of `buf`; 0 or -1 with errno set. */
static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    ssize_t w;

    while (n > 0) {
        w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w < 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/** Whether the `n` bytes at `buf` are all zero. */
static int all_zero(const unsigned char *buf, size_t n)
{
    return n == 0 || (buf[0] == 0 && memcmp(buf, buf + 1, n - 1) == 0);
}

/**
 * Read `from` to its end or `limit`, hashing it unless `hex` is NULL, and
 * write it to `to` unless `to` is negative; 0 with the digest and size, or
 * -1 with errno set.
 *
 * `to` is a new regular file. A chunk of zeros is passed over with
 * lseek(2), which leaves a hole in `to`, so that a sparse file stays
 * sparse; `to` is then given the size read, for a file that ends in zeros.
 */
static int copy_stream(int from, int to, char hex[ETUDE_DIGEST_HEX + 1], uint64_t *size,
                       uint64_t limit)
{
    unsigned char *buf = malloc(COPY_CHUNK);
    unsigned char sum[32];
    struct sha256 h;
    uint64_t total = 0;
    ssize_t r;

    if (buf == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (hex != NULL) {
        sha256_init(&h);
        COUNT(etude_store_hashed);
    }
    if (to >= 0)
        COUNT(etude_store_copied);
    for (;;) {
        size_t want = COPY_CHUNK;
        if (total == limit)
            break;
        if (limit - total < want)
            want = (size_t)(limit - total);
        r = read(from, buf, want);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            goto fail;
        if (r == 0) {
            if (limit != UINT64_MAX) {
                errno = EIO;
                goto fail;
            }
            break;
        }
        if (hex != NULL)
            sha256_update(&h, buf, (size_t)r);
        total += (uint64_t)r;
        if (to < 0)
            continue;
        if (all_zero(buf, (size_t)r)) {
            if (lseek(to, (off_t)r, SEEK_CUR) < 0)
                goto fail;
        } else if (write_all(to, buf, (size_t)r) != 0) {
            goto fail;
        }
    }
    if (to >= 0 && ftruncate(to, (off_t)total) != 0)
        goto fail;
    free(buf);
    if (hex != NULL) {
        sha256_final(&h, sum);
        sha256_hex(sum, hex);
    }
    if (size != NULL)
        *size = total;
    return 0;
fail:
    free(buf);
    return -1;
}

static int copy_hash(int from, int to, char hex[ETUDE_DIGEST_HEX + 1], uint64_t *size)
{
    return copy_stream(from, to, hex, size, UINT64_MAX);
}

/** Lock all of `fd` for `type` without waiting; 0, or -1 with errno set. */
static int lock_try(int fd, short type)
{
#ifdef _WIN32
    return etude_win_file_lock(fd, type, 0);
#else
    struct flock fl;

    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &fl) == 0)
        return 0;
    if (errno == EACCES)
        errno = EAGAIN;
    return -1;
#endif
}

/** Wait until the digests file's lock is held. */
static int store_wait_lock(int fd)
{
#ifdef _WIN32
    return etude_win_file_lock(fd, F_WRLCK, 1);
#else
    struct flock fl;
    memset(&fl, 0, sizeof fl);
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    return fcntl(fd, F_SETLKW, &fl);
#endif
}

/**
 * Make a partial file in tmp/, locked by this process for as long as `*fd`
 * is open, and give its path from malloc in `*path`.
 *
 * A sweep that ran between the file's making and its lock may have removed
 * it, so the lock is followed by a check that the name is still this file.
 */
static int part_open(const struct etude_store *s, char **path, int *fd)
{
    struct stat a, b;
    int tries, e;

    for (tries = 0; tries < PART_TRIES; tries++) {
        *path = store_join(s->tmp, PART_PREFIX "XXXXXX");
        if (*path == NULL) {
            errno = ENOMEM;
            return -1;
        }
        *fd = mkstemp(*path);
        if (*fd < 0)
            goto fail;
        if (fcntl(*fd, F_SETFD, FD_CLOEXEC) != 0)
            goto fail;
        if (lock_try(*fd, F_WRLCK) == 0 && fstat(*fd, &a) == 0 && stat(*path, &b) == 0 &&
            a.st_dev == b.st_dev && a.st_ino == b.st_ino)
            return 0;
        close(*fd);
        free(*path);
    }
    errno = EAGAIN;
    return -1;
fail:
    e = errno;
    if (*fd >= 0)
        close(*fd);
    free(*path);
    errno = e;
    return -1;
}

/** Remove a partial file and close it, keeping errno. */
static void part_drop(char *path, int fd)
{
    int e = errno;

    unlink(path);
    close(fd);
    free(path);
    errno = e;
}

/**
 * Give partial file `part`, whole and synchronized, the name `name` in
 * directory `dir`.
 *
 * With `valid`, a file already there whose content `valid` accepts is kept,
 * so that its inode does not change. Without `valid`, when it refuses the
 * file, or when the file system refuses a hard link, rename(2) replaces the
 * file there. Either way the name shows a whole file.
 */
static int publish(const char *dir, const char *name, const char *part,
                   int (*valid)(const struct etude_store *, const char *),
                   const struct etude_store *s)
{
    char *final = store_join(dir, name);
    int r = -1;

    if (final == NULL) {
        errno = ENOMEM;
        return -1;
    }
    if (valid != NULL && link(part, final) == 0)
        r = 0;
    else if (valid != NULL && errno == EEXIST && valid(s, name) == 0)
        r = 0;
    else if (rename(part, final) == 0)
        r = 0;
    free(final);
    if (r == 0 && dir_sync(dir) != 0)
        r = -1;
    return r;
}

/* ---- The digests of files. ---- */

/* A digests file has a line for each file whose digest it keeps:
 * `DIGEST SIZE DEVICE INODE MTIME CTIME PATH`, its times in nanoseconds. */

/** Take the status of `st` into `k`. */
static void status_of(const struct stat *st, struct status *k)
{
#ifdef __APPLE__
    const struct timespec *m = &st->st_mtimespec, *c = &st->st_ctimespec;
#else
    const struct timespec *m = &st->st_mtim, *c = &st->st_ctim;
#endif

    k->dev = (unsigned long long)st->st_dev;
    k->ino = (unsigned long long)st->st_ino;
    k->size = (unsigned long long)st->st_size;
    k->mtime = (long long)m->tv_sec * 1000000000LL + m->tv_nsec;
    k->ctime = (long long)c->tv_sec * 1000000000LL + c->tv_nsec;
}

/** Say whether `a` and `b` are the status of one file that has not changed:
 * the same device, inode and size, and the same times of its last
 * modification and its last change. */
static int status_same(const struct status *a, const struct status *b)
{
    return a->dev == b->dev && a->ino == b->ino && a->size == b->size && a->mtime == b->mtime &&
           a->ctime == b->ctime;
}

int store_copy_range(const char *source, uint64_t offset, uint64_t length, const char *dest)
{
    struct stat st;
    struct status before, after;
    int in = -1, out = -1, ok = 0, saved;

    in = open(source, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (in < 0 || fstat(in, &st))
        goto done;
    if (!S_ISREG(st.st_mode) || st.st_size < 0 || offset > (uint64_t)st.st_size ||
        length > (uint64_t)st.st_size - offset) {
        errno = EINVAL;
        goto done;
    }
    status_of(&st, &before);
    out = open(dest, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (out < 0)
        goto done;
    if (lseek(in, (off_t)offset, SEEK_SET) < 0 || copy_stream(in, out, NULL, NULL, length) ||
        fsync(out) || fstat(in, &st))
        goto done;
    status_of(&st, &after);
    if (!status_same(&before, &after)) {
        errno = EBADMSG;
        goto done;
    }
    ok = 1;
done:
    saved = errno;
    if (in >= 0 && close(in)) {
        saved = errno;
        ok = 0;
    }
    if (out >= 0 && close(out)) {
        saved = errno;
        ok = 0;
    }
    if (!ok && out >= 0)
        unlink(dest);
    errno = saved;
    return ok ? 0 : -1;
}

/** Give the time of the realtime clock, which stamps the files, in
 * nanoseconds since the epoch. */
static long long now_ns(void)
{
    struct timespec t;

    if (clock_gettime(CLOCK_REALTIME, &t) != 0)
        return 0;
    return (long long)t.tv_sec * 1000000000LL + t.tv_nsec;
}

/** Read the next line of `f` into `buf`; 1, or 0 at the end. A line too long
 * for `buf` is given without its newline, and read to its end. */
static int next_line(FILE *f, char *buf, size_t size)
{
    size_t n;
    int c;

    if (fgets(buf, (int)size, f) == NULL)
        return 0;
    n = strlen(buf);
    if (n == size - 1 && buf[n - 1] != '\n')
        while ((c = getc(f)) != EOF && c != '\n') {
        }
    return 1;
}

/** Say whether a digests entry names an absolute path on this platform. */
static int digests_absolute(const char *path)
{
#ifdef _WIN32
    return etude_win_absolute(path);
#else
    return path[0] == '/';
#endif
}

/** Take a line of a digests file, with its newline, into `k`, `hex` and
 * `*path`; 0, or -1 for a malformed line. */
static int digests_line(char *line, struct status *k, char hex[ETUDE_DIGEST_HEX + 1], char **path)
{
    size_t n = strlen(line);
    int at = -1;

    if (n == 0 || line[n - 1] != '\n')
        return -1;
    line[n - 1] = '\0';
    if (sscanf(line, "%64s %llu %llu %llu %lld %lld %n", hex, &k->size, &k->dev, &k->ino, &k->mtime,
               &k->ctime, &at) != 6 ||
        at < 0 || !digests_absolute(line + at) || !store_is_digest(hex))
        return -1;
    *path = line + at;
    return 0;
}

/** Write the line of the file at `path`, of status `k` and digest `hex`, to
 * `f`; 0, or -1. */
static int digests_put(FILE *f, const struct status *k, const char *hex, const char *path)
{
    return fprintf(f, "%s %llu %llu %llu %lld %lld %s\n", hex, k->size, k->dev, k->ino, k->mtime,
                   k->ctime, path) < 0
               ? -1
               : 0;
}

/** Find in digests file `cache` the digest of a file of status `k`; 0 with
 * it in `hex`, or -1 when the file keeps none. */
static int digests_find(const char *cache, const struct status *k, char hex[ETUDE_DIGEST_HEX + 1])
{
    char line[DIGESTS_LINE], found[ETUDE_DIGEST_HEX + 1], *path;
    struct status e;
    FILE *f = fopen(cache, "r");
    int r = -1;

    if (f == NULL)
        return -1;
    while (r != 0 && next_line(f, line, sizeof(line)))
        if (digests_line(line, &e, found, &path) == 0 && status_same(&e, k)) {
            memcpy(hex, found, sizeof(found));
            r = 0;
        }
    fclose(f);
    return r;
}

/** Give a copy of `a` followed by `b` from malloc, or NULL. */
static char *suffixed(const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b);
    char *s = malloc(na + nb + 1);

    if (s != NULL) {
        memcpy(s, a, na);
        memcpy(s + na, b, nb + 1);
    }
    return s;
}

/** Make the directory of file `path` unless it is there. */
static void parent_make(const char *path)
{
    char *dir = strdup(path), *slash = dir != NULL ? strrchr(dir, '/') : NULL;

    if (slash != NULL && slash != dir) {
        *slash = '\0';
        (void)mkdir(dir, 0755);
    }
    free(dir);
}

/**
 * Keep `hex` in digests file `cache` as the digest of the file at `path`,
 * an absolute path, whose status is `k`.
 *
 * The file is written whole as `CACHE.new` and renamed to its name, under
 * the lock of `CACHE.lock`, so that a reader finds it whole and two writers
 * do not lose each other's lines. The line of the same file is replaced,
 * and a line whose file is gone or has changed is dropped, so that the file
 * keeps no more lines than there are files unchanged. A digest that cannot
 * be kept is only read again the next time, so nothing here fails, and
 * errno is kept.
 */
static void digests_keep(const char *cache, const char *path, const struct status *k,
                         const char *hex)
{
    char line[DIGESTS_LINE], old[ETUDE_DIGEST_HEX + 1], *other;
    char *lock = suffixed(cache, ".lock"), *next = suffixed(cache, ".new");
    struct status e, now;
    struct stat st;
    size_t kept = 1;
    FILE *in, *out = NULL;
    int fd = -1, ok, saved = errno;

    if (lock == NULL || next == NULL || !digests_absolute(path) || strchr(path, '\n') != NULL ||
        strlen(path) + 6 * 21 + ETUDE_DIGEST_HEX >= DIGESTS_LINE)
        goto done;
    parent_make(cache);
    if ((fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0644)) < 0 || store_wait_lock(fd) != 0 ||
        (out = fopen(next, "w")) == NULL)
        goto done;
    ok = digests_put(out, k, hex, path) == 0;
    if ((in = fopen(cache, "r")) != NULL) {
        while (ok && kept < DIGESTS_MAX && next_line(in, line, sizeof(line))) {
            if (digests_line(line, &e, old, &other) != 0 || (e.dev == k->dev && e.ino == k->ino) ||
                stat(other, &st) != 0)
                continue;
            status_of(&st, &now);
            if (!status_same(&e, &now))
                continue;
            ok = digests_put(out, &e, old, other) == 0;
            kept++;
        }
        fclose(in);
    }
    ok = fclose(out) == 0 && ok;
    if (!ok || rename(next, cache) != 0)
        unlink(next);
done:
    if (fd >= 0)
        close(fd);
    free(lock);
    free(next);
    errno = saved;
}

/**
 * Keep the digest `hex` of the file at `path` in digests file `cache`, when
 * there is one, after the whole read `r`. A file that changed while it was
 * read is not kept, nor one that changed so shortly before the read began
 * that a write after the read could leave its status as it was.
 */
static void digests_read(const char *cache, const char *path, const struct reading *r,
                         const char *hex)
{
    if (cache != NULL && status_same(&r->before, &r->after) && r->before.ctime + SETTLE <= r->began)
        digests_keep(cache, path, &r->before, hex);
}

int etude_digest_file(const char *cache, const char *path, struct stat *st,
                      char digest[ETUDE_DIGEST_HEX + 1])
{
    struct reading r;
    struct stat now;
    int fd, e;

    r.began = now_ns();
    fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return -1;
    if (fstat(fd, st) != 0)
        goto fail;
    if (!S_ISREG(st->st_mode)) {
        errno = S_ISDIR(st->st_mode) ? EISDIR : EINVAL;
        goto fail;
    }
    status_of(st, &r.before);
    if (cache != NULL && digests_find(cache, &r.before, digest) == 0) {
        close(fd);
        return 0;
    }
    if (copy_hash(fd, -1, digest, NULL) != 0 || fstat(fd, &now) != 0)
        goto fail;
    close(fd);
    status_of(&now, &r.after);
    digests_read(cache, path, &r, digest);
    return 0;
fail:
    e = errno;
    close(fd);
    errno = e;
    return -1;
}

/** Say whether the store's digests file keeps `digest` as the digest of a
 * file of status `st`. */
static int known(const struct etude_store *s, const struct stat *st, const char *digest)
{
    char hex[ETUDE_DIGEST_HEX + 1];
    struct status k;

    status_of(st, &k);
    return s->digests != NULL && digests_find(s->digests, &k, hex) == 0 && strcmp(hex, digest) == 0;
}

/** Say whether object `digest` is published and its digest kept, so that it
 * need not be read to be trusted. */
static int object_known(const struct etude_store *s, const char *digest)
{
    char *path = store_join(s->objects, digest);
    struct stat st;
    int r = path != NULL && stat(path, &st) == 0 && S_ISREG(st.st_mode) && known(s, &st, digest);

    free(path);
    return r;
}

/**
 * Keep the digest of object `digest` when the object is `mine`, the file
 * this process copied and published under that name.
 *
 * Its content is what the copy wrote, and it had mode 0444 before it had a
 * name, so a later write needs its mode changed, which changes its change
 * time. The digest is therefore kept although the object has only just
 * changed.
 */
static void object_written(const struct etude_store *s, const char *digest, const struct stat *mine)
{
    char *path = store_join(s->objects, digest);
    struct status k;
    struct stat st;

    if (s->digests != NULL && path != NULL && stat(path, &st) == 0 && st.st_dev == mine->st_dev &&
        st.st_ino == mine->st_ino) {
        status_of(&st, &k);
        digests_keep(s->digests, path, &k, digest);
    }
    free(path);
}

/* ---- The store. ---- */

struct etude_store *etude_store_open(const char *dir)
{
    struct etude_store *s = calloc(1, sizeof(*s));
    int e;

    if (s == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    s->dir = strdup(dir);
    if (s->dir == NULL || (s->objects = store_join(dir, "objects")) == NULL ||
        (s->nodes = store_join(dir, "nodes")) == NULL ||
        (s->locks = store_join(dir, "locks")) == NULL ||
        (s->tmp = store_join(dir, "tmp")) == NULL) {
        etude_store_close(s);
        errno = ENOMEM;
        return NULL;
    }
    if (dir_make(dir) != 0 || dir_make(s->objects) != 0 || dir_make(s->nodes) != 0 ||
        dir_make(s->locks) != 0 || dir_make(s->tmp) != 0 || etude_store_sweep(s, NULL) != 0) {
        e = errno;
        etude_store_close(s);
        errno = e;
        return NULL;
    }
    return s;
}

void etude_store_close(struct etude_store *s)
{
    if (s == NULL)
        return;
    free(s->dir);
    free(s->objects);
    free(s->nodes);
    free(s->locks);
    free(s->tmp);
    free(s->digests);
    free(s);
}

int etude_store_digests(struct etude_store *s, const char *cache)
{
    char *copy = strdup(cache);

    if (copy == NULL) {
        errno = ENOMEM;
        return -1;
    }
    free(s->digests);
    s->digests = copy;
    return 0;
}

int etude_store_path(const struct etude_store *s, const char *digest, char *buf, size_t size)
{
    int n;

    if (!store_is_digest(digest)) {
        errno = EINVAL;
        return -1;
    }
    n = snprintf(buf, size, "%s/%s", s->objects, digest);
    if (n < 0 || (size_t)n >= size) {
        errno = ERANGE;
        return -1;
    }
    return 0;
}

int etude_store_check(const struct etude_store *s, const char *digest, uint64_t *size)
{
    char hex[ETUDE_DIGEST_HEX + 1];
    struct reading r;
    struct stat st, now;
    char *path;
    int fd, e;

    if (!store_is_digest(digest)) {
        errno = EINVAL;
        return -1;
    }
    r.began = now_ns();
    path = store_join(s->objects, digest);
    if (path == NULL) {
        errno = ENOMEM;
        return -1;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        goto fail;
    if (fstat(fd, &st) != 0)
        goto fail;
    if (!S_ISREG(st.st_mode)) {
        errno = EBADMSG;
        goto fail;
    }
    if (known(s, &st, digest)) {
        if (size != NULL)
            *size = (uint64_t)st.st_size;
    } else {
        if (copy_hash(fd, -1, hex, size) != 0 || fstat(fd, &now) != 0)
            goto fail;
        if (strcmp(hex, digest) != 0) {
            errno = EBADMSG;
            goto fail;
        }
        status_of(&st, &r.before);
        status_of(&now, &r.after);
        digests_read(s->digests, path, &r, hex);
    }
    close(fd);
    free(path);
    return 0;
fail:
    e = errno;
    if (fd >= 0)
        close(fd);
    free(path);
    errno = e;
    return -1;
}

/** Check object `digest` as publish's test of a file already there. */
static int object_valid(const struct etude_store *s, const char *digest)
{
    return etude_store_check(s, digest, NULL);
}

/**
 * Copy `from`, read as `*r` says from its start, to the partial file `fd`,
 * hashing it into `digest` unless `trust` says that `digest` is the digest
 * the digests file keeps for the status the read began with, and give in
 * `*r` the status at the end. A copy not hashed during which the file
 * changed is made again and hashed, and `*r` is then that read's.
 *
 * @return 1 for a copy not hashed, 0 for one hashed, or -1 with errno set.
 */
static int put_copy(int from, int fd, int trust, struct reading *r,
                    char digest[ETUDE_DIGEST_HEX + 1], uint64_t *size)
{
    struct stat st;

    for (;;) {
        if (copy_hash(from, fd, trust ? NULL : digest, size) != 0 || fstat(from, &st) != 0)
            return -1;
        status_of(&st, &r->after);
        if (!trust || status_same(&r->before, &r->after))
            return trust;
        trust = 0;
        r->before = r->after;
        r->began = now_ns();
        if (lseek(from, 0, SEEK_SET) < 0 || ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0)
            return -1;
    }
}

int etude_store_put(struct etude_store *s, const char *path, const char *expect,
                    char digest[ETUDE_DIGEST_HEX + 1], uint64_t *size)
{
    struct reading r;
    struct stat st, mine;
    char *part = NULL;
    int from, fd = -1, trust, e;

    if (expect != NULL && !store_is_digest(expect)) {
        errno = EINVAL;
        return -1;
    }
    from = open(path, O_RDONLY | O_CLOEXEC);
    if (from < 0)
        return -1;
    r.began = now_ns();
    if (fstat(from, &st) != 0)
        goto fail;
    status_of(&st, &r.before);
    trust = s->digests != NULL && digests_find(s->digests, &r.before, digest) == 0;
    if (trust && expect != NULL && strcmp(digest, expect) != 0) {
        errno = EBADMSG;
        goto fail;
    }
    if (trust && object_known(s, digest)) {
        close(from);
        if (size != NULL)
            *size = (uint64_t)st.st_size;
        return 0;
    }
    if (part_open(s, &part, &fd) != 0) {
        part = NULL;
        goto fail;
    }
    if ((trust = put_copy(from, fd, trust, &r, digest, size)) < 0 || fsync(fd) != 0)
        goto fail;
    close(from);
    from = -1;
    if (!trust)
        digests_read(s->digests, path, &r, digest);
    if (expect != NULL && strcmp(expect, digest) != 0) {
        errno = EBADMSG;
        goto fail;
    }
    if (fchmod(fd, 0444) != 0 || fstat(fd, &mine) != 0 ||
        publish(s->objects, digest, part, object_valid, s) != 0)
        goto fail;
    part_drop(part, fd);
    object_written(s, digest, &mine);
    return 0;
fail:
    e = errno;
    if (from >= 0)
        close(from);
    if (part != NULL)
        part_drop(part, fd);
    errno = e;
    return -1;
}

int etude_store_copy(const struct etude_store *s, const char *digest, const char *dest)
{
    char hex[ETUDE_DIGEST_HEX + 1] = "";
    struct reading r;
    struct stat st, now;
    char *path;
    int from, to, trust, e;

    if (!store_is_digest(digest)) {
        errno = EINVAL;
        return -1;
    }
    r.began = now_ns();
    path = store_join(s->objects, digest);
    if (path == NULL) {
        errno = ENOMEM;
        return -1;
    }
    from = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (from < 0 || fstat(from, &st) != 0)
        goto fail;
    status_of(&st, &r.before);
    trust = known(s, &st, digest);
    for (;;) {
        to = open(dest, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (to < 0)
            goto fail;
        if (copy_hash(from, to, trust ? NULL : hex, NULL) != 0) {
            e = errno;
            close(to);
            errno = e;
            goto drop;
        }
        if (close(to) != 0 || fstat(from, &now) != 0)
            goto drop;
        status_of(&now, &r.after);
        if (!trust || status_same(&r.before, &r.after))
            break;
        trust = 0;
        r.before = r.after;
        r.began = now_ns();
        if (unlink(dest) != 0 || lseek(from, 0, SEEK_SET) < 0)
            goto fail;
    }
    if (!trust && strcmp(hex, digest) != 0) {
        errno = EBADMSG;
        goto drop;
    }
    if (!trust)
        digests_read(s->digests, path, &r, hex);
    close(from);
    free(path);
    return 0;
drop:
    e = errno;
    unlink(dest);
    errno = e;
fail:
    e = errno;
    if (from >= 0)
        close(from);
    free(path);
    errno = e;
    return -1;
}

/** Put the low `n` bytes of `v` at `p`, most significant first. */
static void put_be(unsigned char *p, uint64_t v, int n)
{
    while (n-- > 0) {
        p[n] = (unsigned char)v;
        v >>= 8;
    }
}

/** Give the `n` bytes at `p` as a number, most significant first. */
static uint64_t get_be(const unsigned char *p, int n)
{
    uint64_t v = 0;

    while (n-- > 0)
        v = v << 8 | *p++;
    return v;
}

/**
 * Give the format of the base `path`, of status `st`, and the size of the
 * disk it holds. A file that begins with qcow2's magic is a qcow2 image of
 * version 2 or 3 with no backing file of its own, whose header gives the
 * size; any other file is a raw disk of its own length.
 *
 * @return 0, or -1 with errno set: EINVAL for a qcow2 image of another
 *         version or with a backing file, or an error of the system.
 */
static int base_format(const char *path, const struct stat *st, const char **format, uint64_t *size)
{
    unsigned char head[QCOW_PROBE];
    ssize_t got;
    int fd, e;

    *format = "raw";
    *size = (uint64_t)st->st_size;
    if (st->st_size < QCOW_PROBE)
        return 0;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    got = read(fd, head, sizeof(head));
    e = errno;
    close(fd);
    if (got != (ssize_t)sizeof(head)) {
        errno = got < 0 ? e : EIO;
        return -1;
    }
    if (get_be(head, 4) != QCOW_MAGIC)
        return 0;
    if ((get_be(head + 4, 4) != 2 && get_be(head + 4, 4) != 3) || get_be(head + 8, 8) != 0) {
        errno = EINVAL;
        return -1;
    }
    *format = "qcow2";
    *size = get_be(head + 24, 8);
    return 0;
}

/**
 * Write the clusters of an empty qcow2 version 3 overlay of `size` bytes in
 * clusters of 2^`bits` bytes over `base`, an absolute path, of `format`,
 * `raw` or `qcow2`, to `fd`. The header, the backing file's format and name
 * share cluster 0; the refcount table, its blocks and the L1 table follow,
 * each cluster counted once, and the L1 table is left zero, so no cluster of
 * the overlay is allocated and every read falls through to the base.
 */
static int write_overlay(int fd, const char *base, const char *format, uint64_t size, unsigned bits)
{
    uint64_t cluster = (uint64_t)1 << bits, l2_span = (cluster / 8) << bits;
    uint64_t l1 = (size + l2_span - 1) / l2_span, l1_clusters = (l1 * 8 + cluster - 1) / cluster;
    uint64_t per_block = cluster / 2, blocks = 0, table = 0, total, i;
    size_t name = strlen(base);
    unsigned char *buf;
    int e;

    if (l1 * 8 > QCOW_L1_MAX) {
        errno = EFBIG;
        return -1;
    }
    if (QCOW_NAME + name > cluster) {
        errno = ENAMETOOLONG;
        return -1;
    }
    do {
        total = blocks + table;
        blocks = (1 + table + blocks + l1_clusters + per_block - 1) / per_block;
        table = (blocks * 8 + cluster - 1) / cluster;
    } while (blocks + table != total);
    total = 1 + table + blocks + l1_clusters;
    buf = calloc((size_t)(1 + table + blocks), (size_t)cluster);
    if (buf == NULL)
        return -1;
    put_be(buf, QCOW_MAGIC, 4);
    put_be(buf + 4, 3, 4);
    put_be(buf + 8, QCOW_NAME, 8);
    put_be(buf + 16, name, 4);
    put_be(buf + 20, bits, 4);
    put_be(buf + 24, size, 8);
    put_be(buf + 36, l1, 4);
    put_be(buf + 40, (1 + table + blocks) * cluster, 8);
    put_be(buf + 48, cluster, 8);
    put_be(buf + 56, table, 4);
    put_be(buf + 96, QCOW_REFCOUNT_ORDER, 4);
    put_be(buf + 100, QCOW_HEADER, 4);
    put_be(buf + QCOW_HEADER, QCOW_EXT_FORMAT, 4);
    put_be(buf + QCOW_HEADER + 4, strlen(format), 4);
    memcpy(buf + QCOW_HEADER + 8, format, strlen(format));
    memcpy(buf + QCOW_NAME, base, name);
    for (i = 0; i < blocks; i++)
        put_be(buf + cluster + i * 8, (1 + table + i) * cluster, 8);
    for (i = 0; i < total; i++)
        put_be(buf + (1 + table) * cluster + i * 2, 1, 2);
    if (write(fd, buf, (size_t)((1 + table + blocks) * cluster)) !=
            (ssize_t)((1 + table + blocks) * cluster) ||
        ftruncate(fd, (off_t)(total * cluster)) != 0) {
        e = errno != 0 ? errno : EIO;
        free(buf);
        errno = e;
        return -1;
    }
    free(buf);
    return 0;
}

int etude_store_overlay(const struct etude_store *s, const char *digest, uint32_t cluster_bytes,
                        const char *dest)
{
    char *path, *base;
    const char *format = "raw";
    struct stat st;
    uint64_t size = 0;
    unsigned bits = 9;
    int fd, e;

    while (bits < 21 && ((uint32_t)1 << bits) < cluster_bytes)
        bits++;
    if (!store_is_digest(digest) || cluster_bytes != ((uint32_t)1 << bits)) {
        errno = EINVAL;
        return -1;
    }
    path = store_join(s->objects, digest);
    if (path == NULL) {
        errno = ENOMEM;
        return -1;
    }
    base = realpath(path, NULL);
    free(path);
    if (base == NULL)
        return -1;
    if (stat(base, &st) != 0 ||
        (S_ISREG(st.st_mode) && base_format(base, &st, &format, &size) != 0)) {
        e = errno;
        free(base);
        errno = e;
        return -1;
    }
    if (!S_ISREG(st.st_mode) || size == 0 || size % 512 != 0) {
        free(base);
        errno = EINVAL;
        return -1;
    }
    fd = open(dest, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) {
        free(base);
        return -1;
    }
    errno = 0;
    if (write_overlay(fd, base, format, size, bits) != 0 || close(fd) != 0) {
        e = errno;
        close(fd);
        unlink(dest);
        free(base);
        errno = e;
        return -1;
    }
    free(base);
    return 0;
}

int etude_store_sweep(struct etude_store *s, size_t *removed)
{
    struct dirent *d;
    size_t count = 0;
    char *path;
    DIR *dir;
    int fd;

    dir = opendir(s->tmp);
    if (dir == NULL)
        return -1;
    while ((d = readdir(dir)) != NULL) {
        if (strncmp(d->d_name, PART_PREFIX, strlen(PART_PREFIX)) != 0)
            continue;
        path = store_join(s->tmp, d->d_name);
        if (path == NULL)
            continue;
        fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0) {
            if (lock_try(fd, F_RDLCK) == 0 && unlink(path) == 0)
                count++;
            close(fd);
        }
        free(path);
    }
    closedir(dir);
    if (removed != NULL)
        *removed = count;
    return 0;
}

/* ---- Records and locks of the nodes' keys. ---- */

/**
 * Take the line of `*p` that begins with `field` and a space, and give its
 * value and length; 0, or -1 for another line or none.
 */
static int take_line(const char **p, const char *field, const char **value, size_t *len)
{
    size_t n = strlen(field);
    const char *nl;

    if (strncmp(*p, field, n) != 0 || (*p)[n] != ' ')
        return -1;
    *value = *p + n + 1;
    nl = strchr(*value, '\n');
    if (nl == NULL || nl == *value)
        return -1;
    *len = (size_t)(nl - *value);
    *p = nl + 1;
    return 0;
}

/**
 * Parse record `text` of key `key`, which must be exactly the lines
 * store_record_write writes; 0 with its object and size, or -1.
 */
static int record_parse(const char *text, const char *key, char digest[ETUDE_DIGEST_HEX + 1],
                        uint64_t *size)
{
    const char *p = text, *k, *d, *z, *node;
    size_t nk, nd, nz, nn, i;
    uint64_t n = 0;

    if (strncmp(p, RECORD_MAGIC "\n", strlen(RECORD_MAGIC) + 1) != 0)
        return -1;
    p += strlen(RECORD_MAGIC) + 1;
    if (take_line(&p, "key", &k, &nk) != 0 || take_line(&p, "object", &d, &nd) != 0 ||
        take_line(&p, "size", &z, &nz) != 0 || take_line(&p, "node", &node, &nn) != 0 || *p != '\0')
        return -1;
    if (nk != ETUDE_DIGEST_HEX || strncmp(k, key, nk) != 0 || nd != ETUDE_DIGEST_HEX || nz > 19)
        return -1;
    memcpy(digest, d, ETUDE_DIGEST_HEX);
    digest[ETUDE_DIGEST_HEX] = '\0';
    if (!store_is_digest(digest))
        return -1;
    for (i = 0; i < nz; i++) {
        if (z[i] < '0' || z[i] > '9')
            return -1;
        n = n * 10 + (uint64_t)(z[i] - '0');
    }
    *size = n;
    return 0;
}

int store_record_read(const struct etude_store *s, const char *key,
                      char digest[ETUDE_DIGEST_HEX + 1], uint64_t *size)
{
    char buf[RECORD_MAX + 1];
    char *path = store_join(s->nodes, key);
    size_t n = 0;
    ssize_t r;
    int fd;

    if (path == NULL) {
        errno = ENOMEM;
        return -1;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    free(path);
    if (fd < 0)
        return -1;
    while (n < RECORD_MAX) {
        r = read(fd, buf + n, RECORD_MAX - n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        n += (size_t)r;
    }
    close(fd);
    buf[n] = '\0';
    if (n == RECORD_MAX || strlen(buf) != n || record_parse(buf, key, digest, size) != 0) {
        errno = EBADMSG;
        return -1;
    }
    return 0;
}

int store_record_write(struct etude_store *s, const char *key, const char *node, const char *digest,
                       uint64_t size)
{
    char *part = NULL, *text;
    int fd = -1, n;

    n = snprintf(NULL, 0, RECORD_MAGIC "\nkey %s\nobject %s\nsize %llu\nnode %s\n", key, digest,
                 (unsigned long long)size, node);
    if (n < 0 || n >= RECORD_MAX) {
        errno = EINVAL;
        return -1;
    }
    text = malloc((size_t)n + 1);
    if (text == NULL) {
        errno = ENOMEM;
        return -1;
    }
    snprintf(text, (size_t)n + 1, RECORD_MAGIC "\nkey %s\nobject %s\nsize %llu\nnode %s\n", key,
             digest, (unsigned long long)size, node);
    if (part_open(s, &part, &fd) != 0) {
        free(text);
        return -1;
    }
    if (write_all(fd, text, (size_t)n) != 0 || fsync(fd) != 0 || fchmod(fd, 0444) != 0 ||
        publish(s->nodes, key, part, NULL, s) != 0) {
        free(text);
        part_drop(part, fd);
        return -1;
    }
    free(text);
    part_drop(part, fd);
    return 0;
}

int store_lookup(const struct etude_store *s, const char *key, char digest[ETUDE_DIGEST_HEX + 1],
                 uint64_t *size)
{
    uint64_t found;

    if (store_record_read(s, key, digest, size) != 0)
        return -1;
    if (etude_store_check(s, digest, &found) != 0)
        return -1;
    if (found != *size) {
        errno = EBADMSG;
        return -1;
    }
    return 0;
}

int store_lock(const struct etude_store *s, const char *key)
{
    char *path = store_join(s->locks, key);
    int fd, e;

    if (path == NULL) {
        errno = ENOMEM;
        return -1;
    }
    fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
    free(path);
    if (fd < 0)
        return -1;
    if (lock_try(fd, F_WRLCK) != 0) {
        e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}
