/*
 * store.c: etude's store of immutable artifacts on POSIX systems. An object
 * is written under a partial name, synchronized, and then linked to the name
 * of its digest; a record of a node's key names the object it published.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

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

struct etude_store {
    char *dir;
    char *objects; /* DIGEST: the objects, read-only */
    char *nodes;   /* KEY: the records of the nodes' products */
    char *locks;   /* KEY: the locks of preparations under way */
    char *tmp;     /* part.XXXXXX: partial files, each locked by its writer */
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
 * Read `from` to its end, hashing it, and write it to `to` unless `to` is
 * negative; 0 with the digest and size, or -1 with errno set.
 *
 * `to` is a new regular file. A chunk of zeros is passed over with
 * lseek(2), which leaves a hole in `to`, so that a sparse file stays
 * sparse; `to` is then given the size read, for a file that ends in zeros.
 */
static int copy_hash(int from, int to, char hex[ETUDE_DIGEST_HEX + 1], uint64_t *size)
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
    sha256_init(&h);
    for (;;) {
        r = read(from, buf, COPY_CHUNK);
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0)
            goto fail;
        if (r == 0)
            break;
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
    sha256_final(&h, sum);
    sha256_hex(sum, hex);
    if (size != NULL)
        *size = total;
    return 0;
fail:
    free(buf);
    return -1;
}

/** Lock all of `fd` for `type` without waiting; 0, or -1 with errno set. */
static int lock_try(int fd, short type)
{
    struct flock fl;

    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &fl) == 0)
        return 0;
    if (errno == EACCES)
        errno = EAGAIN;
    return -1;
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
    free(s);
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
    struct stat st;
    char *path;
    int fd, e;

    if (!store_is_digest(digest)) {
        errno = EINVAL;
        return -1;
    }
    path = store_join(s->objects, digest);
    if (path == NULL) {
        errno = ENOMEM;
        return -1;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    free(path);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) != 0) {
        e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        errno = EBADMSG;
        return -1;
    }
    if (copy_hash(fd, -1, hex, size) != 0) {
        e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    close(fd);
    if (strcmp(hex, digest) != 0) {
        errno = EBADMSG;
        return -1;
    }
    return 0;
}

/** Check object `digest` as publish's test of a file already there. */
static int object_valid(const struct etude_store *s, const char *digest)
{
    return etude_store_check(s, digest, NULL);
}

int etude_store_put(struct etude_store *s, const char *path, const char *expect,
                    char digest[ETUDE_DIGEST_HEX + 1], uint64_t *size)
{
    char *part = NULL;
    int from, fd = -1, e;

    if (expect != NULL && !store_is_digest(expect)) {
        errno = EINVAL;
        return -1;
    }
    from = open(path, O_RDONLY | O_CLOEXEC);
    if (from < 0)
        return -1;
    if (part_open(s, &part, &fd) != 0) {
        e = errno;
        close(from);
        errno = e;
        return -1;
    }
    if (copy_hash(from, fd, digest, size) != 0 || fsync(fd) != 0)
        goto fail;
    close(from);
    from = -1;
    if (expect != NULL && strcmp(expect, digest) != 0) {
        errno = EBADMSG;
        goto fail;
    }
    if (fchmod(fd, 0444) != 0 || publish(s->objects, digest, part, object_valid, s) != 0)
        goto fail;
    part_drop(part, fd);
    return 0;
fail:
    if (from >= 0)
        close(from);
    part_drop(part, fd);
    return -1;
}

int etude_store_copy(const struct etude_store *s, const char *digest, const char *dest)
{
    char hex[ETUDE_DIGEST_HEX + 1];
    char *path;
    int from, to, e;

    if (!store_is_digest(digest)) {
        errno = EINVAL;
        return -1;
    }
    path = store_join(s->objects, digest);
    if (path == NULL) {
        errno = ENOMEM;
        return -1;
    }
    from = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    free(path);
    if (from < 0)
        return -1;
    to = open(dest, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (to < 0) {
        e = errno;
        close(from);
        errno = e;
        return -1;
    }
    if (copy_hash(from, to, hex, NULL) != 0 || close(to) != 0) {
        e = errno;
        close(from);
        unlink(dest);
        errno = e;
        return -1;
    }
    close(from);
    if (strcmp(hex, digest) != 0) {
        unlink(dest);
        errno = EBADMSG;
        return -1;
    }
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
