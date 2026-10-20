/* history_json.h: the bounded JSON reader used by the history store. */
#ifndef ETUDE_HISTORY_JSON_H
#define ETUDE_HISTORY_JSON_H
#include <stddef.h>
#include <stdint.h>

#define EH_MAX_INTEGER UINT64_C(9007199254740991)
#define EH_MAX_LINE    (4U * 1024U * 1024U)

struct eh_json {
    char type; /* o object, a array, s string, n int, t true, f false, z null */
    char *key;
    char *string;
    int64_t number;
    struct eh_json *child;
    struct eh_json *next;
};

/** Parse one ASCII JSON value, with unique keys and 53-bit integer values. */
struct eh_json *eh_json_parse(const char *text);
/** Free a parsed value and every descendant. */
void eh_json_free(struct eh_json *value);
/** Return an object's member, or NULL when it is absent. */
const struct eh_json *eh_json_get(const struct eh_json *object, const char *key);
/** Validate a value against the history schema's supported vocabulary. */
int eh_json_validate(const struct eh_json *value, const struct eh_json *schema);
/** Return whether text is a history identity of at most 255 bytes. */
int eh_identity(const char *text);
/** Return whether text is a valid UTC timestamp in the history format. */
int eh_utc(const char *text);
#endif
