/*
 * format.c: the conversions of the Doom port's printf family, as glibc
 * formats them, in C that calls no library and makes no system call.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "format.h"

/* A destination: `len` counts every character formatted, stored or not. */
struct out {
    char *buf;
    size_t size;
    size_t len;
};

/* One conversion's flags, width and precision. */
struct spec {
    int minus; /* '-': pad on the right */
    int plus;  /* '+': a sign on a positive number */
    int space; /* ' ': a space on a positive number */
    int alt;   /* '#': the alternative form */
    int zero;  /* '0': pad with zeros after the sign */
    int width; /* 0 when none */
    int prec;  /* -1 when none */
};

/*
 * The exact decimal value of a double: 0.D * 10^dp, D the n digits of
 * `d`, with no trailing zero. Zero has n == 0. A double's exact value
 * has at most 767 significant digits, a subnormal's 2^52 * 5^1074.
 */
#define DIGITS_MAX 800

struct decimal {
    char d[DIGITS_MAX];
    int n;
    int dp;
};

/* Base 10^9 limbs, least significant first, for the exact conversion. */
#define LIMB_BASE 1000000000u
#define LIMBS_MAX 90

/** Store `c` when it fits below the terminator's place, and count it. */
static void put(struct out *o, char c)
{
    if (o->len + 1 < o->size) o->buf[o->len] = c;
    o->len++;
}

/** Put `n` copies of `c`; nothing when `n` is not positive. */
static void fill(struct out *o, char c, long n)
{
    while (n-- > 0) put(o, c);
}

/** Put the `n` characters of `s`. */
static void put_n(struct out *o, const char *s, size_t n)
{
    while (n--) put(o, *s++);
}

/**
 * Put the spaces that bring a field of `total` characters to the width:
 * field_pad_left before the field unless the '-' flag is given, and
 * field_pad_right after it when it is.
 */
static void field_pad_left(struct out *o, const struct spec *sp, long total)
{
    if (!sp->minus) fill(o, ' ', sp->width - total);
}

static void field_pad_right(struct out *o, const struct spec *sp, long total)
{
    if (sp->minus) fill(o, ' ', sp->width - total);
}

/**
 * Put an integer's field: `prefix` (a sign or 0x), `zeros` zeros of
 * precision, and the `n` digits of `digits`. The '0' flag, honoured only
 * when `zero_ok`, pads with zeros between the prefix and the digits.
 */
static void put_number(struct out *o, const struct spec *sp, int zero_ok,
                       const char *prefix, int plen, long zeros,
                       const char *digits, int n)
{
    long total = plen + zeros + n;

    if (zero_ok && sp->zero && !sp->minus && sp->width > total) {
        zeros += sp->width - total;
        total = sp->width;
    }
    field_pad_left(o, sp, total);
    put_n(o, prefix, plen);
    fill(o, '0', zeros);
    put_n(o, digits, n);
    field_pad_right(o, sp, total);
}

/**
 * Format an integer conversion, `conv` one of d, i, u, o, x, X or p, of
 * the magnitude `u` and the sign `neg`. A precision of 0 prints no digit
 * for zero, and turns off the '0' flag, as any precision does.
 */
static void format_int(struct out *o, struct spec *sp, char conv,
                       unsigned long long u, int neg)
{
    char tmp[24];
    char prefix[2];
    int plen = 0;
    int n = 0;
    int i;
    long zeros;
    unsigned base = (conv == 'o') ? 8 : (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : 10;
    const char *digits = (conv == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
    unsigned long long v = u;

    while (v) {
        tmp[sizeof(tmp) - 1 - n++] = digits[v % base];
        v /= base;
    }
    if (u == 0 && sp->prec != 0) tmp[sizeof(tmp) - 1 - n++] = '0';

    if (conv == 'd' || conv == 'i') {
        if (neg) prefix[plen++] = '-';
        else if (sp->plus) prefix[plen++] = '+';
        else if (sp->space) prefix[plen++] = ' ';
    } else if (conv == 'p') {
        if (sp->plus) prefix[plen++] = '+';
        else if (sp->space) prefix[plen++] = ' ';
    }

    zeros = (sp->prec > n) ? sp->prec - n : 0;
    if (conv == 'o' && sp->alt && zeros == 0 &&
        (n == 0 || tmp[sizeof(tmp) - n] != '0')) {
        zeros = 1;
    }

    if (((conv == 'x' || conv == 'X') && sp->alt && u != 0) || conv == 'p') {
        char p[4];
        int pl = 0;
        for (i = 0; i < plen; i++) p[pl++] = prefix[i];
        p[pl++] = '0';
        p[pl++] = (conv == 'X') ? 'X' : 'x';
        put_number(o, sp, sp->prec < 0, p, pl, zeros, tmp + sizeof(tmp) - n, n);
        return;
    }
    put_number(o, sp, sp->prec < 0, prefix, plen, zeros, tmp + sizeof(tmp) - n, n);
}

/**
 * Put the `n` bytes of `s` in a field of the width, padded with spaces.
 */
static void format_str(struct out *o, const struct spec *sp, const char *s, long n)
{
    field_pad_left(o, sp, n);
    put_n(o, s, n);
    field_pad_right(o, sp, n);
}

/**
 * Multiply the `*nl` limbs of `l` by `m`, below 2^32, growing `*nl` with
 * the carry.
 */
static void limbs_mul(uint32_t *l, int *nl, uint32_t m)
{
    uint64_t carry = 0;
    int i;

    for (i = 0; i < *nl; i++) {
        uint64_t t = (uint64_t)l[i] * m + carry;
        l[i] = (uint32_t)(t % LIMB_BASE);
        carry = t / LIMB_BASE;
    }
    while (carry && *nl < LIMBS_MAX) {
        l[(*nl)++] = (uint32_t)(carry % LIMB_BASE);
        carry /= LIMB_BASE;
    }
}

/**
 * Write the exact decimal value of the finite, nonnegative `bits`, a
 * double's IEEE 754 encoding without its sign, to `x`: the significand
 * times 2^e is the integer m * 2^e when e >= 0, or m * 5^-e / 10^-e when
 * e < 0, either of which the limbs hold exactly.
 */
static void to_decimal(struct decimal *x, uint64_t bits)
{
    uint32_t l[LIMBS_MAX];
    int nl = 0;
    int bexp = (int)((bits >> 52) & 0x7ff);
    uint64_t m = bits & ((1ULL << 52) - 1);
    int e;
    int i, j;
    char tmp[9];

    if (bexp == 0) {
        e = -1074;
    } else {
        m |= 1ULL << 52;
        e = bexp - 1075;
    }
    x->n = 0;
    x->dp = 1;
    if (m == 0) return;
    while (!(m & 1) && e < 0) {
        m >>= 1;
        e++;
    }

    while (m) {
        l[nl++] = (uint32_t)(m % LIMB_BASE);
        m /= LIMB_BASE;
    }
    for (; e >= 29; e -= 29) limbs_mul(l, &nl, 1u << 29);
    if (e > 0) {
        limbs_mul(l, &nl, 1u << e);
        e = 0;
    }
    for (i = -e; i >= 13; i -= 13) limbs_mul(l, &nl, 1220703125u); /* 5^13 */
    for (; i > 0; i--) limbs_mul(l, &nl, 5);

    for (i = nl - 1; i >= 0; i--) {
        uint32_t v = l[i];
        for (j = 8; j >= 0; j--) {
            tmp[j] = (char)('0' + v % 10);
            v /= 10;
        }
        j = 0;
        if (i == nl - 1)
            while (j < 8 && tmp[j] == '0') j++;
        for (; j < 9; j++) x->d[x->n++] = tmp[j];
    }
    x->dp = x->n + e;
    while (x->n > 0 && x->d[x->n - 1] == '0') x->n--;
}

/**
 * Round `x` to its first `keep` digits, to the nearest, ties to even. A
 * `keep` below 1 rounds at a place above the first digit, to 0 or to
 * 10^dp.
 */
static void round_digits(struct decimal *x, int keep)
{
    int up;
    int i;

    if (keep >= x->n) return;
    if (keep < 0) {
        x->n = 0;
        return;
    }
    up = x->d[keep] > '5' ||
         (x->d[keep] == '5' &&
          (x->n > keep + 1 || (keep > 0 && ((x->d[keep - 1] - '0') & 1))));
    x->n = keep;
    if (up) {
        for (i = keep - 1; i >= 0 && x->d[i] == '9'; i--) x->n--;
        if (i < 0) {
            x->d[0] = '1';
            x->n = 1;
            x->dp++;
        } else {
            x->d[i]++;
        }
    }
    while (x->n > 0 && x->d[x->n - 1] == '0') x->n--;
}

/** The digit of `x` at index `i` from its first, 0 past either end. */
static char digit_at(const struct decimal *x, long i)
{
    return (i >= 0 && i < x->n) ? x->d[i] : '0';
}

/**
 * Format a float conversion, `conv` one of f, F, e, E, g or G, of the
 * double whose encoding is `bits`.
 */
static void format_float(struct out *o, struct spec *sp, char conv, uint64_t bits)
{
    struct decimal x;
    char sign = 0;
    int upper = (conv == 'F' || conv == 'E' || conv == 'G');
    int prec = sp->prec < 0 ? 6 : sp->prec;
    int style = conv | 0x20; /* 'f' or 'e' */
    long fd;                 /* the fraction's digits */
    long total;
    long i;
    int exp10 = 0;
    char ebuf[8];
    int elen = 0;

    if (bits >> 63) sign = '-';
    else if (sp->plus) sign = '+';
    else if (sp->space) sign = ' ';
    bits &= ~(1ULL << 63);

    if (((bits >> 52) & 0x7ff) == 0x7ff) {
        const char *s = (bits & ((1ULL << 52) - 1))
            ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        total = 3 + (sign != 0);
        field_pad_left(o, sp, total);
        if (sign) put(o, sign);
        put_n(o, s, 3);
        field_pad_right(o, sp, total);
        return;
    }

    to_decimal(&x, bits);
    if (style == 'g') {
        int p = prec == 0 ? 1 : prec;
        int xe;
        round_digits(&x, p);
        xe = x.n ? x.dp - 1 : 0;
        if (p > xe && xe >= -4) {
            style = 'f';
            fd = p - 1 - xe;
            if (!sp->alt) {
                long need = x.n - x.dp;
                if (need < 0) need = 0;
                if (need < fd) fd = need;
            }
        } else {
            style = 'e';
            fd = p - 1;
            if (!sp->alt && x.n - 1 < fd) fd = x.n > 0 ? x.n - 1 : 0;
        }
    } else if (style == 'e') {
        fd = prec;
        round_digits(&x, prec + 1);
    } else {
        fd = prec;
        round_digits(&x, x.dp + prec);
    }

    if (style == 'e') {
        int a;
        exp10 = x.n ? x.dp - 1 : 0;
        a = exp10 < 0 ? -exp10 : exp10;
        ebuf[elen++] = upper ? 'E' : 'e';
        ebuf[elen++] = exp10 < 0 ? '-' : '+';
        if (a >= 100) ebuf[elen++] = (char)('0' + a / 100);
        ebuf[elen++] = (char)('0' + a / 10 % 10);
        ebuf[elen++] = (char)('0' + a % 10);
        total = 1 + elen;
    } else {
        total = (x.n && x.dp > 0) ? x.dp : 1;
    }
    if (fd > 0 || sp->alt) total += 1 + fd;
    if (sign) total++;

    if (!(sp->zero && !sp->minus)) field_pad_left(o, sp, total);
    if (sign) put(o, sign);
    if (sp->zero && !sp->minus) fill(o, '0', sp->width - total);

    if (style == 'e') {
        put(o, digit_at(&x, 0));
        if (fd > 0 || sp->alt) put(o, '.');
        for (i = 1; i <= fd; i++) put(o, digit_at(&x, i));
        put_n(o, ebuf, elen);
    } else {
        if (x.n && x.dp > 0) {
            for (i = 0; i < x.dp; i++) put(o, digit_at(&x, i));
        } else {
            put(o, '0');
        }
        if (fd > 0 || sp->alt) put(o, '.');
        for (i = 0; i < fd; i++) put(o, digit_at(&x, x.dp + i));
    }
    field_pad_right(o, sp, total);
}

/** The length of `s`, or of its first `max` bytes when `max` >= 0. */
static long str_len(const char *s, long max)
{
    long n = 0;
    while ((max < 0 || n < max) && s[n]) n++;
    return n;
}

int format_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct out o;
    const char *p;

    o.buf = buf;
    o.size = size;
    o.len = 0;

    for (p = fmt; *p; p++) {
        struct spec sp;
        const char *start = p;
        int len = 0; /* -2 hh, -1 h, 0 none, 1 l, 2 ll, 3 L */

        if (*p != '%') {
            put(&o, *p);
            continue;
        }
        p++;

        sp.minus = sp.plus = sp.space = sp.alt = sp.zero = 0;
        sp.width = 0;
        sp.prec = -1;
        for (;; p++) {
            if (*p == '-') sp.minus = 1;
            else if (*p == '+') sp.plus = 1;
            else if (*p == ' ') sp.space = 1;
            else if (*p == '#') sp.alt = 1;
            else if (*p == '0') sp.zero = 1;
            else break;
        }
        if (*p == '*') {
            sp.width = va_arg(ap, int);
            if (sp.width < 0) {
                sp.minus = 1;
                sp.width = -sp.width;
            }
            p++;
        } else {
            while (*p >= '0' && *p <= '9') sp.width = sp.width * 10 + (*p++ - '0');
        }
        if (*p == '.') {
            p++;
            if (*p == '*') {
                sp.prec = va_arg(ap, int);
                if (sp.prec < 0) sp.prec = -1;
                p++;
            } else {
                sp.prec = 0;
                while (*p >= '0' && *p <= '9') sp.prec = sp.prec * 10 + (*p++ - '0');
            }
        }

        switch (*p) {
            case 'h':
                len = -1;
                if (p[1] == 'h') { len = -2; p++; }
                p++;
                break;
            case 'l':
                len = 1;
                if (p[1] == 'l') { len = 2; p++; }
                p++;
                break;
            case 'L': case 'q':
                len = 3;
                p++;
                break;
            case 'j':
                len = sizeof(intmax_t) > sizeof(long) ? 2 : 1;
                p++;
                break;
            case 'z': case 't':
                len = sizeof(size_t) > sizeof(long) ? 2 : 1; /* size_t and ptrdiff_t */
                p++;
                break;
        }

        switch (*p) {
            case 'd':
            case 'i': {
                long long v;
                if (len >= 2) v = va_arg(ap, long long);
                else if (len == 1) v = va_arg(ap, long);
                else v = va_arg(ap, int);
                if (len == -1) v = (short)v;
                else if (len == -2) v = (signed char)v;
                format_int(&o, &sp, *p,
                           v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v,
                           v < 0);
                break;
            }
            case 'u':
            case 'o':
            case 'x':
            case 'X': {
                unsigned long long v;
                if (len >= 2) v = va_arg(ap, unsigned long long);
                else if (len == 1) v = va_arg(ap, unsigned long);
                else v = va_arg(ap, unsigned int);
                if (len == -1) v = (unsigned short)v;
                else if (len == -2) v = (unsigned char)v;
                format_int(&o, &sp, *p, v, 0);
                break;
            }
            case 'p': {
                void *v = va_arg(ap, void *);
                if (v) {
                    format_int(&o, &sp, 'p', (uintptr_t)v, 0);
                } else {
                    format_str(&o, &sp, "(nil)", 5);
                }
                break;
            }
            case 'c': {
                char c = (char)va_arg(ap, int);
                format_str(&o, &sp, &c, 1);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = (sp.prec < 0 || sp.prec >= 6) ? "(null)" : "";
                format_str(&o, &sp, s, str_len(s, sp.prec));
                break;
            }
            case 'f': case 'F':
            case 'e': case 'E':
            case 'g': case 'G': {
                union {
                    double d;
                    uint64_t u;
                } v;
                if (len == 3) v.d = (double)va_arg(ap, long double);
                else v.d = va_arg(ap, double);
                format_float(&o, &sp, *p, v.u);
                break;
            }
            case 'n':
                if (len == -2) *va_arg(ap, signed char *) = (signed char)o.len;
                else if (len == -1) *va_arg(ap, short *) = (short)o.len;
                else if (len == 1) *va_arg(ap, long *) = (long)o.len;
                else if (len >= 2) *va_arg(ap, long long *) = (long long)o.len;
                else *va_arg(ap, int *) = (int)o.len;
                break;
            case '%':
                put(&o, '%');
                break;
            case '\0':
                put_n(&o, start, (size_t)(p - start));
                p--;
                break;
            default:
                put_n(&o, start, (size_t)(p - start) + 1);
                break;
        }
    }

    if (size > 0) o.buf[o.len < size ? o.len : size - 1] = '\0';
    return (int)o.len;
}
