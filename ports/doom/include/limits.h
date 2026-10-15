/*
 * limits.h: the limits of the integer types, and PATH_MAX, for Doom on
 * Rach2. LONG_MIN, LONG_MAX and ULONG_MAX are those of a 32-bit long, which
 * is i386's; on x86-64, where long has 64 bits, they are too small.
 */
#ifndef _LIMITS_H
#define _LIMITS_H

#define CHAR_BIT    8
#define SCHAR_MIN   (-128)
#define SCHAR_MAX   127
#define UCHAR_MAX   255
#define CHAR_MIN    SCHAR_MIN
#define CHAR_MAX    SCHAR_MAX

#define SHRT_MIN    (-32768)
#define SHRT_MAX    32767
#define USHRT_MAX   65535

#define INT_MIN     (-2147483647 - 1)
#define INT_MAX     2147483647
#define UINT_MAX    4294967295U

#define LONG_MIN    (-2147483647L - 1)
#define LONG_MAX    2147483647L
#define ULONG_MAX   4294967295UL

#define PATH_MAX    256

#endif /* _LIMITS_H */
