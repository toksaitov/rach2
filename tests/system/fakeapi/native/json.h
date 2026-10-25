/* json.h: JSON values for the scripted API, including UTF-8 and numbers. */
#ifndef FAKEAPI_JSON_H
#define FAKEAPI_JSON_H
#include <stddef.h>
struct value {
    char type;
    char *key, *text;
    double number;
    struct value *child, *next;
};
struct buffer {
    char *data;
    size_t len, cap;
};
void *allocate(size_t n);
void append(struct buffer *b, const void *p, size_t n);
void putsb(struct buffer *b, const char *s);
void clear(struct buffer *b);
struct value *parse(const char *text);
struct value *read_json(const char *path);
void destroy(struct value *v);
struct value *value(char type);
struct value *string(const char *s);
struct value *integer(long long n);
struct value *json_boolean(int n);
struct value *clone(const struct value *v);
void add(struct value *v, const char *key, struct value *item);
void set(struct value *v, const char *key, struct value *item);
const struct value *get(const struct value *v, const char *key);
const char *text(const struct value *v);
const char *field(const struct value *v, const char *key);
int flag(const struct value *v, const char *key, int fallback);
long long number(const struct value *v, const char *key, long long fallback);
size_t count(const struct value *v);
char *encode(const struct value *v, int compact);
const char *subset(const struct value *expected, const struct value *actual);
#endif
