/*
 * fcntl.h - File control for Doom on Rach2
 */
#ifndef _FCNTL_H
#define _FCNTL_H

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0100
#define O_TRUNC   01000
#define O_APPEND  02000

int open(const char *path, int flags, ...);

#endif /* _FCNTL_H */
