/*
 * json.h: a strict reader of the JSON documents of etude's contracts, and
 * the canonical form that their digests are taken of.
 */
#ifndef ETUDE_JSON_H
#define ETUDE_JSON_H

#include <stddef.h>

/* The largest integer a document may give, 2^53 - 1. */
#define JSON_INTEGER_MAX 9007199254740991LL

enum json_type {
    JSON_NULL,
    JSON_BOOLEAN,
    JSON_INTEGER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
    JSON_NUMBER
};

/* One value of a document. An object's members are its items, each with a
 * name, in the order the document gives them. */
struct json {
    enum json_type type;
    long long integer;  /* an integer's value; a boolean's, 0 or 1 */
    char *string;       /* text, or an external number's validated token */
    char *name;         /* the member's name, or NULL outside an object */
    struct json **item; /* an array's elements or an object's members */
    size_t count;       /* the number of items */
    int line;           /* the line of the document the value starts on */
};

/* A growing buffer of bytes, kept terminated by a NUL. */
struct jbuf {
    char *data;
    size_t len, cap;
};

/* The command named at the start of the reader's diagnostics, which each
 * verb of etude that reads JSON sets. */
extern const char *json_program;

int json_parse(const char *text, size_t len, int first_line, struct json **out, char *err,
               size_t errlen);
/* External data may name UTF-8 files and retain decimal number tokens.
 * Contract callers continue to use the ASCII, integer-only json_parse. */
int json_parse_data(const char *text, size_t len, int first_line, struct json **out, char *err,
                    size_t errlen);
void json_free(struct json *v);
const struct json *json_member(const struct json *object, const char *name);
const char *json_text(const struct json *object, const char *name);
int json_equal(const struct json *a, const struct json *b);
void json_canonical(const struct json *v, struct jbuf *b);
const char *json_type_name(enum json_type type);

void jbuf_put(struct jbuf *b, const char *s, size_t n);
void jbuf_puts(struct jbuf *b, const char *s);
void jbuf_free(struct jbuf *b);
void *json_alloc(size_t n);

#endif
