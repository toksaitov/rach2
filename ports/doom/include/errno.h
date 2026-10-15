/*
 * errno.h: the error numbers the engine compares errno with, with Linux's
 * values, for Doom on Rach2.
 */
#ifndef _ERRNO_H
#define _ERRNO_H

#define ENOENT 2
#define ENOMEM 12
#define EACCES 13
#define EEXIST 17
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define ENOSPC 28
#define ENOTEMPTY 39

/* Each file that includes this header has an errno of its own. Nothing sets
 * it, since the system call wrappers return a negative errno instead, so it
 * is always 0. */
static int _errno_val = 0;
#define errno _errno_val

#endif /* _ERRNO_H */
