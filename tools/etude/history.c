/* history.c: exclusive JSON Lines streams, validated before durable writes. */
#define _POSIX_C_SOURCE 200809L
#include "include/etude/history.h"
#include "history_json.h"
#include "../../utils/sha256.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#define open      _open
#define close     _close
#define fdopen    _fdopen
#define fileno    _fileno
#define sync_file _commit
#define owner_pid _getpid
#define EH_BINARY (_O_BINARY | _O_NOINHERIT)
#else
#include <unistd.h>
#define sync_file fsync
#define owner_pid getpid
#define EH_BINARY O_CLOEXEC
#endif

struct etude_history {
    FILE *file;
    struct eh_json *schema;
    char stream[256];
    char run[256];
    uint64_t sequence;
    uint64_t bytes;
    uint64_t limit;
    long owner;
    int poisoned;
};

/** Read the trusted schema once, with the same size limit as records. */
static struct eh_json *load_schema(const char *path)
{
    FILE *file = fopen(path, "rb");
    struct eh_json *schema = NULL;
    char *text;
    size_t size;
    if (!file)
        return NULL;
    text = malloc(EH_MAX_LINE + 1);
    if (text) {
        size = fread(text, 1, EH_MAX_LINE, file);
        if (!ferror(file) && feof(file)) {
            text[size] = 0;
            if (!memchr(text, 0, size))
                schema = eh_json_parse(text);
        }
        free(text);
    }
    fclose(file);
    if (!schema || !eh_json_get(schema, "$defs")) {
        eh_json_free(schema);
        errno = EINVAL;
        return NULL;
    }
    return schema;
}

/** Allocate a stream before publishing its exclusively created file. */
struct etude_history *etude_history_open(const char *path, const char *schema_path,
                                         const char *stream_id, const char *run_id,
                                         uint64_t limit_bytes)
{
    struct etude_history *h;
    int fd;
    if (!eh_identity(stream_id) || strlen(stream_id) > 237 || !eh_identity(run_id) ||
        !limit_bytes || limit_bytes > EH_MAX_INTEGER) {
        errno = EINVAL;
        return NULL;
    }
    h = calloc(1, sizeof(*h));
    if (!h)
        return NULL;
    h->schema = load_schema(schema_path);
    if (!h->schema) {
        free(h);
        return NULL;
    }
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | EH_BINARY, 0600);
    if (fd < 0) {
        eh_json_free(h->schema);
        free(h);
        return NULL;
    }
    h->file = fdopen(fd, "wb");
    if (!h->file) {
        int saved = errno;
        close(fd);
        eh_json_free(h->schema);
        free(h);
        errno = saved;
        return NULL;
    }
    strcpy(h->stream, stream_id);
    strcpy(h->run, run_id);
    h->limit = limit_bytes;
    h->owner = (long)owner_pid();
    return h;
}

/** Format an identity whose uniqueness follows from the stream allocator. */
int etude_history_record_id(const struct etude_history *h, char *out, size_t size)
{
    int length;
    if (!h || h->sequence >= 65536) {
        errno = EOVERFLOW;
        return -1;
    }
    length = snprintf(out, size, "%s/%llu", h->stream, (unsigned long long)h->sequence);
    if (length < 0 || (size_t)length >= size) {
        errno = EOVERFLOW;
        return -1;
    }
    return 0;
}

/** Validate schema and local arithmetic that cannot depend on other streams. */
static int valid_record(const struct eh_json *record, const struct eh_json *schema)
{
    const struct eh_json *data, *time, *start, *end, *elapsed;
    const struct eh_json *item, *attempt, *result, *outcome, *cleanup;
    if (!eh_json_validate(record, schema))
        return 0;
    data = eh_json_get(record, "data");
    time = eh_json_get(data, "time");
    if (time) {
        start = eh_json_get(time, "start_us");
        end = eh_json_get(time, "end_us");
        elapsed = eh_json_get(eh_json_get(time, "elapsed_us"), "value");
        if (end->type == 'n' &&
            (end->number < start->number || elapsed->number != end->number - start->number))
            return 0;
        result = eh_json_get(data, "result");
        outcome = eh_json_get(result, "outcome");
        cleanup = eh_json_get(result, "cleanup");
        if (end->type == 'z' && !strcmp(outcome->string, "pass"))
            return 0;
        if (!strcmp(eh_json_get(record, "kind")->string, "run") &&
            !strcmp(cleanup->string, "unconfirmed") && end->type != 'z')
            return 0;
    }
    item = eh_json_get(data, "item_id");
    attempt = eh_json_get(data, "attempt_id");
    if (item && attempt && (item->type == 'z') != (attempt->type == 'z'))
        return 0;
    return 1;
}

/** Remove JSON whitespace outside strings after validation. */
static size_t compact(char *line)
{
    char *read = line, *write = line;
    int quoted = 0, escaped = 0;
    while (*read) {
        char c = *read++;
        if (quoted || !strchr(" \t\r\n", c))
            *write++ = c;
        if (quoted && !escaped && c == '\\') {
            escaped = 1;
            continue;
        }
        if (!escaped && c == '"')
            quoted = !quoted;
        escaped = 0;
    }
    *write = 0;
    return (size_t)(write - line);
}

/** Commit object bytes before a newline, poisoning a failed stream. */
static int commit_record(struct etude_history *h, const char *line, size_t length)
{
    if (length + 1 > h->limit - h->bytes) {
        errno = ENOSPC;
        return -1;
    }
    if (fwrite(line, 1, length, h->file) != length || fflush(h->file) ||
        sync_file(fileno(h->file)) || fputc('\n', h->file) == EOF || fflush(h->file) ||
        sync_file(fileno(h->file))) {
        h->poisoned = 1;
        return -1;
    }
    h->sequence++;
    h->bytes += length + 1;
    return 0;
}

/** Commit a validated object with a separately flushed newline. */
int etude_history_append(struct etude_history *h, const char *record_id, const char *utc,
                         const char *kind, const char *data)
{
    char expected[256], *line;
    struct eh_json *record;
    size_t capacity, length;
    int n;
    if (!h || h->poisoned || h->owner != (long)owner_pid()) {
        errno = EIO;
        return -1;
    }
    if (etude_history_record_id(h, expected, sizeof(expected)) < 0)
        return -1;
    if (!record_id || strcmp(record_id, expected) || !utc || !eh_utc(utc) || !kind ||
        !eh_identity(kind) || !data || strlen(data) > EH_MAX_LINE - 2048) {
        errno = EINVAL;
        return -1;
    }
    capacity = strlen(data) + 2048;
    line = malloc(capacity);
    if (!line)
        return -1;
    n = snprintf(line, capacity,
                 "{\"schema_version\":1,\"record_id\":\"%s\",\"stream_id\":\"%s\","
                 "\"sequence\":%llu,\"run_id\":\"%s\",\"recorded_utc\":\"%s\","
                 "\"kind\":\"%s\",\"data\":%s}",
                 record_id, h->stream, (unsigned long long)h->sequence, h->run, utc, kind, data);
    if (n < 0 || (size_t)n >= capacity) {
        free(line);
        errno = EOVERFLOW;
        return -1;
    }
    record = eh_json_parse(line);
    if (!valid_record(record, h->schema)) {
        eh_json_free(record);
        free(line);
        errno = EINVAL;
        return -1;
    }
    eh_json_free(record);
    length = compact(line);
    n = commit_record(h, line, length);
    free(line);
    return n;
}

/** Release the stream without deleting its evidence, even on failure. */
int etude_history_close(struct etude_history *h)
{
    int status;
    if (!h)
        return 0;
    status = fclose(h->file);
    eh_json_free(h->schema);
    free(h);
    return status == 0 ? 0 : -1;
}

struct stored_record {
    char *id;
    unsigned char digest[32];
    size_t length;
};

struct stream_order {
    char stream[256];
    char run[256];
    uint64_t sequence;
    size_t id_count;
    struct stored_record *records;
    unsigned *slots;
};

/** Hash exact record bytes, apart from the framing newline. */
static void record_digest(const char *line, size_t length, unsigned char digest[32])
{
    struct sha256 sha;
    sha256_init(&sha);
    sha256_update(&sha, (const unsigned char *)line, length);
    sha256_final(&sha, digest);
}

/** Return 1 for a new record, 2 for an exact replay, or 0 on conflict. */
static int ordered_record(const struct eh_json *record, struct stream_order *order,
                          const char *line, size_t length)
{
    const struct eh_json *s = eh_json_get(record, "stream_id");
    const struct eh_json *r = eh_json_get(record, "run_id");
    const struct eh_json *id = eh_json_get(record, "record_id");
    size_t slot;
    uint64_t hash = UINT64_C(14695981039346656037);
    const unsigned char *byte;
    unsigned char digest[32];
    struct stored_record *stored;
    if (!order->sequence) {
        strcpy(order->stream, s->string);
        strcpy(order->run, r->string);
    }
    if (strcmp(s->string, order->stream) || strcmp(r->string, order->run))
        return 0;
    record_digest(line, length, digest);
    for (byte = (const unsigned char *)id->string; *byte; byte++)
        hash = (hash ^ *byte) * UINT64_C(1099511628211);
    slot = (size_t)(hash & 131071);
    while (order->slots[slot]) {
        stored = &order->records[order->slots[slot] - 1];
        if (!strcmp(stored->id, id->string))
            return stored->length == length && !memcmp(stored->digest, digest, 32) ? 2 : 0;
        slot = (slot + 1) & 131071;
    }
    if (order->id_count == 65536 ||
        eh_json_get(record, "sequence")->number != (int64_t)order->sequence)
        return 0;
    stored = &order->records[order->id_count];
    stored->id = malloc(strlen(id->string) + 1);
    if (!stored->id)
        return 0;
    strcpy(stored->id, id->string);
    memcpy(stored->digest, digest, 32);
    stored->length = length;
    order->slots[slot] = (unsigned)++order->id_count;
    order->sequence++;
    return 1;
}

/** Validate complete lines, retaining a distinct result for a partial tail. */
int etude_history_read(const char *path, const char *schema_path,
                       int (*visit)(const char *, void *), void *context, uint64_t *bad_line)
{
    struct eh_json *schema = load_schema(schema_path), *record = NULL;
    FILE *file = NULL;
    char *line = NULL;
    size_t length = 0;
    struct stream_order order = {{0}, {0}, 0, 0, NULL, NULL};
    int c, status = -1;
    *bad_line = 1;
    if (!schema)
        goto done;
    file = fopen(path, "rb");
    line = malloc(EH_MAX_LINE + 1);
    order.slots = calloc(131072, sizeof(*order.slots));
    order.records = calloc(65536, sizeof(*order.records));
    if (!file || !line || !order.slots || !order.records)
        goto done;
    while ((c = fgetc(file)) != EOF) {
        int ordered;
        if (c != '\n') {
            if (!c || length == EH_MAX_LINE)
                goto done;
            line[length++] = (char)c;
            continue;
        }
        line[length] = 0;
        record = eh_json_parse(line);
        if (!valid_record(record, schema))
            goto done;
        ordered = ordered_record(record, &order, line, length);
        if (!ordered)
            goto done;
        if (ordered == 1 && visit && visit(line, context))
            goto done;
        eh_json_free(record);
        record = NULL;
        (*bad_line)++;
        length = 0;
    }
    if (ferror(file))
        goto done;
    status = length ? 1 : 0;
    if (!status)
        *bad_line = 0;
done:
    while (order.id_count)
        free(order.records[--order.id_count].id);
    free(order.records);
    free(order.slots);
    eh_json_free(record);
    eh_json_free(schema);
    free(line);
    if (file && fclose(file))
        status = -1;
    if (status < 0)
        errno = EINVAL;
    return status;
}
