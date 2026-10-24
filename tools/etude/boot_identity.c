/* boot_identity.c: recognize Rach2 and read its published boot identity. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#endif
#include "boot_identity.h"
#include <errno.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

int etude_boot_is_rach2(const char *system, const char *release, const char *version)
{
    size_t n = strlen(release);
    return !strcmp(system, "Linux") && n > 6 && !strcmp(release + n - 6, "-rach2") &&
           !strncmp(version, "#1 Rach2 ", 9) && version[9] != '\0';
}

int etude_boot_file_read(const char *path, char *out, size_t size)
{
    if (size)
        out[0] = '\0';
#ifdef _WIN32
    (void)path;
    errno = ENOSYS;
    return -1;
#else
    char bytes[65];
    struct stat st;
    size_t used = 0;
    int fd, ok;
    if (size < 65) {
        errno = EINVAL;
        return -1;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return -1;
    ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 64 &&
         (st.st_uid == 0 || st.st_uid == geteuid()) && !(st.st_mode & 022);
    while (ok && used < sizeof(bytes)) {
        ssize_t n = read(fd, bytes + used, sizeof(bytes) - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        used += (size_t)n;
    }
    if (close(fd))
        ok = 0;
    if (!ok || used != 64) {
        errno = EINVAL;
        return -1;
    }
    bytes[64] = '\0';
    if (strspn(bytes, "0123456789abcdef") != 64) {
        errno = EINVAL;
        return -1;
    }
    memcpy(out, bytes, 65);
    return 0;
#endif
}
