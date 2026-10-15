/*
 * inttypes.h: the printf and scanf conversions of the exact-width integer
 * types, for Doom on Rach2. crt0.c's printf reads an ll argument as a long,
 * so the 64-bit macros are right only on x86-64, and its sscanf reads only
 * %d.
 */
#ifndef _INTTYPES_H
#define _INTTYPES_H

#include <stdint.h>

#define PRId8  "d"
#define PRId16 "d"
#define PRId32 "d"
#define PRId64 "lld"

#define PRIi8  "i"
#define PRIi16 "i"
#define PRIi32 "i"
#define PRIi64 "lli"

#define PRIu8  "u"
#define PRIu16 "u"
#define PRIu32 "u"
#define PRIu64 "llu"

#define PRIx8  "x"
#define PRIx16 "x"
#define PRIx32 "x"
#define PRIx64 "llx"

#define PRIX8  "X"
#define PRIX16 "X"
#define PRIX32 "X"
#define PRIX64 "llX"

#define SCNd8  "hhd"
#define SCNd16 "hd"
#define SCNd32 "d"
#define SCNd64 "lld"

#define SCNu8  "hhu"
#define SCNu16 "hu"
#define SCNu32 "u"
#define SCNu64 "llu"

#endif /* _INTTYPES_H */
