/* json.c: bounded JSON parsing and object-subset comparison for API bodies. */
#include "json.h"
#ifdef _WIN32
#include "../../../../tools/etude/win/files.h"
#endif
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *allocate(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) {
        fputs("fakeapi: out of memory\n", stderr);
        exit(2);
    }
    return p;
}
void append(struct buffer *b, const void *p, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        char *q;
        while (cap < b->len + n + 1)
            cap *= 2;
        q = realloc(b->data, cap);
        if (!q) {
            fputs("fakeapi: out of memory\n", stderr);
            exit(2);
        }
        b->data = q;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = 0;
}
void putsb(struct buffer *b, const char *s)
{
    append(b, s, strlen(s));
}
void clear(struct buffer *b)
{
    free(b->data);
    memset(b, 0, sizeof(*b));
}
struct value *value(char type)
{
    struct value *v = allocate(sizeof(*v));
    v->type = type;
    return v;
}
struct value *string(const char *s)
{
    struct value *v = value('s');
    v->text = strdup(s ? s : "");
    return v;
}
struct value *integer(long long n)
{
    struct value *v = value('n');
    v->number = (double)n;
    return v;
}
struct value *json_boolean(int n)
{
    return value(n ? 't' : 'f');
}
void destroy(struct value *v)
{
    while (v) {
        struct value *next = v->next;
        destroy(v->child);
        free(v->key);
        free(v->text);
        free(v);
        v = next;
    }
}
void add(struct value *v, const char *key, struct value *item)
{
    struct value **p;
    if (!v || !item)
        return;
    p = &v->child;
    while (*p)
        p = &(*p)->next;
    item->key = key ? strdup(key) : NULL;
    *p = item;
}
void set(struct value *v, const char *key, struct value *item)
{
    struct value **p = &v->child;
    while (*p) {
        if ((*p)->key && !strcmp((*p)->key, key)) {
            struct value *old = *p;
            *p = old->next;
            old->next = NULL;
            destroy(old);
            break;
        }
        p = &(*p)->next;
    }
    add(v, key, item);
}
const struct value *get(const struct value *v, const char *key)
{
    const struct value *p;
    for (p = v && v->type == 'o' ? v->child : NULL; p; p = p->next)
        if (!strcmp(p->key, key))
            return p;
    return NULL;
}
const char *text(const struct value *v)
{
    return v && v->type == 's' ? v->text : "";
}
const char *field(const struct value *v, const char *key)
{
    return text(get(v, key));
}
int flag(const struct value *v, const char *key, int fallback)
{
    const struct value *p = get(v, key);
    return p ? p->type == 't' : fallback;
}
long long number(const struct value *v, const char *key, long long fallback)
{
    const struct value *p = get(v, key);
    return p && p->type == 'n' ? (long long)p->number : fallback;
}
size_t count(const struct value *v)
{
    const struct value *p;
    size_t n = 0;
    for (p = v ? v->child : NULL; p; p = p->next)
        n++;
    return n;
}
struct value *clone(const struct value *v)
{
    struct value *r;
    const struct value *p;
    if (!v)
        return value('z');
    r = value(v->type);
    r->number = v->number;
    if (v->text)
        r->text = strdup(v->text);
    for (p = v->child; p; p = p->next)
        add(r, p->key, clone(p));
    return r;
}
struct reader {
    const unsigned char *p;
    unsigned nodes;
};
static void space(struct reader *r)
{
    while (*r->p && strchr(" \t\r\n", *r->p))
        r->p++;
}
static int hex4(struct reader *r, unsigned *n)
{
    unsigned i;
    *n = 0;
    for (i = 0; i < 4; i++) {
        unsigned c = *r->p;
        if (!isxdigit(c))
            return 0;
        r->p++;
        *n = *n * 16 + (c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
    }
    return 1;
}
static char *read_string(struct reader *r)
{
    struct buffer b = {0};
    if (*r->p++ != '"')
        return NULL;
    while (*r->p && *r->p != '"') {
        unsigned c = *r->p++;
        if (c < 32)
            goto bad;
        if (c == '\\') {
            const char *p;
            c = *r->p;
            if (!c)
                goto bad;
            r->p++;
            if (c == 'u') {
                unsigned low;
                unsigned char utf[4];
                size_t n;
                if (!hex4(r, &c) || c == 0)
                    goto bad;
                if (c >= 0xd800 && c <= 0xdbff) {
                    if (r->p[0] != '\\' || r->p[1] != 'u')
                        goto bad;
                    r->p += 2;
                    if (!hex4(r, &low) || low < 0xdc00 || low > 0xdfff)
                        goto bad;
                    c = 0x10000 + ((c - 0xd800) << 10) + low - 0xdc00;
                } else if (c >= 0xdc00 && c <= 0xdfff)
                    goto bad;
                if (c < 128) {
                    utf[0] = (unsigned char)c;
                    n = 1;
                } else if (c < 2048) {
                    utf[0] = 0xc0 | (c >> 6);
                    utf[1] = 0x80 | (c & 63);
                    n = 2;
                } else if (c < 65536) {
                    utf[0] = 0xe0 | (c >> 12);
                    utf[1] = 0x80 | ((c >> 6) & 63);
                    utf[2] = 0x80 | (c & 63);
                    n = 3;
                } else {
                    utf[0] = 0xf0 | (c >> 18);
                    utf[1] = 0x80 | ((c >> 12) & 63);
                    utf[2] = 0x80 | ((c >> 6) & 63);
                    utf[3] = 0x80 | (c & 63);
                    n = 4;
                }
                append(&b, utf, n);
                continue;
            }
            p = strchr("\"\\/bfnrt", c);
            if (!p)
                goto bad;
            c = (unsigned char)"\"\\/\b\f\n\r\t"[p - "\"\\/bfnrt"];
        }
        append(&b, &c, 1);
    }
    if (*r->p != '"')
        goto bad;
    r->p++;
    if (!b.data)
        putsb(&b, "");
    return b.data;
bad:
    clear(&b);
    return NULL;
}
static struct value *read_value(struct reader *r, unsigned depth);
static struct value *read_container(struct reader *r, unsigned depth)
{
    struct value *v = NULL;
    unsigned char close = *r->p++ == '{' ? '}' : ']';
    v = value(close == '}' ? 'o' : 'a');
    space(r);
    if (*r->p == close) {
        r->p++;
        return v;
    }
    for (;;) {
        char *key = NULL;
        struct value *child;
        space(r);
        if (close == '}') {
            if (*r->p != '"' || !(key = read_string(r)))
                goto bad;
            space(r);
            if (*r->p != ':' || get(v, key)) {
                free(key);
                goto bad;
            }
            r->p++;
        }
        child = read_value(r, depth + 1);
        if (!child) {
            free(key);
            goto bad;
        }
        add(v, key, child);
        free(key);
        space(r);
        if (*r->p == close) {
            r->p++;
            return v;
        }
        if (*r->p != ',')
            goto bad;
        r->p++;
    }
bad:
    destroy(v);
    return NULL;
}
static struct value *read_number(struct reader *r)
{
    struct value *v = NULL;
    char *end;

    const unsigned char *start = r->p;
    if (*r->p == '-')
        r->p++;
    if (*r->p == '0')
        r->p++;
    else {
        if (*r->p < '1' || *r->p > '9')
            goto bad;
        while (isdigit(*r->p))
            r->p++;
    }
    if (*r->p == '.') {
        r->p++;
        if (!isdigit(*r->p))
            goto bad;
        while (isdigit(*r->p))
            r->p++;
    }
    if (*r->p == 'e' || *r->p == 'E') {
        r->p++;
        if (*r->p == '+' || *r->p == '-')
            r->p++;
        if (!isdigit(*r->p))
            goto bad;
        while (isdigit(*r->p))
            r->p++;
    }
    v = value('n');
    v->number = strtod((const char *)start, &end);
    if ((const unsigned char *)end != r->p || !isfinite(v->number))
        goto bad;
    return v;
bad:
    destroy(v);
    return NULL;
}
static struct value *read_value(struct reader *r, unsigned depth)
{
    struct value *v = NULL;
    space(r);
    if (depth > 128 || ++r->nodes > 1048576)
        return NULL;
    if (*r->p == '"') {
        v = value('s');
        if (!(v->text = read_string(r)))
            goto bad;
        return v;
    }
    if (*r->p == '{' || *r->p == '[')
        return read_container(r, depth);
    if (!strncmp((const char *)r->p, "true", 4)) {
        r->p += 4;
        return value('t');
    }
    if (!strncmp((const char *)r->p, "false", 5)) {
        r->p += 5;
        return value('f');
    }
    if (!strncmp((const char *)r->p, "null", 4)) {
        r->p += 4;
        return value('z');
    }
    if (*r->p == '-' || isdigit(*r->p))
        return read_number(r);

bad:
    destroy(v);
    return NULL;
}
struct value *parse(const char *s)
{
    struct reader r;
    struct value *v;
    r.p = (const unsigned char *)s;
    r.nodes = 0;
    v = read_value(&r, 0);
    space(&r);
    if (*r.p) {
        destroy(v);
        return NULL;
    }
    return v;
}
struct value *read_json(const char *path)
{
    FILE *f = fopen(path, "rb");
    struct buffer b = {0};
    struct value *v;
    char block[8192];
    size_t n;
    if (!f)
        return NULL;
    while ((n = fread(block, 1, sizeof(block), f)) != 0) {
        if (b.len + n > 64U * 1024U * 1024U) {
            fclose(f);
            clear(&b);
            return NULL;
        }
        append(&b, block, n);
    }
    if (ferror(f)) {
        fclose(f);
        clear(&b);
        return NULL;
    }
    fclose(f);
    v = parse(b.data ? b.data : "");
    clear(&b);
    return v;
}
static void quoted(struct buffer *b, const char *s)
{
    putsb(b, "\"");
    while (*s) {
        unsigned char c = (unsigned char)*s++;
        char escaped[7];
        if (c == '"' || c == '\\') {
            putsb(b, "\\");
            append(b, &c, 1);
        } else if (c < 32) {
            snprintf(escaped, sizeof(escaped), "\\u%04x", c);
            putsb(b, escaped);
        } else
            append(b, &c, 1);
    }
    putsb(b, "\"");
}
static void encoded(struct buffer *b, const struct value *v, int compact)
{
    const struct value *p;
    char n[64];
    if (!v) {
        putsb(b, "null");
        return;
    }
    switch (v->type) {
    case 's':
        quoted(b, v->text);
        break;
    case 'n':
        snprintf(n, sizeof(n), "%.17g", v->number);
        putsb(b, n);
        break;
    case 't':
        putsb(b, "true");
        break;
    case 'f':
        putsb(b, "false");
        break;
    case 'a':
    case 'o':
        putsb(b, v->type == 'a' ? "[" : "{");
        for (p = v->child; p; p = p->next) {
            if (p != v->child)
                putsb(b, compact ? "," : ", ");
            if (v->type == 'o') {
                quoted(b, p->key);
                putsb(b, compact ? ":" : ": ");
            }
            encoded(b, p, compact);
        }
        putsb(b, v->type == 'a' ? "]" : "}");
        break;
    default:
        putsb(b, "null");
    }
}
char *encode(const struct value *v, int compact)
{
    struct buffer b = {0};
    encoded(&b, v, compact);
    return b.data;
}
static const char *compare(const struct value *e, const struct value *a, char *why, size_t size,
                           const char *path)
{
    const struct value *p, *q;
    char next[1024];
    size_t i = 0;
    if (!e || !a || e->type != a->type)
        goto differs;
    if (e->type == 'o') {
        for (p = e->child; p; p = p->next) {
            q = get(a, p->key);
            if (!q) {
                snprintf(why, size, "%s has no member \"%s\"", path, p->key);
                return why;
            }
            snprintf(next, sizeof(next), "%s.%s", path, p->key);
            if (compare(p, q, why, size, next))
                return why;
        }
        return NULL;
    }
    if (e->type == 'a') {
        if (count(e) != count(a))
            goto differs;
        for (p = e->child, q = a->child; p; p = p->next, q = q->next, i++) {
            snprintf(next, sizeof(next), "%s[%lu]", path, (unsigned long)i);
            if (compare(p, q, why, size, next))
                return why;
        }
        return NULL;
    }
    if ((e->type == 's' && strcmp(e->text, a->text)) || (e->type == 'n' && e->number != a->number))
        goto differs;
    return NULL;
differs:
    snprintf(why, size, "%s differs from the expected value", path);
    return why;
}
const char *subset(const struct value *e, const struct value *a)
{
    /* A caller copies the diagnostic before another comparison. */
    static _Thread_local char why[2048];
    return compare(e, a, why, sizeof(why), "$");
}
