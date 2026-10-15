/*
 * strings.h: bzero and bcopy, as macros over memset and memmove, for Doom on
 * Rach2.
 */
#ifndef _STRINGS_H
#define _STRINGS_H

#include <string.h>

#define bzero(s, n) memset(s, 0, n)
#define bcopy(src, dest, n) memmove(dest, src, n)

#endif /* _STRINGS_H */
