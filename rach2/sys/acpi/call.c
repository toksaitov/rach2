/*
 * call.c: /proc/acpi/call (sys/acpicall.h), which evaluates a method of the
 * firmware's namespace through ACPICA and keeps its result as text, as
 * Linux's acpi_call module words it.
 */

#include "../acpicall.h"

#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/spinlock.h"
#include "../lib/kstring.h"

#include "acpi.h"

#include "acpica.h"

/* The most arguments a call passes, as ACPI's methods take at most. */
#define CALL_ARGS 7

/* The deepest package within a package the result spells out. */
#define CALL_DEPTH 4

/* The result of the last call, NUL-terminated. */
static char result[ACPI_CALL_MAX] = "not called";
static struct spinlock result_lock = SPINLOCK_INIT;

/* A result being written: `len` bytes of `cap` used, the rest cut. */
struct text {
    char *s;
    int len;
    int cap;
};

/** Add the NUL-terminated `s` to `t`, as much as fits. */
static void put(struct text *t, const char *s)
{
    while (*s != '\0' && t->len < t->cap - 1) {
        t->s[t->len++] = *s++;
    }
    t->s[t->len] = '\0';
}

/** Add `v` as 0x and its hexadecimal digits, at least `digits` of them. */
static void put_hex(struct text *t, UINT64 v, int digits)
{
    static const char hex[] = "0123456789abcdef";
    char buf[19];
    int i = (int)sizeof(buf) - 1, n = 0;

    buf[i] = '\0';
    do {
        buf[--i] = hex[(unsigned)(v & 0xF)];
        v >>= 4;
        n++;
    } while (v != 0 || n < digits);
    buf[--i] = 'x';
    buf[--i] = '0';
    put(t, buf + i);
}

/**
 * Add the object `obj` as acpi_call spells it: an integer as 0x and its
 * digits, a string in double quotes, a buffer as {0x01, 0x02}, a package as
 * [A, B] of its elements, and any other as its type.
 */
static void put_object(struct text *t, const ACPI_OBJECT *obj, int depth)
{
    char one[2];
    UINT32 i;

    switch (obj->Type) {
    case ACPI_TYPE_INTEGER:
        put_hex(t, obj->Integer.Value, 1);
        break;
    case ACPI_TYPE_STRING:
        put(t, "\"");
        one[1] = '\0';
        for (i = 0; i < obj->String.Length; i++) {
            one[0] = obj->String.Pointer[i];
            put(t, one);
        }
        put(t, "\"");
        break;
    case ACPI_TYPE_BUFFER:
        put(t, "{");
        for (i = 0; i < obj->Buffer.Length; i++) {
            put(t, i == 0 ? "" : ", ");
            put_hex(t, obj->Buffer.Pointer[i], 2);
        }
        put(t, "}");
        break;
    case ACPI_TYPE_PACKAGE:
        put(t, "[");
        for (i = 0; i < obj->Package.Count; i++) {
            put(t, i == 0 ? "" : ", ");
            if (depth < CALL_DEPTH) {
                put_object(t, &obj->Package.Elements[i], depth + 1);
            } else {
                put(t, "...");
            }
        }
        put(t, "]");
        break;
    default:
        put(t, "Object type ");
        put_hex(t, obj->Type, 1);
        break;
    }
}

/** Whether `c` ends a word of the request. */
static int word_end(char c)
{
    return c == '\0' || c == ' ' || c == '\n';
}

/* A tenth of the largest 64-bit value, against which a decimal's next digit
 * is checked, since i686's kernel has no 64-bit division (no libgcc). */
#define MAX_TENTH 0x1999999999999999ull

/**
 * Read the number at `*p`, decimal or after 0x hexadecimal, up to the end of
 * its word, into `*v`, and move `*p` past it. 0, or -EINVAL for no digits,
 * a digit of neither base, or a value past 64 bits.
 */
static int parse_number(char **p, UINT64 *v)
{
    char *s = *p;
    unsigned base = 10, d;
    int n = 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    *v = 0;
    for (; !word_end(*s); s++, n++) {
        if (*s >= '0' && *s <= '9') {
            d = (unsigned)(*s - '0');
        } else if (base == 16 && *s >= 'a' && *s <= 'f') {
            d = (unsigned)(*s - 'a' + 10);
        } else if (base == 16 && *s >= 'A' && *s <= 'F') {
            d = (unsigned)(*s - 'A' + 10);
        } else {
            return -EINVAL;
        }
        if (d >= base || (base == 16 && (*v >> 60) != 0) ||
            (base == 10 && (*v > MAX_TENTH || (*v == MAX_TENTH && d > 5)))) {
            return -EINVAL;
        }
        *v = base == 16 ? (*v << 4) | d : *v * 10 + d;
    }
    *p = s;
    return n > 0 ? 0 : -EINVAL;
}

/**
 * Split the request `s`, in place, into its path, NUL-terminated at
 * `*path`, and its arguments, `*argc` of them in `argv`, whose strings
 * point into `s`. 0 or -EINVAL.
 */
static int parse(char *s, char **path, ACPI_OBJECT *argv, UINT32 *argc)
{
    char *end;
    UINT64 v;

    if (*s != '\\') {
        return -EINVAL;
    }
    *path = s;
    while (!word_end(*s)) {
        s++;
    }
    *argc = 0;
    for (;;) {
        end = s;
        while (*s == ' ') {
            s++;
        }
        if (*s == '\n') {
            s++;
        }
        *end = '\0';
        if (*s == '\0') {
            return 0;
        }
        if (s == end || *argc == CALL_ARGS) {
            return -EINVAL;
        }
        if (*s == '"') {
            argv[*argc].Type = ACPI_TYPE_STRING;
            argv[*argc].String.Pointer = ++s;
            while (*s != '"' && *s != '\0') {
                s++;
            }
            if (*s != '"') {
                return -EINVAL;
            }
            argv[*argc].String.Length = (UINT32)(s - argv[*argc].String.Pointer);
            *s++ = '\0';
            if (!word_end(*s)) {
                return -EINVAL;
            }
        } else {
            if (parse_number(&s, &v) != 0) {
                return -EINVAL;
            }
            argv[*argc].Type = ACPI_TYPE_INTEGER;
            argv[*argc].Integer.Value = v;
        }
        (*argc)++;
    }
}

/** Keep `text` as the result of the last call. */
static void keep(const char *text)
{
    uint32_t flags = spin_lock_irqsave(&result_lock);
    size_t n = k_strlen(text);

    if (n > sizeof(result) - 1) {
        n = sizeof(result) - 1;
    }
    k_memcpy(result, text, n);
    result[n] = '\0';
    spin_unlock_irqrestore(&result_lock, flags);
}

int acpi_call_write(const char *text, uint32_t len)
{
    char request[ACPI_CALL_MAX], out[ACPI_CALL_MAX];
    ACPI_OBJECT argv[CALL_ARGS];
    ACPI_OBJECT_LIST args;
    ACPI_BUFFER ret;
    ACPI_STATUS status;
    struct text t;
    char *path;
    UINT32 argc;

    if (len >= sizeof(request)) {
        return -EINVAL;
    }
    k_memcpy(request, text, len);
    request[len] = '\0';
    if (k_strlen(request) != len || parse(request, &path, argv, &argc) != 0) {
        return -EINVAL;
    }
    if (!acpica_up()) {
        return -ENODEV;
    }
    args.Count = argc;
    args.Pointer = argv;
    ret.Length = ACPI_ALLOCATE_BUFFER;
    ret.Pointer = NULL;
    status = AcpiEvaluateObject(NULL, path, &args, &ret);
    t.s = out;
    t.len = 0;
    t.cap = (int)sizeof(out);
    out[0] = '\0';
    if (ACPI_FAILURE(status)) {
        put(&t, "Error: ");
        put(&t, AcpiFormatException(status));
    } else if (ret.Pointer != NULL) {
        put_object(&t, (const ACPI_OBJECT *)ret.Pointer, 0);
    } else {
        put(&t, "0x0");
    }
    if (ret.Pointer != NULL) {
        AcpiOsFree(ret.Pointer);
    }
    keep(out);
    klog(KLOG_INFO, "acpi", "call: %s: %s", path, out);
    return 0;
}

int acpi_call_result(char *out, int cap)
{
    uint32_t flags;
    int n;

    if (cap <= 0) {
        return 0;
    }
    flags = spin_lock_irqsave(&result_lock);
    n = (int)k_strlen(result);
    n = n < cap - 1 ? n : cap - 1;
    k_memcpy(out, result, (size_t)n);
    out[n] = '\0';
    spin_unlock_irqrestore(&result_lock, flags);
    return n;
}
