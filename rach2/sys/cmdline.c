#include "cmdline.h"
#include "lib/kstring.h"

/* Long enough for any realistic GRUB or QEMU kernel command line; a longer one
 * is truncated, which at worst drops a trailing flag. */
#define KCMDLINE_MAX 256

static char kcmdline[KCMDLINE_MAX];

void kcmdline_capture(const char *cmdline)
{
    size_t i;

    if (cmdline == 0) {
        kcmdline[0] = '\0';
        return;
    }
    for (i = 0; i + 1 < KCMDLINE_MAX && cmdline[i] != '\0'; i++) {
        kcmdline[i] = cmdline[i];
    }
    kcmdline[i] = '\0';

    /* If the source was longer than the buffer, the final token may have been
       cut mid-word, and a partial token must not spuriously match a flag (a
       truncated `rach2.disk=ahcix` would otherwise match `rach2.disk=ahci`).
       Drop the torn trailing token by terminating at the last delimiter. */
    if (cmdline[i] != '\0') {
        while (i > 0 && kcmdline[i - 1] != ' ' && kcmdline[i - 1] != '\t') {
            i--;
        }
        kcmdline[i] = '\0';
    }
}

const char *kcmdline_get(void)
{
    return kcmdline;
}

/**
 * Walk the tokens, calling nothing: this is the shared scan the two lookups
 * below use so the tokenizing rule lives in one place.
 *
 * On each call it advances `*p` past one token and reports where that token
 * began and how long it was. Returns 0 when the line is exhausted.
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
    size_t klen = k_strlen(key);
    const char *p = kcmdline;
    const char *tok;
    size_t len;

    if (outlen == 0) {
        return 0;
    }
    out[0] = '\0';
    while (next_token(&p, &tok, &len)) {
        if (token_has_key(tok, len, key, klen)) {
            size_t vlen = len - klen - 1;
            size_t i;

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
