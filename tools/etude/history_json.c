/* history_json.c: strict ASCII JSON and validation of history record shapes. */
#include "history_json.h"
#include <stdlib.h>
#include <string.h>

struct parser {
    const char *cursor;
    size_t nodes; /* allocation bound, independent of nesting */
};

/** Free a value, its members and the following siblings. */
void eh_json_free(struct eh_json *value)
{
    while (value) {
        struct eh_json *next = value->next;
        eh_json_free(value->child);
        free(value->key);
        free(value->string);
        free(value);
        value = next;
    }
}

/** Skip the four JSON whitespace characters. */
static void space(struct parser *parser)
{
    while (*parser->cursor && strchr(" \t\r\n", *parser->cursor))
        parser->cursor++;
}

/** Decode a string; embedded NUL and non-ASCII bytes are refused. */
static char *string(struct parser *parser)
{
    const char *start;
    char *result, *out;
    if (*parser->cursor++ != '"')
        return NULL;
    start = parser->cursor;
    while (*parser->cursor && *parser->cursor != '"') {
        if (*parser->cursor == '\\') {
            parser->cursor++;
            if (!*parser->cursor)
                return NULL;
        }
        parser->cursor++;
    }
    if (!*parser->cursor)
        return NULL;
    result = malloc((size_t)(parser->cursor - start) + 1);
    if (!result)
        return NULL;
    out = result;
    while (start < parser->cursor) {
        unsigned c = (unsigned char)*start++;
        if (c < 32 || c > 127)
            goto bad;
        if (c == '\\') {
            const char *escapes = "\"\\/bfnrt";
            const char *decoded = "\"\\/\b\f\n\r\t";
            const char *found;
            c = (unsigned char)*start++;
            if (c == 'u') {
                unsigned i;
                c = 0;
                for (i = 0; i < 4; i++) {
                    const char *digit, *hex;
                    if (start >= parser->cursor)
                        goto bad;
                    hex =
                        (*start >= 'A' && *start <= 'F') ? "0123456789ABCDEF" : "0123456789abcdef";
                    digit = strchr(hex, *start);
                    if (!digit)
                        goto bad;
                    c = c * 16 + (unsigned)(digit - hex);
                    start++;
                }
                if (c == 0 || c > 127)
                    goto bad;
            } else {
                found = strchr(escapes, (int)c);
                if (!found)
                    goto bad;
                c = (unsigned char)decoded[found - escapes];
            }
        }
        *out++ = (char)c;
    }
    *out = 0;
    parser->cursor++;
    return result;
bad:
    free(result);
    return NULL;
}

static struct eh_json *value(struct parser *parser, unsigned depth);

/** Read an object or array into an already allocated value. */
static int collection(struct parser *parser, struct eh_json *node, unsigned depth)
{
    char end = *parser->cursor == '{' ? '}' : ']';
    struct eh_json **tail = &node->child;
    node->type = end == '}' ? 'o' : 'a';
    parser->cursor++;
    space(parser);
    if (*parser->cursor == end) {
        parser->cursor++;
        return 1;
    }
    for (;;) {
        char *key = NULL;
        struct eh_json *member;
        space(parser);
        if (node->type == 'o') {
            const struct eh_json *previous;
            if (*parser->cursor != '"' || !(key = string(parser)))
                return 0;
            for (previous = node->child; previous; previous = previous->next)
                if (!strcmp(previous->key, key)) {
                    free(key);
                    return 0;
                }
            space(parser);
            if (*parser->cursor != ':') {
                free(key);
                return 0;
            }
            parser->cursor++;
        }
        member = value(parser, depth + 1);
        if (!member) {
            free(key);
            return 0;
        }
        member->key = key;
        *tail = member;
        tail = &member->next;
        space(parser);
        if (*parser->cursor == end) {
            parser->cursor++;
            return 1;
        }
        if (*parser->cursor != ',')
            return 0;
        parser->cursor++;
    }
}

/** Read an integer without overflow or a noncanonical leading zero. */
static int integer(struct parser *parser, struct eh_json *node)
{
    uint64_t number = 0;
    int negative = *parser->cursor == '-';
    const char *start;
    parser->cursor += negative;
    start = parser->cursor;
    if (*start < '0' || *start > '9')
        return 0;
    while (*parser->cursor >= '0' && *parser->cursor <= '9') {
        unsigned digit = (unsigned)(*parser->cursor++ - '0');
        if (number > (EH_MAX_INTEGER - digit) / 10)
            return 0;
        number = number * 10 + digit;
    }
    if (parser->cursor - start > 1 && *start == '0')
        return 0;
    node->type = 'n';
    node->number = negative ? -(int64_t)number : (int64_t)number;
    return 1;
}

/** Read one bounded JSON value and its members. */
static struct eh_json *value(struct parser *parser, unsigned depth)
{
    struct eh_json *node;
    int valid;
    if (depth > 64 || ++parser->nodes > EH_MAX_LINE / sizeof(*node))
        return NULL;
    space(parser);
    node = calloc(1, sizeof(*node));
    if (!node)
        return NULL;
    switch (*parser->cursor) {
    case '{':
    case '[':
        valid = collection(parser, node, depth);
        break;
    case '"':
        node->type = 's';
        node->string = string(parser);
        valid = node->string != NULL;
        break;
    case 't':
    case 'f':
    case 'n': {
        const char *word = *parser->cursor == 't'   ? "true"
                           : *parser->cursor == 'f' ? "false"
                                                    : "null";
        node->type = *word == 'n' ? 'z' : *word;
        valid = !strncmp(parser->cursor, word, strlen(word));
        if (valid)
            parser->cursor += strlen(word);
        break;
    }
    default:
        valid = integer(parser, node);
        break;
    }
    if (!valid) {
        eh_json_free(node);
        return NULL;
    }
    return node;
}

/** Parse a complete string, refusing trailing bytes and oversized input. */
struct eh_json *eh_json_parse(const char *text)
{
    struct parser parser;
    struct eh_json *result;
    if (!text || strlen(text) > EH_MAX_LINE)
        return NULL;
    parser.cursor = text;
    parser.nodes = 0;
    result = value(&parser, 0);
    space(&parser);
    if (*parser.cursor) {
        eh_json_free(result);
        return NULL;
    }
    return result;
}

/** Look up a member without accepting an array as an object. */
const struct eh_json *eh_json_get(const struct eh_json *object, const char *key)
{
    const struct eh_json *member;
    if (!object || object->type != 'o')
        return NULL;
    for (member = object->child; member; member = member->next)
        if (!strcmp(member->key, key))
            return member;
    return NULL;
}

/** Test the restricted alphabet and length of an identity. */
int eh_identity(const char *text)
{
    size_t i;
    if (!text || !*text || strlen(text) > 255)
        return 0;
    for (i = 0; text[i]; i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            continue;
        if (!i || !strchr("._:/-", c))
            return 0;
    }
    return 1;
}

/** Read a fixed decimal field, or return -1 for a non-digit. */
static int decimal(const char *text, unsigned count)
{
    int result = 0;
    while (count--) {
        if (*text < '0' || *text > '9')
            return -1;
        result = result * 10 + *text++ - '0';
    }
    return result;
}

/** Check a Gregorian calendar date, including leap years. */
static int calendar_date(const char *text)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int year = decimal(text, 4), month = decimal(text + 5, 2);
    int day = decimal(text + 8, 2), limit;
    if (year < 1 || month < 1 || month > 12)
        return 0;
    limit = days[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    return day >= 1 && day <= limit;
}

/** Check the UTC spelling and the Gregorian calendar date. */
int eh_utc(const char *text)
{
    size_t length = strlen(text), i;
    int hour, minute, second;
    if (length < 20 || length > 27 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[length - 1] != 'Z' || !calendar_date(text))
        return 0;
    hour = decimal(text + 11, 2);
    minute = decimal(text + 14, 2);
    second = decimal(text + 17, 2);
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59)
        return 0;
    if (length > 20) {
        if (length < 22 || text[19] != '.')
            return 0;
        for (i = 20; i < length - 1; i++)
            if (text[i] < '0' || text[i] > '9')
                return 0;
    }
    return 1;
}

/** Compare JSON values independently of the order of object members. */
static int equal(const struct eh_json *a, const struct eh_json *b)
{
    const struct eh_json *x, *y;
    size_t na = 0, nb = 0;
    if (!a || !b || a->type != b->type)
        return 0;
    if (a->type == 's')
        return !strcmp(a->string, b->string);
    if (a->type == 'n')
        return a->number == b->number;
    y = b->child;
    for (x = a->child; x; x = x->next) {
        if (a->type == 'o')
            y = eh_json_get(b, x->key);
        if (!equal(x, y))
            return 0;
        if (a->type == 'a')
            y = y->next;
        na++;
    }
    for (y = b->child; y; y = y->next)
        nb++;
    return na == nb;
}

/** Match the patterns used by the version-one history schema. */
static int pattern(const char *text, const char *expression)
{
    size_t length = strlen(text);
    if (!strcmp(expression, "^[A-Za-z0-9][A-Za-z0-9._:/-]{0,254}$"))
        return eh_identity(text);
    if (!strcmp(expression, "^[0-9a-f]{64}$"))
        return length == 64 && strspn(text, "0123456789abcdef") == length;
    if (!strcmp(expression, "^[0-9a-f]{40}([0-9a-f]{24})?$"))
        return (length == 40 || length == 64) && strspn(text, "0123456789abcdef") == length;
    if (!strcmp(expression,
                "^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\\.[0-9]{1,6})?Z$"))
        return eh_utc(text);
    return 0;
}

/** Resolve a local JSON Pointer whose components have no escapes. */
static const struct eh_json *reference(const struct eh_json *root, const char *ref)
{
    char key[128];
    if (strncmp(ref, "#/", 2))
        return NULL;
    ref += 2;
    while (*ref && root) {
        size_t length = strcspn(ref, "/");
        if (!length || length >= sizeof(key))
            return NULL;
        memcpy(key, ref, length);
        key[length] = 0;
        if (root->type == 'a') {
            int index = decimal(key, (unsigned)length);
            root = root->child;
            if (index < 0)
                return NULL;
            while (root && index--)
                root = root->next;
        } else {
            root = eh_json_get(root, key);
        }
        ref += length;
        if (*ref == '/')
            ref++;
    }
    return root;
}

static int validate(const struct eh_json *v, const struct eh_json *s, const struct eh_json *root,
                    unsigned depth);

/** Check type and literal values. */
static int literal_rules(const struct eh_json *v, const struct eh_json *s)
{
    const struct eh_json *rule, *member;
    rule = eh_json_get(s, "type");
    if (rule) {
        char type = rule->type == 's' ? (!strcmp(rule->string, "integer") ? 'n'
                                         : !strcmp(rule->string, "null")  ? 'z'
                                                                          : rule->string[0])
                                      : 0;
        if (!type || v->type != type)
            return 0;
    }
    rule = eh_json_get(s, "const");
    if (rule && !equal(v, rule))
        return 0;
    rule = eh_json_get(s, "enum");
    if (rule) {
        for (member = rule->child; member; member = member->next)
            if (equal(v, member))
                break;
        if (!member)
            return 0;
    }
    return 1;
}

/** Check number and string restrictions. */
static int scalar_rules(const struct eh_json *v, const struct eh_json *s)
{
    const struct eh_json *rule;
    if (v->type == 'n') {
        rule = eh_json_get(s, "minimum");
        if (rule && v->number < rule->number)
            return 0;
        rule = eh_json_get(s, "maximum");
        if (rule && v->number > rule->number)
            return 0;
    }
    if (v->type == 's') {
        rule = eh_json_get(s, "minLength");
        if (rule && strlen(v->string) < (uint64_t)rule->number)
            return 0;
        rule = eh_json_get(s, "pattern");
        if (rule && !pattern(v->string, rule->string))
            return 0;
        rule = eh_json_get(s, "format");
        if (rule && (strcmp(rule->string, "date-time") || !eh_utc(v->string)))
            return 0;
    }
    return 1;
}

/** Check required members and each declared property of an object. */
static int object_rules(const struct eh_json *v, const struct eh_json *s,
                        const struct eh_json *root, unsigned depth)
{
    const struct eh_json *rule, *member;
    if (v->type == 'o') {
        const struct eh_json *properties = eh_json_get(s, "properties");
        const struct eh_json *additional = eh_json_get(s, "additionalProperties");
        rule = eh_json_get(s, "required");
        for (member = rule ? rule->child : NULL; member; member = member->next)
            if (!eh_json_get(v, member->string))
                return 0;
        for (member = v->child; member; member = member->next) {
            rule = eh_json_get(properties, member->key);
            if (rule && !validate(member, rule, root, depth + 1))
                return 0;
            if (!rule && additional && additional->type == 'f')
                return 0;
        }
    }
    return 1;
}

/** Check array members, uniqueness and length. */
static int array_rules(const struct eh_json *v, const struct eh_json *s, const struct eh_json *root,
                       unsigned depth)
{
    const struct eh_json *rule, *member, *other;
    size_t count = 0;
    if (v->type == 'a') {
        const struct eh_json *items = eh_json_get(s, "items");
        const struct eh_json *unique = eh_json_get(s, "uniqueItems");
        for (member = v->child; member; member = member->next) {
            count++;
            if (items && !validate(member, items, root, depth + 1))
                return 0;
            if (unique && unique->type == 't')
                for (other = member->next; other; other = other->next)
                    if (equal(member, other))
                        return 0;
        }
        rule = eh_json_get(s, "minItems");
        if (rule && count < (uint64_t)rule->number)
            return 0;
    }
    return 1;
}

/** Check unions, intersections and conditional schemas. */
static int combined_rules(const struct eh_json *v, const struct eh_json *s,
                          const struct eh_json *root, unsigned depth)
{
    const struct eh_json *rule, *member;
    size_t count;
    rule = eh_json_get(s, "allOf");
    for (member = rule ? rule->child : NULL; member; member = member->next)
        if (!validate(v, member, root, depth + 1))
            return 0;
    rule = eh_json_get(s, "oneOf");
    if (rule) {
        count = 0;
        for (member = rule->child; member; member = member->next)
            count += validate(v, member, root, depth + 1) != 0;
        if (count != 1)
            return 0;
    }
    rule = eh_json_get(s, "if");
    if (rule) {
        rule = eh_json_get(s, validate(v, rule, root, depth + 1) ? "then" : "else");
        if (rule && !validate(v, rule, root, depth + 1))
            return 0;
    }
    return 1;
}

/** Apply the schema vocabulary used by history.schema.json. */
static int validate(const struct eh_json *v, const struct eh_json *s, const struct eh_json *root,
                    unsigned depth)
{
    const struct eh_json *rule;
    if (!v || !s || s->type != 'o' || depth > 128)
        return 0;
    rule = eh_json_get(s, "$ref");
    if (rule && (rule->type != 's' || !validate(v, reference(root, rule->string), root, depth + 1)))
        return 0;
    return literal_rules(v, s) && scalar_rules(v, s) && object_rules(v, s, root, depth) &&
           array_rules(v, s, root, depth) && combined_rules(v, s, root, depth);
}

/** Validate a record with a trusted version-one history schema. */
int eh_json_validate(const struct eh_json *record, const struct eh_json *schema)
{
    return validate(record, schema, schema, 0);
}
