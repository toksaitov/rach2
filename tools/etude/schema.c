/*
 * schema.c: a checker of documents against the JSON Schemas of etude's
 * contracts.
 *
 * The schemas are the definition of each document's shape, so the checker
 * reads them rather than repeating them in C. It knows the keywords those
 * schemas use, of JSON Schema's 2020-12 draft, and reports a schema that
 * uses another as a fault of the schema, so that a keyword it does not read
 * cannot pass a document unchecked. Patterns are regular expressions of a
 * small form: anchors, literals, `.`, classes, groups and the quantifiers
 * `*`, `+`, `?` and `{m,n}`, without alternation.
 */
#include "schema.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The keywords the checker reads, and those it takes as annotations. */
static const char *const KEYWORDS[] = {"$ref",
                                       "type",
                                       "enum",
                                       "const",
                                       "properties",
                                       "required",
                                       "additionalProperties",
                                       "propertyNames",
                                       "items",
                                       "minItems",
                                       "uniqueItems",
                                       "minimum",
                                       "maximum",
                                       "minLength",
                                       "maxLength",
                                       "pattern",
                                       "allOf",
                                       "anyOf",
                                       "oneOf",
                                       "not",
                                       "if",
                                       "then",
                                       "else",
                                       "$schema",
                                       "$defs",
                                       "title",
                                       "description",
                                       "format"};

/* The state of one check: the schema's root, where the reports go, and the
 * place in the document reached, as a JSON pointer. */
struct checker {
    const struct json *root;
    schema_report report;
    void *ctx;
    char path[1024];
    size_t len;
};

static int check(struct checker *c, const struct json *s, const struct json *v, int quiet);

/* ---- patterns ---- */

/* What follows a part of a pattern: more of the pattern, or another turn of
 * a group that repeats. */
struct cont {
    const char *p, *pend;     /* the rest of the pattern, for a sequence */
    const char *group, *gend; /* the group's text, for a repetition */
    int count, min, max;      /* the turns taken, and the bounds; max -1 for none */
    const char *start;        /* where the last turn began */
    const struct cont *next;
};

static int match_here(const char *p, const char *pend, const char *s, const struct cont *k);

/**
 * Returns the end of the atom at `p`: a class, an escape, a group or a byte.
 */
static const char *atom_end(const char *p, const char *pend)
{
    int depth = 0;

    if (*p == '\\') {
        return p + 2 <= pend ? p + 2 : pend;
    }
    if (*p == '[') {
        for (p++; p < pend && *p != ']'; p++) {
            if (*p == '\\') {
                p++;
            }
        }
        return p < pend ? p + 1 : pend;
    }
    if (*p != '(') {
        return p + 1;
    }
    for (; p < pend; p++) {
        if (*p == '\\') {
            p++;
        } else if (*p == '(') {
            depth++;
        } else if (*p == ')' && --depth == 0) {
            return p + 1;
        }
    }
    return pend;
}

/**
 * Reads the quantifier at `*q`, if any, into `min` and `max`; advances `*q`.
 */
static void quantifier(const char **q, const char *pend, int *min, int *max)
{
    const char *p = *q;

    *min = *max = 1;
    if (p >= pend) {
        return;
    }
    if (*p == '*' || *p == '+' || *p == '?') {
        *min = *p == '+' ? 1 : 0;
        *max = *p == '?' ? 1 : -1;
        *q = p + 1;
        return;
    }
    if (*p != '{') {
        return;
    }
    for (*min = 0, p++; p < pend && *p >= '0' && *p <= '9'; p++) {
        *min = *min * 10 + (*p - '0');
    }
    *max = *min;
    if (p < pend && *p == ',') {
        for (*max = 0, p++; p < pend && *p >= '0' && *p <= '9'; p++) {
            *max = *max * 10 + (*p - '0');
        }
        if (p[-1] == ',') {
            *max = -1;
        }
    }
    *q = p < pend ? p + 1 : pend;
}

/**
 * Returns 1 when the byte `ch` is in the class of `[p, end)`, brackets
 * included.
 */
static int in_class(const char *p, const char *end, char ch)
{
    int negate = 0, found = 0;

    p++;
    end--;
    if (p < end && *p == '^') {
        negate = 1;
        p++;
    }
    while (p < end) {
        char lo = *p == '\\' ? *++p : *p;
        char hi = lo;

        p++;
        if (p + 1 < end && *p == '-') {
            hi = p[1] == '\\' ? p[2] : p[1];
            p += p[1] == '\\' ? 3 : 2;
        }
        if (ch >= lo && ch <= hi) {
            found = 1;
        }
    }
    return found != negate;
}

/** Returns 1 when the one-byte atom `[p, end)` matches the byte `ch`. */
static int atom_matches(const char *p, const char *end, char ch)
{
    if (ch == '\0') {
        return 0;
    }
    if (*p == '[') {
        return in_class(p, end, ch);
    }
    if (*p == '.') {
        return 1;
    }
    return (*p == '\\' ? p[1] : *p) == ch;
}

/** Continues after a part of the pattern has matched up to `s`. */
static int resume(const char *s, const struct cont *k)
{
    struct cont again;

    if (k == NULL) {
        return 1;
    }
    if (k->group == NULL) {
        return match_here(k->p, k->pend, s, k->next);
    }
    again = *k;
    again.count = k->count + 1;
    if (s == k->start && again.count < again.min) {
        again.count = again.min;
    }
    if (s != k->start && (again.max < 0 || again.count < again.max)) {
        again.start = s;
        if (match_here(k->group, k->gend, s, &again)) {
            return 1;
        }
    }
    return again.count >= again.min && match_here(k->p, k->pend, s, k->next);
}

/** Matches a group of `[g, gend)` repeated `min` to `max` times at `s`. */
static int match_group(const char *g, const char *gend, const char *s, struct cont *rest)
{
    struct cont turn = *rest;

    turn.group = g;
    turn.gend = gend;
    turn.count = 0;
    turn.start = s;
    if (turn.max != 0 && match_here(g, gend, s, &turn)) {
        return 1;
    }
    return turn.min == 0 && match_here(rest->p, rest->pend, s, rest->next);
}

/** Matches the pattern `[p, pend)` at `s`, then whatever `k` asks for. */
static int match_here(const char *p, const char *pend, const char *s, const struct cont *k)
{
    const char *end, *after;
    struct cont rest;
    int min, max, n;

    if (p >= pend) {
        return resume(s, k);
    }
    if (*p == '$' && p + 1 == pend && k == NULL) {
        return *s == '\0';
    }
    end = atom_end(p, pend);
    after = end;
    quantifier(&after, pend, &min, &max);
    if (*p == '(') {
        memset(&rest, 0, sizeof rest);
        rest.p = after;
        rest.pend = pend;
        rest.min = min;
        rest.max = max;
        rest.next = k;
        return match_group(p + 1, end - 1, s, &rest);
    }
    for (n = 0; (max < 0 || n < max) && atom_matches(p, end, s[n]); n++) {
    }
    for (; n >= min; n--) {
        if (match_here(after, pend, s + n, k)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Returns 1 when `text` matches `pattern` as JSON Schema matches: anywhere
 * in the text, unless the pattern begins with `^` or ends with `$`.
 */
int schema_pattern(const char *pattern, const char *text)
{
    const char *pend = pattern + strlen(pattern);
    const char *s = text;

    if (*pattern == '^') {
        return match_here(pattern + 1, pend, text, NULL);
    }
    do {
        if (match_here(pattern, pend, s, NULL)) {
            return 1;
        }
    } while (*s++ != '\0');
    return 0;
}

/* ---- reports ---- */

/** Reports a fault at the current place, unless `quiet`; returns 1. */
static int fault(struct checker *c, const struct json *v, int quiet, const char *fmt, ...)
{
    char what[512];
    va_list ap;

    if (quiet) {
        return 1;
    }
    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    c->report(c->ctx, v->line, c->len ? c->path : "/", what);
    return 1;
}

/** Appends `/name` to the current place and returns the old length. */
static size_t push(struct checker *c, const char *name)
{
    size_t old = c->len;
    int n = snprintf(c->path + c->len, sizeof c->path - c->len, "/%s", name);

    if (n > 0) {
        c->len += (size_t)n < sizeof c->path - c->len ? (size_t)n : sizeof c->path - c->len - 1;
    }
    return old;
}

/** Appends `/index` to the current place and returns the old length. */
static size_t push_index(struct checker *c, size_t index)
{
    char number[32];

    snprintf(number, sizeof number, "%lu", (unsigned long)index);
    return push(c, number);
}

/** Returns the current place to the length `old`. */
static void pop(struct checker *c, size_t old)
{
    c->len = old;
    c->path[old] = '\0';
}

/** Writes a short form of `v` into `out`, for a report. */
static void describe(const struct json *v, char *out, size_t n)
{
    struct jbuf b = {0};

    json_canonical(v, &b);
    if (b.len > 60) {
        snprintf(out, n, "%.57s...", b.data);
    } else {
        snprintf(out, n, "%s", b.data);
    }
    jbuf_free(&b);
}

/* ---- keywords ---- */

/**
 * Returns the value the JSON pointer `ref`, after its `#`, names in `root`, or
 * NULL.
 */
static const struct json *resolve(const struct json *root, const char *ref)
{
    const struct json *v = root;
    char token[256];

    while (v != NULL && *ref == '/') {
        size_t n = 0;

        for (ref++; *ref != '\0' && *ref != '/' && n + 1 < sizeof token; ref++) {
            if (*ref == '~' && (ref[1] == '0' || ref[1] == '1')) {
                token[n++] = ref[1] == '0' ? '~' : '/';
                ref++;
            } else {
                token[n++] = *ref;
            }
        }
        token[n] = '\0';
        if (v->type == JSON_ARRAY) {
            char *end;
            unsigned long index = strtoul(token, &end, 10);

            v = *end == '\0' && n > 0 && index < v->count ? v->item[index] : NULL;
        } else {
            v = json_member(v, token);
        }
    }
    return *ref == '\0' ? v : NULL;
}

/** Returns the schema a `$ref` of `s` refers to, or `s` when it has none. */
static const struct json *deref(const struct checker *c, const struct json *s)
{
    const char *ref = json_text(s, "$ref");

    if (ref == NULL) {
        return s;
    }
    return ref[0] == '#' ? resolve(c->root, ref + 1) : NULL;
}

/**
 * Returns the JSON Schema type name `type` with its article, as a report says
 * it.
 */
static const char *with_article(const char *type)
{
    static const char *const forms[] = {
        "null",     "null",   "boolean",  "a boolean", "integer",  "an integer", "number",
        "a number", "string", "a string", "array",     "an array", "object",     "an object"};
    size_t i;

    for (i = 0; i < sizeof forms / sizeof forms[0]; i += 2) {
        if (strcmp(forms[i], type) == 0) {
            return forms[i + 1];
        }
    }
    return type;
}

/** Returns 1 when `v` has the JSON Schema type `type`. */
static int type_is(const char *type, const struct json *v)
{
    if (strcmp(type, "number") == 0) {
        return v->type == JSON_INTEGER;
    }
    return strcmp(type, json_type_name(v->type)) == 0;
}

/** Applies `enum` and `const`. */
static int check_values(struct checker *c, const struct json *s, const struct json *v, int quiet)
{
    const struct json *list = json_member(s, "enum");
    const struct json *one = json_member(s, "const");
    char text[80];
    size_t i;

    if (one != NULL && !json_equal(one, v)) {
        describe(one, text, sizeof text);
        return fault(c, v, quiet, "is not %s", text);
    }
    if (list == NULL) {
        return 0;
    }
    for (i = 0; i < list->count; i++) {
        if (json_equal(list->item[i], v)) {
            return 0;
        }
    }
    describe(v, text, sizeof text);
    return fault(c, v, quiet, "%s is not one of the values the schema allows", text);
}

/** Applies `required`. */
static int check_required(struct checker *c, const struct json *s, const struct json *v, int quiet)
{
    const struct json *required = json_member(s, "required");
    int bad = 0;
    size_t i;

    for (i = 0; required != NULL && i < required->count; i++) {
        if (json_member(v, required->item[i]->string) == NULL) {
            bad += fault(c, v, quiet, "has no member \"%s\"", required->item[i]->string);
            if (quiet) {
                return bad;
            }
        }
    }
    return bad;
}

/** Applies `properties`, `additionalProperties` and `propertyNames`. */
static int check_members(struct checker *c, const struct json *s, const struct json *v, int quiet)
{
    const struct json *props = json_member(s, "properties");
    const struct json *extra = json_member(s, "additionalProperties");
    const struct json *names = json_member(s, "propertyNames");
    int bad = 0;
    size_t i;

    for (i = 0; i < v->count && !(quiet && bad); i++) {
        const struct json *m = v->item[i];
        const struct json *sub = json_member(props, m->name);
        size_t old = push(c, m->name);
        struct json name = {0};

        name.type = JSON_STRING;
        name.string = m->name;
        name.line = m->line;
        if (names != NULL) {
            bad += check(c, names, &name, quiet);
        }
        if (sub != NULL) {
            bad += check(c, sub, m, quiet);
        } else if (extra != NULL && extra->type == JSON_BOOLEAN && !extra->integer) {
            bad += fault(c, m, quiet, "is a member the schema does not allow");
        } else if (extra != NULL && extra->type == JSON_OBJECT) {
            bad += check(c, extra, m, quiet);
        }
        pop(c, old);
    }
    return bad;
}

/** Applies the keywords of an object. */
static int check_object(struct checker *c, const struct json *s, const struct json *v, int quiet)
{
    int bad = check_required(c, s, v, quiet);

    if (quiet && bad) {
        return bad;
    }
    return bad + check_members(c, s, v, quiet);
}

/** Applies `items`, `minItems` and `uniqueItems`. */
static int check_array(struct checker *c, const struct json *s, const struct json *v, int quiet)
{
    const struct json *items = json_member(s, "items");
    const struct json *min = json_member(s, "minItems");
    const struct json *unique = json_member(s, "uniqueItems");
    int bad = 0;
    size_t i, j;

    if (min != NULL && (long long)v->count < min->integer) {
        bad += fault(c, v, quiet, "has %lu items, fewer than %lld", (unsigned long)v->count,
                     min->integer);
    }
    for (i = 0; items != NULL && i < v->count && !(quiet && bad); i++) {
        size_t old = push_index(c, i);

        bad += check(c, items, v->item[i], quiet);
        pop(c, old);
    }
    for (i = 0; unique != NULL && unique->integer && i < v->count; i++) {
        for (j = i + 1; j < v->count; j++) {
            if (json_equal(v->item[i], v->item[j])) {
                return bad + fault(c, v, quiet, "has items %lu and %lu equal", (unsigned long)i,
                                   (unsigned long)j);
            }
        }
    }
    return bad;
}

/** Applies `minLength`, `maxLength` and `pattern`. */
static int check_string(struct checker *c, const struct json *s, const struct json *v, int quiet)
{
    const struct json *min = json_member(s, "minLength");
    const struct json *max = json_member(s, "maxLength");
    const char *pattern = json_text(s, "pattern");
    long long len = (long long)strlen(v->string);
    char text[80];

    if (min != NULL && len < min->integer) {
        return fault(c, v, quiet, "is shorter than %lld characters", min->integer);
    }
    if (max != NULL && len > max->integer) {
        return fault(c, v, quiet, "is longer than %lld characters", max->integer);
    }
    if (pattern != NULL && !schema_pattern(pattern, v->string)) {
        describe(v, text, sizeof text);
        return fault(c, v, quiet, "%s does not match %s", text, pattern);
    }
    return 0;
}

/** Applies `minimum` and `maximum`. */
static int check_integer(struct checker *c, const struct json *s, const struct json *v, int quiet)
{
    const struct json *min = json_member(s, "minimum");
    const struct json *max = json_member(s, "maximum");

    if (min != NULL && v->integer < min->integer) {
        return fault(c, v, quiet, "%lld is less than %lld", v->integer, min->integer);
    }
    if (max != NULL && v->integer > max->integer) {
        return fault(c, v, quiet, "%lld is more than %lld", v->integer, max->integer);
    }
    return 0;
}

/** Returns 1 when the alternative `alt` could be meant for `v` by its type. */
static int could_be(const struct checker *c, const struct json *alt, const struct json *v)
{
    const struct json *s = deref(c, alt);
    const char *type = json_text(s, "type");
    const struct json *one = json_member(s, "const");
    const struct json *list = json_member(s, "enum");
    size_t i;

    if (s == NULL || (type != NULL && !type_is(type, v))) {
        return 0;
    }
    if (one != NULL && one->type != v->type) {
        return 0;
    }
    for (i = 0; list != NULL && i < list->count; i++) {
        if (list->item[i]->type == v->type) {
            return 1;
        }
    }
    return list == NULL;
}

/**
 * Returns 1 when the object alternative `alt` names `v`'s form by a constant
 * member.
 */
static int named_by(const struct checker *c, const struct json *alt, const struct json *v)
{
    const struct json *props = json_member(deref(c, alt), "properties");
    size_t i;

    for (i = 0; props != NULL && i < props->count; i++) {
        const struct json *one = json_member(props->item[i], "const");
        const struct json *m = json_member(v, props->item[i]->name);

        if (one != NULL && m != NULL && json_equal(one, m)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Returns 1 when any alternative of `alts` could be meant for `v` by its type.
 */
static int any_could_be(const struct checker *c, const struct json *alts, const struct json *v)
{
    size_t i;

    for (i = 0; i < alts->count; i++) {
        if (could_be(c, alts->item[i], v)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Returns the one alternative of `alts` that `v` was evidently meant for, or
 * NULL.
 */
static const struct json *meant(const struct checker *c, const struct json *alts,
                                const struct json *v)
{
    const struct json *found = NULL;
    int n = 0;
    size_t i;

    for (i = 0; i < alts->count; i++) {
        if (could_be(c, alts->item[i], v)) {
            found = alts->item[i];
            n++;
        }
    }
    if (n <= 1 || v->type != JSON_OBJECT) {
        return n == 1 ? found : NULL;
    }
    for (n = 0, i = 0; i < alts->count; i++) {
        if (could_be(c, alts->item[i], v) && named_by(c, alts->item[i], v)) {
            found = alts->item[i];
            n++;
        }
    }
    return n == 1 ? found : NULL;
}

/** Applies `oneOf` or, with `any`, `anyOf`. */
static int check_choice(struct checker *c, const struct json *alts, const struct json *v, int quiet,
                        int any)
{
    const struct json *alt;
    char types[128];
    int n = 0;
    size_t i;

    for (i = 0; i < alts->count; i++) {
        n += check(c, alts->item[i], v, 1) == 0;
    }
    if (n == 1 || (any && n > 1)) {
        return 0;
    }
    if (n > 1) {
        return fault(c, v, quiet, "matches more than one of the forms the schema allows");
    }
    alt = meant(c, alts, v);
    if (alt != NULL && !quiet) {
        return check(c, alt, v, 0);
    }
    if (quiet) {
        return 1;
    }
    types[0] = '\0';
    for (i = 0; i < alts->count; i++) {
        const char *type = json_text(deref(c, alts->item[i]), "type");
        size_t len = strlen(types);

        if (type != NULL && strstr(types, type) == NULL) {
            snprintf(types + len, sizeof types - len, "%s%s", len ? " or " : "",
                     with_article(type));
        }
    }
    if (types[0] != '\0' && meant(c, alts, v) == NULL && !any_could_be(c, alts, v)) {
        return fault(c, v, 0, "is %s, where the schema wants %s",
                     with_article(json_type_name(v->type)), types);
    }
    return fault(c, v, 0, "matches none of the forms the schema allows");
}

/** Adds to `out` the members `v` has that a `required` of `s` names. */
static void present(const struct json *s, const struct json *v, char *out, size_t n)
{
    const struct json *required = json_member(s, "required");
    size_t i, len;

    for (i = 0; required != NULL && i < required->count; i++) {
        if (json_member(v, required->item[i]->string) != NULL) {
            len = strlen(out);
            snprintf(out + len, n - len, "%s\"%s\"", len ? ", " : "", required->item[i]->string);
        }
    }
}

/**
 * Applies `not`, naming the members it forbids when it is made of `required`.
 */
static int check_not(struct checker *c, const struct json *s, const struct json *v, int quiet)
{
    const struct json *sub = deref(c, s);
    const struct json *any = json_member(sub, "anyOf");
    char names[256] = "";
    size_t i;

    if (check(c, s, v, 1) != 0) {
        return 0;
    }
    present(sub, v, names, sizeof names);
    for (i = 0; any != NULL && i < any->count; i++) {
        present(deref(c, any->item[i]), v, names, sizeof names);
    }
    if (names[0] != '\0') {
        return fault(c, v, quiet, "has %s, which this form does not allow", names);
    }
    return fault(c, v, quiet, "has a form the schema does not allow");
}

/** Applies `allOf`, `anyOf`, `oneOf`, `not` and `if`, `then` and `else`. */
static int check_logic(struct checker *c, const struct json *s, const struct json *v, int quiet)
{
    const struct json *all = json_member(s, "allOf");
    const struct json *any = json_member(s, "anyOf");
    const struct json *one = json_member(s, "oneOf");
    const struct json *neg = json_member(s, "not");
    const struct json *cond = json_member(s, "if");
    const struct json *branch = NULL;
    int bad = 0;
    size_t i;

    for (i = 0; all != NULL && i < all->count && !(quiet && bad); i++) {
        bad += check(c, all->item[i], v, quiet);
    }
    if (any != NULL && !(quiet && bad)) {
        bad += check_choice(c, any, v, quiet, 1);
    }
    if (one != NULL && !(quiet && bad)) {
        bad += check_choice(c, one, v, quiet, 0);
    }
    if (neg != NULL && !(quiet && bad)) {
        bad += check_not(c, neg, v, quiet);
    }
    if (cond != NULL && !(quiet && bad)) {
        branch = json_member(s, check(c, cond, v, 1) == 0 ? "then" : "else");
    }
    if (branch != NULL) {
        bad += check(c, branch, v, quiet);
    }
    return bad;
}

/** Reports each keyword of `s` that the checker does not read. */
static int check_keywords(struct checker *c, const struct json *s, const struct json *v)
{
    int bad = 0;
    size_t i, k;

    for (i = 0; i < s->count; i++) {
        for (k = 0; k < sizeof KEYWORDS / sizeof KEYWORDS[0]; k++) {
            if (strcmp(s->item[i]->name, KEYWORDS[k]) == 0) {
                break;
            }
        }
        if (k == sizeof KEYWORDS / sizeof KEYWORDS[0]) {
            bad += fault(c, v, 0, "the schema uses %s, which etude check does not read",
                         s->item[i]->name);
        }
    }
    return bad;
}

/** Applies the keywords of the type of `v`. */
static int check_kind(struct checker *c, const struct json *s, const struct json *v, int quiet)
{
    switch (v->type) {
    case JSON_OBJECT:
        return check_object(c, s, v, quiet);
    case JSON_ARRAY:
        return check_array(c, s, v, quiet);
    case JSON_STRING:
        return check_string(c, s, v, quiet);
    case JSON_INTEGER:
        return check_integer(c, s, v, quiet);
    default:
        return 0;
    }
}

/**
 * Checks `v` against the schema `s`; returns the number of faults, or 1 when
 * `quiet`.
 */
static int check(struct checker *c, const struct json *s, const struct json *v, int quiet)
{
    const struct json *target = deref(c, s);
    const char *type;
    int bad;

    if (s->type == JSON_BOOLEAN) {
        return s->integer ? 0 : fault(c, v, quiet, "is not allowed here");
    }
    if (target == NULL) {
        return fault(c, v, 0, "the schema refers to %s, which it does not define",
                     json_text(s, "$ref"));
    }
    if (target != s) {
        return check(c, target, v, quiet);
    }
    bad = check_keywords(c, s, v);
    type = json_text(s, "type");
    if (type != NULL && !type_is(type, v)) {
        return bad + fault(c, v, quiet, "is %s, where the schema wants %s",
                           with_article(json_type_name(v->type)), with_article(type));
    }
    bad += check_values(c, s, v, quiet);
    if (quiet && bad) {
        return bad;
    }
    bad += check_kind(c, s, v, quiet);
    if (quiet && bad) {
        return bad;
    }
    return bad + check_logic(c, s, v, quiet);
}

/**
 * Checks the document `doc` against `schema`, calling `report` once for each
 * fault with `ctx`. Returns the number of faults reported.
 */
int schema_check(const struct json *schema, const struct json *doc, schema_report report, void *ctx)
{
    struct checker c;

    c.root = schema;
    c.report = report;
    c.ctx = ctx;
    c.path[0] = '\0';
    c.len = 0;
    return check(&c, schema, doc, 0);
}
