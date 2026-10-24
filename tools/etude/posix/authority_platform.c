#ifndef _WIN32
/* POSIX authority gates are open-description locks, including across fork. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <copyfile.h>
#else
#include <sys/xattr.h>
#endif
#include "../authority_platform.h"

const char *authority_installed_binding(void)
{
#ifdef RACH2_HOSTTEST
    const char *test = getenv("ETUDE_TEST_AUTHORITY");
    if (test)
        return test;
#endif
    return "/etc/rach2/etude-authority";
}

int authority_file_id(const char *path, struct authority_file_id *identity)
{
    struct stat st;
    if (stat(path, &st))
        return -1;
    identity->device = (uint64_t)st.st_dev;
    identity->inode = (uint64_t)st.st_ino;
    return 0;
}

static int operator_uid(const char *text, uid_t *uid)
{
    char *end;
    if (!text || !*text || strspn(text, "0123456789") != strlen(text))
        return -1;
    errno = 0;
    unsigned long long n = strtoull(text, &end, 10);
    *uid = (uid_t)n;
    return errno || *end || (unsigned long long)*uid != n ? -1 : 0;
}

int authority_operator(const char *text)
{
    uid_t uid;
    return !operator_uid(text, &uid) && geteuid() == uid;
}

int authority_protected(const char *path, const char *text, int private_file)
{
    struct stat st;
    uid_t uid;
    if (operator_uid(text, &uid) || lstat(path, &st) ||
        (private_file < 0 ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) ||
        (st.st_uid != 0 && st.st_uid != uid) || (st.st_mode & 022) ||
        (private_file > 0 && (st.st_uid != uid || (st.st_mode & 077)))) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int authority_gate_open(const char *path, int exclusive, struct authority_gate *gate)
{
    int fd = open(path, (exclusive ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW);
    gate->value = -1;
    if (fd < 0)
        return -1;
    if (flock(fd, (exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB)) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    gate->value = fd;
    return 0;
}

void authority_gate_close(struct authority_gate *gate)
{
    if (gate->value >= 0)
        close((int)gate->value);
    gate->value = -1;
}

/* Replace policy bytes only after preserving the original access metadata.
 * An unsupported or inaccessible attribute fails before publication. */
static int copy_access(int original, int target)
{
#ifdef __APPLE__
    return fcopyfile(original, target, NULL, COPYFILE_METADATA);
#else
    ssize_t size = flistxattr(original, NULL, 0);
    if (size < 0)
        return errno == ENOTSUP ? 0 : -1;
    char *names = malloc((size_t)size + 1);
    if (!names)
        return -1;
    int error = flistxattr(original, names, (size_t)size) != size;
    for (ssize_t at = 0; !error && at < size;) {
        const char *name = names + at;
        size_t n = strnlen(name, (size_t)(size - at));
        if (!n || n == (size_t)(size - at)) {
            error = 1;
            break;
        }
        ssize_t bytes = fgetxattr(original, name, NULL, 0);
        void *value = bytes < 0 ? NULL : malloc((size_t)bytes + 1);
        if (!value || fgetxattr(original, name, value, (size_t)bytes) != bytes ||
            fsetxattr(target, name, value, (size_t)bytes, 0))
            error = 1;
        free(value);
        at += (ssize_t)n + 1;
    }
    free(names);
    return error ? -1 : 0;
#endif
}

static int preserve_access(const char *path, int target, const struct stat *previous)
{
    if (!S_ISREG(previous->st_mode) || fchown(target, previous->st_uid, previous->st_gid) ||
        fchmod(target, previous->st_mode & 0777))
        return -1;
    int original = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat actual;
    int error = original < 0 || fstat(original, &actual) || actual.st_dev != previous->st_dev ||
                actual.st_ino != previous->st_ino || copy_access(original, target);
    if (original >= 0)
        close(original);
    return error ? -1 : 0;
}

static int write_all(int fd, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    size_t at = 0;
    while (at < size) {
        ssize_t n = write(fd, bytes + at, size - at);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!n)
                errno = EIO;
            return -1;
        }
        at += (size_t)n;
    }
    return fsync(fd);
}

static int flush_parent(const char *path)
{
    char parent[4096];
    memcpy(parent, path, strlen(path) + 1);
    char *slash = strrchr(parent, '/');
    if (!slash) {
        errno = EINVAL;
        return -1;
    }
    *slash = 0;
    int fd = open(*parent ? parent : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int error = fsync(fd), saved = errno;
    if (close(fd) && !error)
        return -1;
    errno = saved;
    return error;
}

int authority_publish(const char *path, const void *data, size_t size, int private_file)
{
    char temporary[4096];
    int n = snprintf(temporary, sizeof temporary, "%s.route-%ld", path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof temporary) {
        errno = ENAMETOOLONG;
        return -1;
    }
    struct stat previous;
    int existed = lstat(path, &previous) == 0;
    if (!existed && errno != ENOENT)
        return -1;
    int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                  private_file ? 0600 : 0644);
    if (fd < 0)
        return -1;
    int error = 0;
    if (existed && preserve_access(path, fd, &previous))
        error = errno ? errno : EACCES;
    if (!error && write_all(fd, data, size))
        error = errno;
    if (close(fd) && !error)
        error = errno;
    if (!error && rename(temporary, path))
        error = errno;
    if (error)
        unlink(temporary);
    else if (flush_parent(path))
        error = errno;
    errno = error;
    return error ? -1 : 0;
}

int authority_nonce(char out[65])
{
    unsigned char bytes[32];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t at = 0;
    while (at < sizeof bytes) {
        ssize_t n = read(fd, bytes + at, sizeof bytes - at);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            return -1;
        }
        at += (size_t)n;
    }
    close(fd);
    for (at = 0; at < sizeof bytes; at++)
        snprintf(out + at * 2, 3, "%02x", bytes[at]);
    return 0;
}

#endif
