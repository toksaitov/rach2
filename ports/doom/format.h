/*
 * format.h: the formatter behind the Doom port's printf family, which
 * crt0.c's printf, fprintf, sprintf, snprintf and vsnprintf call.
 */
#ifndef DOOM_FORMAT_H
#define DOOM_FORMAT_H

#include <stdarg.h>
#include <stddef.h>

/**
 * Format `fmt` and its arguments as glibc's vsnprintf does, store at most
 * `size` - 1 characters of the result in `buf` and terminate it; a `size`
 * of 0 stores nothing and `buf` may then be NULL.
 *
 * Every flag, a width and a precision, given or as `*`, and the length
 * modifiers hh, h, l, ll, L, j, z and t apply to d, i, u, o, x, X, c, s,
 * p, n, f, F, e, E, g and G as C99 and glibc define them; a float is
 * rounded to the nearest, ties to even, from its exact value. A NULL
 * string prints "(null)", a NULL pointer "(nil)", and an unknown
 * conversion prints itself, '%' to the conversion character, and takes no
 * argument. %Lf reads a long double and formats it as a double.
 *
 * Returns the length of the whole result, without the terminator, which
 * exceeds `size` - 1 when the result was cut.
 */
int format_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#endif /* DOOM_FORMAT_H */
