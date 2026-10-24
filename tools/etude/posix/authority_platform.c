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
    if (operator_uid(text, &uid) || lstat(path, &st) || (private_file < 0 ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) ||
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
    const unsigned char *bytes = data;
    size_t at = 0;
    int error = 0;
    if (existed && (!S_ISREG(previous.st_mode) ||
        fchown(fd, previous.st_uid, previous.st_gid) ||
        fchmod(fd, private_file ? 0600 : previous.st_mode & 0777)))
        error = errno ? errno : EACCES;
    while (!error && at < size) {
        ssize_t wrote = write(fd, bytes + at, size - at);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0) {
            error = wrote < 0 ? errno : EIO;
            break;
        }
        at += (size_t)wrote;
    }
    if (!error && fsync(fd))
        error = errno;
    if (close(fd) && !error)
        error = errno;
    if (!error && rename(temporary, path))
        error = errno;
    if (error)
        unlink(temporary);
    else {
        char parent[4096];
        memcpy(parent, path, strlen(path) + 1);
        char *slash = strrchr(parent, '/');
        if (!slash)
            error = EINVAL;
        else {
            *slash = 0;
            fd = open(*parent ? parent : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (fd < 0)
                error = errno;
            else {
                if (fsync(fd))
                    error = errno;
                close(fd);
            }
        }
    }
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
