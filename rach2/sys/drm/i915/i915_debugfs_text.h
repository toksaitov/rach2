/* Text formatting for the i915 debugfs files. */
#ifndef _I915_DEBUGFS_TEXT_H_
#define _I915_DEBUGFS_TEXT_H_

#include <stdarg.h>

#include "../../timer.h"

/* Text made into `cap` bytes at `buf`, `len` bytes of it so far, counted
 * past `cap` so that a show returns the length it needs. */
struct emit {
    char *buf;
    uint32_t cap;
    uint32_t len;
};

/** Append the byte `c` to `e`, as far as its room goes. */
static void emit_c(struct emit *e, char c)
{
    if (e->len < e->cap) {
        e->buf[e->len] = c;
    }
    e->len++;
}

/** Append the string `s`. */
static void emit_s(struct emit *e, const char *s)
{
    while (*s) {
        emit_c(e, *s++);
    }
}

/** Append `v` in base `radix`, 10 or 16, in at least `width` columns,
 * padded with zeros when `zero`, with a minus sign when `neg`. */
static void emit_n(struct emit *e, uint64_t v, uint32_t radix, int neg, uint32_t width, int zero)
{
    char digits[24];
    uint32_t n = 0, rem, k;

    do {
        v = tick_div(v, radix, &rem);
        digits[n++] = "0123456789abcdef"[rem];
    } while (v);
    k = n + (neg ? 1u : 0u);
    for (; !zero && k < width; k++) {
        emit_c(e, ' ');
    }
    if (neg) {
        emit_c(e, '-');
    }
    for (; zero && k < width; k++) {
        emit_c(e, '0');
    }
    while (n > 0) {
        emit_c(e, digits[--n]);
    }
}

/**
 * Append `fmt` with its arguments: `%s`, `%c`, `%d`, `%u` and `%x`, each
 * with an optional `0` flag and a width, and an `L` before a number's
 * letter for a 64-bit argument; `%%` for a percent sign.
 */
static void emitf(struct emit *e, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        uint32_t width = 0;
        int zero = 0, wide = 0;
        uint64_t v;
        int64_t sv;

        if (*fmt != '%') {
            emit_c(e, *fmt);
            continue;
        }
        fmt++;
        if (*fmt == '0') {
            zero = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10u + (uint32_t)(*fmt++ - '0');
        }
        if (*fmt == 'L') {
            wide = 1;
            fmt++;
        }
        switch (*fmt) {
        case 's':
            emit_s(e, va_arg(ap, const char *));
            break;
        case 'c':
            emit_c(e, (char)va_arg(ap, int));
            break;
        case 'd':
            sv = wide ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);
            emit_n(e, sv < 0 ? (uint64_t)-sv : (uint64_t)sv, 10, sv < 0, width, zero);
            break;
        case 'u':
        case 'x':
            v = wide ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned);
            emit_n(e, v, *fmt == 'x' ? 16u : 10u, 0, width, zero);
            break;
        case '%':
            emit_c(e, '%');
            break;
        default:
            va_end(ap);
            return;
        }
    }
    va_end(ap);
}

/** The length `e` needs, as a show returns it, with the text ended when
 * it fit. */
static int32_t emit_end(struct emit *e)
{
    if (e->len < e->cap) {
        e->buf[e->len] = '\0';
    }
    return (int32_t)e->len;
}

#endif /* _I915_DEBUGFS_TEXT_H_ */
