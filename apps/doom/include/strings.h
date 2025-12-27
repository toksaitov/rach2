/*
 * strings.h - String functions (BSD compatibility) for Doom on Rach2
 */
#ifndef _STRINGS_H
#define _STRINGS_H

#include <string.h>

/* BSD functions aliased to POSIX equivalents */
#define bzero(s, n) memset(s, 0, n)
#define bcopy(src, dest, n) memmove(dest, src, n)

#endif /* _STRINGS_H */
