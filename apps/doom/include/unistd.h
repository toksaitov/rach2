/*
 * unistd.h - Unix standard functions for Doom on Rach2
 */
#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

int read(int fd, void *buf, size_t count);
int write(int fd, const void *buf, size_t count);
int close(int fd);

/* Not implemented but may be needed */
static inline int usleep(unsigned int usec) {
    (void)usec;
    return 0;
}

static inline unsigned int sleep(unsigned int seconds) {
    (void)seconds;
    return 0;
}

static inline int isatty(int fd) {
    return fd >= 0 && fd <= 2;
}

static inline char *getcwd(char *buf, size_t size) {
    if (buf && size > 0) {
        buf[0] = '/';
        buf[1] = '\0';
    }
    return buf;
}

static inline int chdir(const char *path) {
    (void)path;
    return 0;
}

static inline int access(const char *path, int mode) {
    (void)path; (void)mode;
    return -1;
}

#endif /* _UNISTD_H */
