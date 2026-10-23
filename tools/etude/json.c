/*
 * json.c: the strict JSON reader of etude's contracts, and their canonical
 * form.
 *
 * A document of the contracts is ASCII, has no member named twice in one
 * object and gives every number as an integer (tools/etude/Readme.md); the
 * reader refuses any other text, where a general reader would accept it and
 * leave the difference to be found later. The canonical form is the one the
 * history's digests are taken of: members sorted by name, no space outside
 * strings, and the escapes of Python's json.dumps.
 */
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The deepest nesting a document may have. */
#define DEPTH_MAX 64

/* The state of one parse: the text, the place reached and the first error. */
struct parser {
    const char *p, *end;
    int line;
    int depth;
    char *err;
    size_t errlen;
    int failed;
};

const char *json_program = "etude";

/** malloc that exits with status 2 when memory runs out; never NULL. */
void *json_alloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);

    if (p == NULL) {
        fprintf(stderr, "%s: out of memory\n", json_program);
        exit(2);
    }
    return p;
}

/** Appends `n` bytes of `s` to `b`, which stays terminated. */
void jbuf_put(struct jbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        char *grown;

        while (b->len + n + 1 > cap) {
            cap *= 2;
        }
        grown = realloc(b->data, cap);
        if (grown == NULL) {
            fprintf(stderr, "%s: out of memory\n", json_program);
            exit(2);
        }
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

/** Appends the string `s` to `b`. */
void jbuf_puts(struct jbuf *b, const char *s)
{
    jbuf_put(b, s, strlen(s));
}

/** Frees the bytes of `b` and leaves it empty. */
void jbuf_free(struct jbuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/** Records the first error of the parse, at the current line. */
static void fail(struct parser *ps, const char *what)
{
    if (!ps->failed) {
        snprintf(ps->err, ps->errlen, "line %d: %s", ps->line, what);
        ps->failed = 1;
    }
}

/** Skips the white space JSON allows between tokens, counting lines. */
static void skip_space(struct parser *ps)
{
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) {
        if (*ps->p == '\n') {
            ps->line++;
        }
        ps->p++;
    }
}

/** Returns a new value of `type` that starts on the current line. */
static struct json *new_value(const struct parser *ps, enum json_type type)
{
    struct json *v = json_alloc(sizeof *v);

    v->type = type;
    v->line = ps->line;
    return v;
}

/** Appends `child` to the items of the array or object `v`. */
static void add_item(struct json *v, struct json *child)
{
    struct json **grown = realloc(v->item, (v->count + 1) * sizeof *grown);

    if (grown == NULL) {
        fprintf(stderr, "%s: out of memory\n", json_program);
        exit(2);
    }
    v->item = grown;
    v->item[v->count++] = child;
}

/** Reads the four hexadecimal digits of a \u escape; returns -1 for others. */
static int hex4(const char *p)
{
    int i, code = 0;

    for (i = 0; i < 4; i++) {
        char c = p[i];
        int digit;

        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return -1;
        }
        code = code * 16 + digit;
    }
    return code;
}

/** Reads one escape after its backslash into `out`; returns 0 or -1. */
static int read_escape(struct parser *ps, struct jbuf *out)
{
    static const char from[] = "\"\\/bfnrt";
    static const char to[] = "\"\\/\b\f\n\r\t";
    const char *at;
    char c;
    int code;

    if (ps->p >= ps->end) {
        fail(ps, "a string ends inside an escape");
        return -1;
    }
    at = strchr(from, *ps->p);
    if (*ps->p != '\0' && at != NULL) {
        jbuf_put(out, &to[at - from], 1);
        ps->p++;
        return 0;
    }
    if (*ps->p != 'u' || ps->end - ps->p < 5 || (code = hex4(ps->p + 1)) < 0) {
        fail(ps, "a string has an escape JSON does not define");
        return -1;
    }
    if (code == 0 || code > 0x7f) {
        fail(ps, "a string escapes a NUL or a character outside ASCII");
        return -1;
    }
    c = (char)code;
    jbuf_put(out, &c, 1);
    ps->p += 5;
    return 0;
}

/** Reads a string from its opening quote; returns its text or NULL. */
static char *read_string(struct parser *ps)
{
    struct jbuf out = {0};

    ps->p++;
    jbuf_put(&out, "", 0);
    while (ps->p < ps->end && *ps->p != '"') {
        unsigned char c = (unsigned char)*ps->p;

        if (c < 0x20 || c > 0x7f) {
            fail(ps, c < 0x20 ? "a string has a control character" : "a byte outside ASCII");
            jbuf_free(&out);
            return NULL;
        }
        ps->p++;
        if (c != '\\') {
            jbuf_put(&out, (const char *)&c, 1);
        } else if (read_escape(ps, &out) != 0) {
            jbuf_free(&out);
            return NULL;
        }
    }
    if (ps->p >= ps->end) {
        fail(ps, "a string has no closing quote");
        jbuf_free(&out);
        return NULL;
    }
    ps->p++;
    return out.data;
}

/** Reads an integer; a fraction, an exponent or a leading zero is refused. */
static struct json *read_integer(struct parser *ps)
{
    struct json *v;
    long long value = 0;
    int negative = 0;

    if (*ps->p == '-') {
        negative = 1;
        ps->p++;
    }
    if (ps->p >= ps->end || *ps->p < '0' || *ps->p > '9') {
        fail(ps, "a value JSON does not define");
        return NULL;
    }
    if (*ps->p == '0' && ps->end - ps->p > 1 && ps->p[1] >= '0' && ps->p[1] <= '9') {
        fail(ps, "an integer with a leading zero");
        return NULL;
    }
    while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9') {
        value = value * 10 + (*ps->p++ - '0');
        if (value > JSON_INTEGER_MAX) {
            fail(ps, "an integer beyond 2^53 - 1");
            return NULL;
        }
    }
    if (ps->p < ps->end && (*ps->p == '.' || *ps->p == 'e' || *ps->p == 'E')) {
        fail(ps, "a number that is not an integer");
        return NULL;
    }
    v = new_value(ps, JSON_INTEGER);
    v->integer = negative ? -value : value;
    return v;
}

static struct json *read_value(struct parser *ps);

/** Reads the members of an object after its opening brace. */
static struct json *read_object(struct parser *ps, struct json *v)
{
    skip_space(ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        return v;
    }
    for (;;) {
        struct json *member;
        char *name;

        skip_space(ps);
        if (ps->p >= ps->end || *ps->p != '"') {
            fail(ps, "an object member has no quoted name");
            return NULL;
        }
        name = read_string(ps);
        if (name == NULL) {
            return NULL;
        }
        if (json_member(v, name) != NULL) {
            fail(ps, "an object names a member twice");
            free(name);
            return NULL;
        }
        skip_space(ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            fail(ps, "an object member has no colon after its name");
            free(name);
            return NULL;
        }
        ps->p++;
        member = read_value(ps);
        if (member == NULL) {
            free(name);
            return NULL;
        }
        member->name = name;
        add_item(v, member);
        skip_space(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == '}') {
            ps->p++;
            return v;
        }
        fail(ps, "an object member is followed by neither a comma nor a brace");
        return NULL;
    }
}

/** Reads the elements of an array after its opening bracket. */
static struct json *read_array(struct parser *ps, struct json *v)
{
    skip_space(ps);
    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        return v;
    }
    for (;;) {
        struct json *element = read_value(ps);

        if (element == NULL) {
            return NULL;
        }
        add_item(v, element);
        skip_space(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            return v;
        }
        fail(ps, "an array element is followed by neither a comma nor a bracket");
        return NULL;
    }
}

/** Reads `word` as a literal of `type` with `value`; returns NULL if absent. */
static struct json *read_word(struct parser *ps, const char *word, enum json_type type, int value)
{
    size_t n = strlen(word);
    struct json *v;

    if ((size_t)(ps->end - ps->p) < n || memcmp(ps->p, word, n) != 0) {
        fail(ps, "a value JSON does not define");
        return NULL;
    }
    ps->p += n;
    v = new_value(ps, type);
    v->integer = value;
    return v;
}

/** Reads a container whose opening byte is at the current place. */
static struct json *read_container(struct parser *ps)
{
    struct json *v, *done;
    int object = *ps->p == '{';

    if (++ps->depth > DEPTH_MAX) {
        fail(ps, "the document is nested too deeply");
        return NULL;
    }
    v = new_value(ps, object ? JSON_OBJECT : JSON_ARRAY);
    ps->p++;
    done = object ? read_object(ps, v) : read_array(ps, v);
    ps->depth--;
    if (done == NULL) {
        json_free(v);
    }
    return done;
}

/** Reads one value at the current place; returns NULL after an error. */
static struct json *read_value(struct parser *ps)
{
    struct json *v;
    char *text;

    skip_space(ps);
    if (ps->p >= ps->end) {
        fail(ps, "the document ends where a value should be");
        return NULL;
    }
    switch (*ps->p) {
    case '{':
    case '[':
        return read_container(ps);
    case '"':
        v = new_value(ps, JSON_STRING);
        text = read_string(ps);
        if (text == NULL) {
            free(v);
            return NULL;
        }
        v->string = text;
        return v;
    case 't':
        return read_word(ps, "true", JSON_BOOLEAN, 1);
    case 'f':
        return read_word(ps, "false", JSON_BOOLEAN, 0);
    case 'n':
        return read_word(ps, "null", JSON_NULL, 0);
    default:
        return read_integer(ps);
    }
}

/**
 * Parses one document of `len` bytes at `text`, whose first byte is on line
 * `first_line` of its file.
 *
 * Returns 0 and the document in `*out`, which the caller frees with
 * json_free, or -1 with the first error written to `err` as `line N: WHAT`.
 * Anything but white space after the document is an error.
 */
int json_parse(const char *text, size_t len, int first_line, struct json **out, char *err,
               size_t errlen)
{
    struct parser ps;
    struct json *v;

    ps.p = text;
    ps.end = text + len;
    ps.line = first_line;
    ps.depth = 0;
    ps.err = err;
    ps.errlen = errlen;
    ps.failed = 0;
    *out = NULL;
    v = read_value(&ps);
    if (v == NULL) {
        return -1;
    }
    skip_space(&ps);
    if (ps.p < ps.end) {
        fail(&ps, "text follows the document");
        json_free(v);
        return -1;
    }
    *out = v;
    return 0;
}

/** Frees `v` and every value inside it. */
void json_free(struct json *v)
{
    size_t i;

    if (v == NULL) {
        return;
    }
    for (i = 0; i < v->count; i++) {
        json_free(v->item[i]);
    }
    free(v->item);
    free(v->string);
    free(v->name);
    free(v);
}

/** Returns the member `name` of `object`, or NULL when it has none. */
const struct json *json_member(const struct json *object, const char *name)
{
    size_t i;

    if (object == NULL || object->type != JSON_OBJECT) {
        return NULL;
    }
    for (i = 0; i < object->count; i++) {
        if (strcmp(object->item[i]->name, name) == 0) {
            return object->item[i];
        }
    }
    return NULL;
}

/** Returns the text of the string member `name`, or NULL when it is not one. */
const char *json_text(const struct json *object, const char *name)
{
    const struct json *v = json_member(object, name);

    return v != NULL && v->type == JSON_STRING ? v->string : NULL;
}

/** Returns 1 when `a` and `b` are equal JSON values, members in any order. */
int json_equal(const struct json *a, const struct json *b)
{
    size_t i;

    if (a->type != b->type || a->count != b->count) {
        return 0;
    }
    switch (a->type) {
    case JSON_STRING:
        return strcmp(a->string, b->string) == 0;
    case JSON_ARRAY:
        for (i = 0; i < a->count; i++) {
            if (!json_equal(a->item[i], b->item[i])) {
                return 0;
            }
        }
        return 1;
    case JSON_OBJECT:
        for (i = 0; i < a->count; i++) {
            const struct json *other = json_member(b, a->item[i]->name);

            if (other == NULL || !json_equal(a->item[i], other)) {
                return 0;
            }
        }
        return 1;
    default:
        return a->integer == b->integer;
    }
}

/** Appends `s` to `b` as a JSON string, escaped as json.dumps escapes it. */
static void put_string(struct jbuf *b, const char *s)
{
    jbuf_put(b, "\"", 1);
    for (; *s != '\0'; s++) {
        char esc[8];

        switch (*s) {
        case '"':
            jbuf_puts(b, "\\\"");
            break;
        case '\\':
            jbuf_puts(b, "\\\\");
            break;
        case '\n':
            jbuf_puts(b, "\\n");
            break;
        case '\r':
            jbuf_puts(b, "\\r");
            break;
        case '\t':
            jbuf_puts(b, "\\t");
            break;
        case '\b':
            jbuf_puts(b, "\\b");
            break;
        case '\f':
            jbuf_puts(b, "\\f");
            break;
        default:
            if ((unsigned char)*s < 0x20) {
                snprintf(esc, sizeof esc, "\\u%04x", (unsigned)(unsigned char)*s);
                jbuf_puts(b, esc);
            } else {
                jbuf_put(b, s, 1);
            }
        }
    }
    jbuf_put(b, "\"", 1);
}

/** Orders two members by name, for qsort. */
static int by_name(const void *a, const void *b)
{
    const struct json *const *x = a;
    const struct json *const *y = b;

    return strcmp((*x)->name, (*y)->name);
}

/** Appends the members of `v` to `b` in the order of their names. */
static void put_object(const struct json *v, struct jbuf *b)
{
    const struct json **sorted = json_alloc((v->count ? v->count : 1) * sizeof *sorted);
    size_t i;

    for (i = 0; i < v->count; i++) {
        sorted[i] = v->item[i];
    }
    qsort(sorted, v->count, sizeof *sorted, by_name);
    jbuf_put(b, "{", 1);
    for (i = 0; i < v->count; i++) {
        if (i > 0) {
            jbuf_put(b, ",", 1);
        }
        put_string(b, sorted[i]->name);
        jbuf_put(b, ":", 1);
        json_canonical(sorted[i], b);
    }
    jbuf_put(b, "}", 1);
    free(sorted);
}

/**
 * Appends the canonical form of `v` to `b`: members sorted by name, no
 * space outside strings, integers in decimal, and the string escapes of
 * Python's json.dumps, so that equal values give equal bytes.
 */
void json_canonical(const struct json *v, struct jbuf *b)
{
    char number[32];
    size_t i;

    switch (v->type) {
    case JSON_NULL:
        jbuf_puts(b, "null");
        break;
    case JSON_BOOLEAN:
        jbuf_puts(b, v->integer ? "true" : "false");
        break;
    case JSON_INTEGER:
        snprintf(number, sizeof number, "%lld", v->integer);
        jbuf_puts(b, number);
        break;
    case JSON_STRING:
        put_string(b, v->string);
        break;
    case JSON_ARRAY:
        jbuf_put(b, "[", 1);
        for (i = 0; i < v->count; i++) {
            if (i > 0) {
                jbuf_put(b, ",", 1);
            }
            json_canonical(v->item[i], b);
        }
        jbuf_put(b, "]", 1);
        break;
    case JSON_OBJECT:
        put_object(v, b);
        break;
    }
}

/** Returns the JSON Schema name of `type`. */
const char *json_type_name(enum json_type type)
{
    static const char *const names[] = {"null", "boolean", "integer", "string", "array", "object"};

    return names[type];
}
