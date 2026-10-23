/* The pinned BSD strptime port needs only these C-runtime definitions. */
#ifndef RACH2_MANDOC_STRPTIME_H
#define RACH2_MANDOC_STRPTIME_H
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MSWIN             1
#define UNUSED            __attribute__((unused))
#define STRNICMP(a, b, n) _strnicmp((const char *)(a), (const char *)(b), (n))
char *strptime(const char *, const char *, struct tm *);
#endif
