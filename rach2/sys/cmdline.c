/*
 * cmdline.c: the kernel command line's buffer and its lookups, as cmdline.h
 * describes them.
 */
#include "cmdline.h"
#include "boot.h"
#include "lib/kstd.h"
#include "lib/kstring.h"

/* The buffer's bytes, the NUL included: one fewer than the boot record's
 * (boot.h), so that a line the record cut at its own limit, perhaps inside a
 * word, is too long here and is cut again before that word. */
#define KCMDLINE_MAX (BOOT_CMDLINE_MAX - 1)

static char kcmdline[KCMDLINE_MAX];

const char *kcmdline_get(void)
{
    return kcmdline;
}

/** Whether `c` is a space as Linux's isspace takes it (include/linux/ctype.h,
 * line 33; lib/ctype.c, lines 14, 17 and 31): a tab, a line feed, a vertical
 * tab, a form feed, a carriage return, a space or Latin-1's no-break space. */
static int is_space(char c)
{
    unsigned char u = (unsigned char)c;

    return u == ' ' || (u >= '\t' && u <= '\r') || u == 0xa0;
}

/** One word of the line, as Linux's next_arg splits it. */
struct arg {
    const char *word; /* where the word begins, its opening quote included */
    const char *name; /* the parameter's name, without its quote */
    size_t nlen;
    /* after the first `=`, its quotes left out; NULL for none */
    const char *val;
    size_t vlen;
    int whole; /* 1 when a space ends the word, 0 when the line's end does */
};

/**
 * Split the next word from `*p` as Linux's next_arg does (lib/cmdline.c,
 * lines 227 to 274), and advance `*p` past it; 0 when the line has no more.
 *
 * Spaces inside double quotes do not end a word. A word that begins with a
 * quote loses it, and loses its last byte when that is a quote; a value that
 * begins with a quote loses it, and loses the word's last byte when that is
 * a quote. next_arg drops these bytes by writing NULs, so the name and the
 * value are the bytes before the first NUL it would write. A `=` at the
 * word's first byte, the quote dropped, gives no value, as next_arg's
 * `equals` of 0 means none.
 */
static int split_word(const char **p, struct arg *a)
{
    const char *s = *p;
    size_t i, equals = 0, cut, off;
    int in_quote = 0, quoted = 0;

    while (is_space(*s)) {
        s++;
    }
    if (*s == '\0') {
        *p = s;
        return 0;
    }
    a->word = s;
    if (*s == '"') {
        s++;
        in_quote = 1;
        quoted = 1;
    }
    for (i = 0; s[i] != '\0'; i++) {
        if (is_space(s[i]) && !in_quote) {
            break;
        }
        if (equals == 0 && s[i] == '=') {
            equals = i;
        }
        if (s[i] == '"') {
            in_quote = !in_quote;
        }
    }
    cut = i;
    if (i > 0 && s[i - 1] == '"' && (quoted || (equals != 0 && s[equals + 1] == '"'))) {
        cut = i - 1;
    }
    a->name = s;
    if (equals == 0) {
        a->nlen = cut;
        a->val = 0;
        a->vlen = 0;
    } else {
        off = equals + 1 + (s[equals + 1] == '"' ? 1 : 0);
        a->nlen = equals;
        a->val = s + off;
        a->vlen = cut > off ? cut - off : 0;
    }
    a->whole = s[i] != '\0';
    *p = s + i;
    return 1;
}

/**
 * Split the next word as `split_word` does, and stop at a bare `--`, as
 * Linux's parse_args stops there (kernel/params.c, lines 181 to 184): the
 * words after it are init's, and no lookup here reads them. Returns 0 at
 * the line's end or at the `--`, and leaves `*p` at the `--`, so that
 * another call stops there again.
 */
static int next_arg(const char **p, struct arg *a)
{
    const char *s = *p;

    if (!split_word(p, a)) {
        return 0;
    }
    if (a->val == 0 && a->nlen == 2 && a->name[0] == '-' && a->name[1] == '-') {
        *p = s;
        return 0;
    }
    return 1;
}

/** A byte of a parameter's name as Linux's dash2underscore gives it for a
 * comparison (kernel/params.c, lines 79 to 84): `-` is `_`. */
static char name_byte(char c)
{
    return c == '-' ? '_' : c;
}

/** Whether the word `a` names the parameter `key`, of length `klen`, as
 * Linux's parameq compares a name (kernel/params.c, lines 86 to 100), `-`
 * and `_` alike, so that `intel-iommu` is `intel_iommu`. */
static int arg_is(const struct arg *a, const char *key, size_t klen)
{
    size_t i;

    if (a->nlen != klen) {
        return 0;
    }
    for (i = 0; i < klen && name_byte(a->name[i]) == name_byte(key[i]); i++) {
        /* match each byte of the name against the key */
    }
    return i == klen;
}

/**
 * Whether the word `a` begins with the `n` bytes of `str`, as Linux's
 * obsolete_checksetup matches a `__setup` string (init/main.c, lines 195
 * to 222): the word is read whole, its name, then a `=` and its value when
 * it has one, as repair_env_string puts it back together (lines 462 to
 * 470), and its bytes are compared by parameqn, `-` and `_` alike.
 */
static int arg_starts(const struct arg *a, const char *str, size_t n)
{
    size_t i;
    char c;

    for (i = 0; i < n; i++) {
        if (i < a->nlen) {
            c = a->name[i];
        } else if (a->val == 0 || i - a->nlen > a->vlen) {
            return 0;
        } else if (i == a->nlen) {
            c = '=';
        } else {
            c = a->val[i - a->nlen - 1];
        }
        if (name_byte(c) != name_byte(str[i])) {
            return 0;
        }
    }
    return 1;
}

/** A line that goes on past the buffer is cut before the word that the
 * buffer's end ends, which `split_word` finds as the lookups do, a quoted
 * value's spaces included. */
int kcmdline_capture(const char *cmdline)
{
    const char *p = kcmdline;
    struct arg a;
    size_t i;

    if (cmdline == 0) {
        kcmdline[0] = '\0';
        return 0;
    }
    for (i = 0; i + 1 < KCMDLINE_MAX && cmdline[i] != '\0'; i++) {
        kcmdline[i] = cmdline[i];
    }
    kcmdline[i] = '\0';
    if (cmdline[i] == '\0') {
        return 0;
    }

    while (split_word(&p, &a)) {
        if (!a.whole) {
            kcmdline[a.word - kcmdline] = '\0';
            break;
        }
    }
    return 1;
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
    const char *s, *end;
    struct arg a;
    int seen = 0, w;

    while (next_arg(&p, &a)) {
        if (flag != 0 && arg_is(&a, flag, flen)) {
            seen |= 1;
            if (last != 0) {
                *last = 0;
            }
            continue;
        }
        if (a.val == 0 || !arg_is(&a, key, klen)) {
            continue;
        }
        end = a.val + a.vlen;
        for (s = a.val; s < end;) {
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
    struct arg a;
    size_t i;
    int seen = 0;

    if (outlen == 0) {
        return 0;
    }
    out[0] = '\0';
    while (next_arg(&p, &a)) {
        if (a.val == 0 || !arg_is(&a, key, klen) || seen++ != nth) {
            continue;
        }
        if (a.vlen + 1 > (size_t)outlen) {
            return 0;
        }
        for (i = 0; i < a.vlen; i++) {
            out[i] = a.val[i];
        }
        out[a.vlen] = '\0';
        return 1;
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
 * Find the `nth` `key=value` word, counting from zero, and set `*a` to it,
 * so that a value of any length is read in place, without the quotes
 * `split_word` leaves out. Returns 0 when the line has no such word.
 */
static int find_value(const char *key, int nth, struct arg *a)
{
    size_t klen = k_strlen(key);
    const char *p = kcmdline;
    int seen = 0;

    while (next_arg(&p, a)) {
        if (a->val != 0 && arg_is(a, key, klen) && seen++ == nth) {
            return 1;
        }
    }
    return 0;
}

/** The byte `i` of the value `a`, or NUL past its end, where next_arg ends
 * the value with a NUL. */
static char val_at(const struct arg *a, size_t i)
{
    return i < a->vlen ? a->val[i] : '\0';
}

/** The base is chosen as Linux's _parse_integer_fixup_radix chooses it
 * (lib/kstrtox.c, lines 26 to 40), and the digits wrap as its unsigned
 * long long does, whose low 32 bits are what the int keeps. The digits
 * stop at the value's end, where next_arg puts a NUL. */
int kcmdline_option_nth(const char *key, int nth, int *out)
{
    struct arg a;
    unsigned int base = 10u, value = 0u, d;
    size_t i = 0;
    int neg = 0;

    if (!find_value(key, nth, &a)) {
        return -1;
    }
    if (a.vlen == 0) {
        return 0;
    }
    if (val_at(&a, 0) == '-') {
        neg = 1;
        i++;
    }
    if (val_at(&a, i) == '0') {
        base = 8u;
        if ((val_at(&a, i + 1) == 'x' || val_at(&a, i + 1) == 'X') &&
            digit_value(val_at(&a, i + 2)) < 16u) {
            base = 16u;
            i += 2;
        }
    }
    for (; (d = digit_value(val_at(&a, i))) < base; i++) {
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
    struct arg a;
    int value = dflt;

    while (next_arg(&p, &a)) {
        if (!arg_is(&a, key, klen)) {
            continue;
        }
        if (a.val == 0) {
            if (bare) {
                value = 1;
            }
            continue;
        }
        (void)bool_text(val_at(&a, 0), val_at(&a, 1), &value);
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

/** The value is read in place by k_strtoll, Linux's kstrtoll, whose range
 * kstrtoint then narrows to an int (lib/kstrtox.c:156-177 and 259-271). */
int kcmdline_param_int(const char *key, int dflt)
{
    size_t klen = k_strlen(key);
    const char *p = kcmdline;
    struct arg a;
    long long v;
    int value = dflt;

    while (next_arg(&p, &a)) {
        if (a.val != 0 && arg_is(&a, key, klen) && k_strtoll(a.val, a.vlen, &v) == 0 &&
            v >= -0x7FFFFFFFLL - 1 && v <= 0x7FFFFFFFLL) {
            value = (int)v;
        }
    }
    return value;
}

int kcmdline_param_nth(const char *key, int nth, char *out, unsigned int outlen)
{
    size_t klen = k_strlen(key);
    const char *p = kcmdline;
    struct arg a;
    size_t i;
    int seen = 0;

    if (outlen > 0) {
        out[0] = '\0';
    }
    while (next_arg(&p, &a)) {
        if (!arg_is(&a, key, klen) || seen++ != nth) {
            continue;
        }
        if (a.val == 0) {
            return 0;
        }
        if (a.vlen + 1 <= (size_t)outlen) {
            for (i = 0; i < a.vlen; i++) {
                out[i] = a.val[i];
            }
            out[a.vlen] = '\0';
        }
        return 1;
    }
    return -1;
}

int kcmdline_count(const char *key)
{
    size_t klen = k_strlen(key);
    const char *p = kcmdline;
    struct arg a;
    int n = 0;

    while (next_arg(&p, &a)) {
        if (a.val != 0 && arg_is(&a, key, klen)) {
            n++;
        }
    }
    return n;
}

int kcmdline_bare_count(const char *flag)
{
    size_t flen = k_strlen(flag);
    const char *p = kcmdline;
    struct arg a;
    int n = 0;

    while (next_arg(&p, &a)) {
        if (a.val == 0 && arg_is(&a, flag, flen)) {
            n++;
        }
    }
    return n;
}

/** A `flag` with a `=` is a name and the value it must have, compared byte
 * for byte, as Linux's oops_setup compares `panic` (kernel/panic.c, lines
 * 909 to 917). */
int kcmdline_has(const char *flag)
{
    size_t flen = k_strlen(flag), klen, i;
    const char *p = kcmdline;
    struct arg a;

    for (klen = 0; klen < flen && flag[klen] != '='; klen++) {
        /* find the end of the name */
    }
    if (klen == flen) {
        return kcmdline_bare_count(flag) > 0;
    }
    while (next_arg(&p, &a)) {
        if (a.val == 0 || !arg_is(&a, flag, klen) || a.vlen != flen - klen - 1) {
            continue;
        }
        for (i = 0; i < a.vlen && a.val[i] == flag[klen + 1 + i]; i++) {
            /* match each byte of the value */
        }
        if (i == a.vlen) {
            return 1;
        }
    }
    return 0;
}

/** Linux's do_early_param runs an early parameter's setup function for
 * each word that names it, with a value or none (init/main.c, lines 723 to
 * 739), and the setup functions of the flags read here, as parse_noapic,
 * ignore the value. */
int kcmdline_early(const char *name)
{
    size_t nlen = k_strlen(name);
    const char *p = kcmdline;
    struct arg a;

    while (next_arg(&p, &a)) {
        if (arg_is(&a, name, nlen)) {
            return 1;
        }
    }
    return 0;
}

int kcmdline_setup(const char *str)
{
    size_t n = k_strlen(str);
    const char *p = kcmdline;
    struct arg a;

    while (next_arg(&p, &a)) {
        if (arg_starts(&a, str, n)) {
            return 1;
        }
    }
    return 0;
}
