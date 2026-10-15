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

/** Whether `tok` of length `len` is `key` followed by `=`. */
static int token_has_key(const char *tok, size_t len, const char *key, size_t klen)
{
    size_t i;

    if (len < klen + 1 || tok[klen] != '=') {
        return 0;
    }
    for (i = 0; i < klen && tok[i] == key[i]; i++) {
        /* match each byte of the token against the key */
    }
    return i == klen;
}

int kcmdline_value(const char *key, char *out, unsigned int outlen)
{
    return kcmdline_value_nth(key, 0, out, outlen);
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

int kcmdline_has(const char *flag)
{
    size_t flen = k_strlen(flag);
    const char *p = kcmdline;

    while (*p != '\0') {
        const char *start;
        size_t n, i;

        if (*p == ' ' || *p == '\t') {
            p++;
            continue;
        }
        start = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            p++;
        }
        n = (size_t)(p - start);
        if (n == flen) {
            for (i = 0; i < flen && start[i] == flag[i]; i++) {
                /* match each byte of the token against the flag */
            }
            if (i == flen) {
                return 1;
            }
        }
    }
    return 0;
}
