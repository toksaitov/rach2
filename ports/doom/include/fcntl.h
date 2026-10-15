/*
 * fcntl.h: the open flags, with Linux's values, and open, for Doom on Rach2.
 */
#ifndef _FCNTL_H
#define _FCNTL_H

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0100
#define O_TRUNC   01000
#define O_APPEND  02000

/**
 * The open system call, in crt0.c. The third argument, the mode of a file
 * created, is read only when `flags` has O_CREAT. Returns the descriptor or
 * a negative errno, and does not set errno.
 */
int open(const char *path, int flags, ...);

#endif /* _FCNTL_H */
