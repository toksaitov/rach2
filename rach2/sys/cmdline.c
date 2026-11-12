/*
 * cmdline.c: the kernel command line's buffer and its lookups, as cmdline.h
 * describes them.
 */
#include "cmdline.h"
#include "boot.h"
#include "lib/kstring.h"

/* The buffer's bytes, the NUL included: one fewer than the boot record's
 * (boot.h), so that a line the record cut at its own limit, perhaps inside a
 * token, is too long here and is cut again at a delimiter. */
#define KCMDLINE_MAX (BOOT_CMDLINE_MAX - 1)

static char kcmdline[KCMDLINE_MAX];

int kcmdline_capture(const char *cmdline)
{
    size_t i;

    if (cmdline == 0) {
        kcmdline[0] = '\0';
        return 0;
    }
    for (i = 0; i + 1 < KCMDLINE_MAX && cmdline[i] != '\0'; i++) {
        kcmdline[i] = cmdline[i];
    }
    kcmdline[i] = '\0';

    if (cmdline[i] != '\0') {
        while (i > 0 && kcmdline[i - 1] != ' ' && kcmdline[i - 1] != '\t') {
            i--;
        }
        kcmdline[i] = '\0';
        return 1;
    }
    return 0;
}

const char *kcmdline_get(void)
{
    return kcmdline;
}

/**
 * Advance `*p` past the next token, delimited by spaces and tabs, and set
 * `*start` and `*len` to where it begins and how long it is; 0 when the line
 * has no more. `kcmdline_value_nth` and `kcmdline_count` scan with it.
 */
static int next_token(const char **p, const char **start, size_t *len)
{
    while (**p == ' ' || **p == '\t') {
        (*p)++;
    }
    if (**p == '\0') {
        return 0;
    }
    *start = *p;
    while (**p != '\0' && **p != ' ' && **p != '\t') {
        (*p)++;
    }
    *len = (size_t)(*p - *start);
    return 1;
}

/** Whether the first `klen` bytes of `tok` are `key`'s. */
static int token_is(const char *tok, const char *key, size_t klen)
{
    size_t i;

    for (i = 0; i < klen && tok[i] == key[i]; i++) {
        /* match each byte of the token against the key */
    }
    return i == klen;
}

/** Whether `tok` of length `len` is `key` followed by `=`. */
static int token_has_key(const char *tok, size_t len, const char *key, size_t klen)
{
    return len >= klen + 1 && tok[klen] == '=' && token_is(tok, key, klen);
}

int kcmdline_value(const char *key, char *out, unsigned int outlen)
{
    return kcmdline_value_nth(key, 0, out, outlen);
}

/** The index of the first of the `nwords` `words` that the item at `s`,
 * which ends before `end`, begins with, as strncmp(s, word, strlen(word))
 * matches it; -1 for none. */
static int list_word(const char *s, const char *end, const char *const *words, int nwords)
{
    int w;

    for (w = 0; w < nwords; w++) {
        size_t i = 0, n = k_strlen(words[w]);

        while (i < n && s + i < end && s[i] == words[w][i]) {
            i++;
        }
        if (i == n) {
            return w;
        }
    }
    return -1;
}

/** Linux's parse_args hands an early parameter's setup function each token
 * whose name is the parameter's, with a value or none (init/main.c, lines
 * 723 to 739), so `nointremap` and `nointremap=1` both run
 * setup_nointremap. A list's items are taken as setup_irqremap and
 * intel_iommu_setup take theirs (drivers/iommu/irq_remapping.c, lines 62
 * to 80; drivers/iommu/intel/iommu.c, lines 245 to 281): a word is matched
 * by strncmp over its own length, so `onx` is `on`, and the item ends at
 * the next comma, a run of commas being one. */
int kcmdline_list(const char *key, const char *flag, const char *const *words, int nwords,
                  int *last)
{
    size_t klen = k_strlen(key), flen = flag != 0 ? k_strlen(flag) : 0;
    const char *p = kcmdline;
    const char *tok, *s, *end;
    size_t len;
    int seen = 0, w;

    while (next_token(&p, &tok, &len)) {
        if (flag != 0 && len >= flen && token_is(tok, flag, flen) &&
            (len == flen || tok[flen] == '=')) {
            seen |= 1;
            if (last != 0) {
                *last = 0;
            }
            continue;
        }
        if (!token_has_key(tok, len, key, klen)) {
            continue;
        }
        end = tok + len;
        for (s = tok + klen + 1; s < end;) {
            w = list_word(s, end, words, nwords);
            if (w >= 0) {
                seen |= 1 << w;
                if (last != 0) {
                    *last = w;
                }
            }
            while (s < end && *s != ',') {
                s++;
            }
            while (s < end && *s == ',') {
                s++;
            }
        }
    }
    return seen;
}

int kcmdline_value_nth(const char *key, int nth, char *out, unsigned int outlen)
{
    size_t klen = k_strlen(key);
    const char *p = kcmdline;
    const char *tok;
    size_t len;
    int seen = 0;

    if (outlen == 0) {
        return 0;
    }
    out[0] = '\0';
    while (next_token(&p, &tok, &len)) {
        if (token_has_key(tok, len, key, klen)) {
            size_t vlen = len - klen - 1;
            size_t i;

            if (seen++ != nth) {
                continue;
            }

            if (vlen + 1 > (size_t)outlen) {
                return 0;
            }
            for (i = 0; i < vlen; i++) {
                out[i] = tok[klen + 1 + i];
            }
            out[vlen] = '\0';
            return 1;
        }
    }
    return 0;
}

/** The value of the digit `c` in bases up to 16, or 16 for none. */
static unsigned int digit_value(char c)
{
    if (c >= '0' && c <= '9') {
        return (unsigned int)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (unsigned int)(c - 'a') + 10u;
    }
    if (c >= 'A' && c <= 'F') {
        return (unsigned int)(c - 'A') + 10u;
    }
    return 16u;
}

/**
 * The value of the `nth` `key=value` token, counting from zero, as a pointer
 * into the line, so that a value of any length is read in place; NULL when
 * the line has no such token. The value ends at the token's end, a space, a
 * tab or the NUL.
 */
static const char *value_in_place(const char *key, int nth)
{
    size_t klen = k_strlen(key);
    const char *p = kcmdline;
    const char *tok;
    size_t len;
    int seen = 0;

    while (next_token(&p, &tok, &len)) {
        if (token_has_key(tok, len, key, klen) && seen++ == nth) {
            return tok + klen + 1;
        }
    }
    return 0;
}

/** The base is chosen as Linux's _parse_integer_fixup_radix chooses it
 * (lib/kstrtox.c, lines 26 to 40), and the digits wrap as its unsigned
 * long long does, whose low 32 bits are what the int keeps. A space or a
 * tab is no digit, so the digits stop at the token's end. */
int kcmdline_option_nth(const char *key, int nth, int *out)
{
    const char *s = value_in_place(key, nth);
    unsigned int base = 10u, value = 0u, d;
    int neg = 0;

    if (s == 0) {
        return -1;
    }
    if (*s == '\0' || *s == ' ' || *s == '\t') {
        return 0;
    }
    if (*s == '-') {
        neg = 1;
        s++;
    }
    if (s[0] == '0') {
        base = 8u;
        if ((s[1] == 'x' || s[1] == 'X') && digit_value(s[2]) < 16u) {
            base = 16u;
            s += 2;
        }
    }
    for (; (d = digit_value(*s)) < base; s++) {
        value = value * base + d;
    }
    *out = (int)(neg ? 0u - value : value);
    return 1;
}

int kcmdline_option(const char *key, int *out)
{
    int nth, r, stored = 0;

    for (nth = 0; (r = kcmdline_option_nth(key, nth, out)) >= 0; nth++) {
        stored |= r;
    }
    return stored;
}

/**
 * Set `*out` from the first two bytes of a boolean value, `c0` and `c1`, the
 * second 0 when the value is shorter, as Linux's kstrtobool reads them
 * (Linux 6.12.115, lib/kstrtox.c:348-388). Returns 0, or -1 for no boolean.
 */
static int bool_text(char c0, char c1, int *out)
{
    switch (c0) {
    case 'y':
    case 'Y':
    case 't':
    case 'T':
    case '1':
        *out = 1;
        return 0;
    case 'n':
    case 'N':
    case 'f':
    case 'F':
    case '0':
        *out = 0;
        return 0;
    case 'o':
    case 'O':
        if (c1 == 'n' || c1 == 'N') {
            *out = 1;
            return 0;
        }
        if (c1 == 'f' || c1 == 'F') {
            *out = 0;
            return 0;
        }
        return -1;
    default:
        return -1;
    }
}

int kcmdline_parse_bool(const char *text, int *out)
{
    return bool_text(text[0], text[0] != '\0' ? text[1] : '\0', out);
}

/**
 * The boolean option `key`, as `kcmdline_bool` reads it; a bare `key` token
 * is true when `bare` is nonzero, and is ignored otherwise.
 */
static int cmdline_bool(const char *key, int dflt, int bare)
{
    size_t klen = k_strlen(key);
    const char *p = kcmdline;
    const char *tok;
    size_t len;
    int value = dflt;

    while (next_token(&p, &tok, &len)) {
        if (bare && len == klen && token_is(tok, key, klen)) {
            value = 1;
            continue;
        }
        if (!token_has_key(tok, len, key, klen)) {
            continue;
        }
        (void)bool_text(len > klen + 1 ? tok[klen + 1] : '\0',
                        len > klen + 2 ? tok[klen + 2] : '\0', &value);
    }
    return value;
}

int kcmdline_bool(const char *key, int dflt)
{
    return cmdline_bool(key, dflt, 0);
}

int kcmdline_param_bool(const char *key, int dflt)
{
    return cmdline_bool(key, dflt, 1);
}

int kcmdline_param_nth(const char *key, int nth, char *out, unsigned int outlen)
{
    size_t klen = k_strlen(key);
    const char *p = kcmdline;
    const char *tok;
    size_t len, vlen, i;
    int seen = 0;

    if (outlen > 0) {
        out[0] = '\0';
    }
    while (next_token(&p, &tok, &len)) {
        if (len == klen && token_is(tok, key, klen)) {
            if (seen++ == nth) {
                return 0;
            }
        } else if (token_has_key(tok, len, key, klen) && seen++ == nth) {
            vlen = len - klen - 1;
            if (vlen + 1 <= (size_t)outlen) {
                for (i = 0; i < vlen; i++) {
                    out[i] = tok[klen + 1 + i];
                }
                out[vlen] = '\0';
            }
            return 1;
        }
    }
    return -1;
}

int kcmdline_count(const char *key)
{
    size_t klen = k_strlen(key);
    const char *p = kcmdline;
    const char *tok;
    size_t len;
    int n = 0;

    while (next_token(&p, &tok, &len)) {
        if (token_has_key(tok, len, key, klen)) {
            n++;
        }
    }
    return n;
}

int kcmdline_bare_count(const char *flag)
{
    size_t flen = k_strlen(flag);
    const char *p = kcmdline;
    const char *tok;
    size_t len;
    int n = 0;

    while (next_token(&p, &tok, &len)) {
        if (len == flen && token_is(tok, flag, flen)) {
            n++;
        }
    }
    return n;
}

int kcmdline_has(const char *flag)
{
    return kcmdline_bare_count(flag) > 0;
}
