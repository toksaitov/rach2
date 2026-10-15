/*
 * unistd.h: the descriptor calls of Doom on Rach2, and stubs of the other
 * calls the engine names.
 */
#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* read, write and close are the system calls, in crt0.c. Each returns the
 * kernel's answer, a count or 0, or a negative errno, and does not set
 * errno. */
int read(int fd, void *buf, size_t count);
int write(int fd, const void *buf, size_t count);
int close(int fd);

/** A stub: returns 0 at once, without waiting. */
static inline int usleep(unsigned int usec) {
    (void)usec;
    return 0;
}

/** A stub: returns 0 at once, without waiting. */
static inline unsigned int sleep(unsigned int seconds) {
    (void)seconds;
    return 0;
}

/** Nonzero for descriptors 0, 1 and 2, whatever they are open on. */
static inline int isatty(int fd) {
    return fd >= 0 && fd <= 2;
}

/** A stub: "/" in `buf`, which must hold 2 bytes, whatever the working
 * directory is. */
static inline char *getcwd(char *buf, size_t size) {
    if (buf && size > 0) {
        buf[0] = '/';
        buf[1] = '\0';
    }
    return buf;
}

/** A stub: changes nothing and returns 0. */
static inline int chdir(const char *path) {
    (void)path;
    return 0;
}

/** A stub: fails for every path, with -1. */
static inline int access(const char *path, int mode) {
    (void)path; (void)mode;
    return -1;
}

#endif /* _UNISTD_H */
