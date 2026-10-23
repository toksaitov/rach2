/* Check etude history storage, clocks and accounting under damaged input.
 * host-inputs: tools/etude/schemas/history.schema.json
 * host-inputs: tools/etude/schemas/history.example.jsonl
 * host-inputs: tools/etude/schemas/history.incomplete.example.jsonl
 * host-deps: tools/etude/history.c tools/etude/history_json.c
 * host-deps: tools/etude/history_collect.c utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude
 */
#include <etude/history.h>
#include "history_json.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static char directory[1024];
static char paths[16][1100];
static pid_t children[4];
static const char schema[] = "tools/etude/schemas/history.schema.json";
static const char utc[] = "2026-10-20T00:00:00.000000Z";
static char *examples[5];
static const char *kinds[] = {"plan", "step", "attempt", "run", "comparison"};

#define CHECK(test, detail)                                                  \
    do {                                                                     \
        if (!(test)) {                                                       \
            fprintf(stderr, "etude_history_hosttest: failed: %s\n", detail); \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/** Reap owned children before removing every fixture, even after failure. */
static void cleanup(void)
{
    size_t i;
    for (i = 0; i < 4; i++)
        if (children[i] > 0) {
            kill(children[i], SIGTERM);
            waitpid(children[i], NULL, 0);
        }
    for (i = 0; i < 16; i++)
        if (*paths[i])
            unlink(paths[i]);
    if (*directory)
        rmdir(directory);
    for (i = 0; i < 5; i++)
        free(examples[i]);
}

/** Exit through the fixture cleanup after a signal. */
static void stopped(int number)
{
    exit(128 + number);
}

/** Create private paths after installing cleanup. */
static void fixtures(void)
{
    const char *base = getenv("TMPDIR");
    size_t i;
    if (!base || !*base)
        base = "/tmp";
    atexit(cleanup);
    signal(SIGTERM, stopped);
    signal(SIGINT, stopped);
    signal(SIGHUP, stopped);
    if (snprintf(directory, sizeof(directory), "%s/rach2-etude-history.XXXXXX", base) >=
            (int)sizeof(directory) ||
        !mkdtemp(directory))
        exit(1);
    for (i = 0; i < 16; i++)
        snprintf(paths[i], sizeof(paths[i]), "%s/%u.jsonl", directory, (unsigned)i);
}

/** Read the exact data bytes of each schema example. */
static void load_examples(void)
{
    FILE *file = fopen("tools/etude/schemas/history.example.jsonl", "rb");
    char line[32768];
    if (!file)
        exit(1);
    while (fgets(line, sizeof(line), file)) {
        struct eh_json *record = eh_json_parse(line);
        const struct eh_json *kind = eh_json_get(record, "kind");
        char *data = strstr(line, "\"data\":");
        size_t i, length;
        if (!kind || !data)
            exit(1);
        data += 7;
        length = strlen(data);
        if (length < 2 || data[length - 1] != '\n' || data[length - 2] != '}')
            exit(1);
        data[length - 2] = 0;
        for (i = 0; i < 5; i++)
            if (!examples[i] && !strcmp(kind->string, kinds[i])) {
                examples[i] = strdup(data);
                if (!examples[i])
                    exit(1);
            }
        eh_json_free(record);
    }
    fclose(file);
    for (size_t i = 0; i < 5; i++)
        if (!examples[i])
            exit(1);
}

/** Count validated callback invocations. */
static int visited(const char *line, void *context)
{
    unsigned *count = context;
    struct eh_json *record = eh_json_parse(line);
    CHECK(record != NULL, "callback receives valid JSON");
    eh_json_free(record);
    (*count)++;
    return 0;
}

/** Append a data object with the next stream identity. */
static int append(struct etude_history *history, unsigned kind, const char *data)
{
    char id[256];
    if (etude_history_record_id(history, id, sizeof(id)))
        return -1;
    return etude_history_append(history, id, utc, kinds[kind], data);
}

/** Make one replacement to produce a malformed record. */
static char *replace(const char *source, const char *old, const char *new_text)
{
    const char *at = strstr(source, old);
    size_t prefix, length;
    char *result;
    if (!at || strstr(at + strlen(old), old))
        exit(1);
    prefix = (size_t)(at - source);
    length = strlen(source) - strlen(old) + strlen(new_text);
    result = malloc(length + 1);
    if (!result)
        exit(1);
    memcpy(result, source, prefix);
    strcpy(result + prefix, new_text);
    strcpy(result + prefix + strlen(new_text), at + strlen(old));
    return result;
}

/** Accept all record kinds while refusing invalid shapes without advancing. */
static void store_records(void)
{
    struct etude_history *h = etude_history_open(paths[0], schema, "writer", "run", 1000000);
    uint64_t bad;
    unsigned count = 0;
    size_t i, n;
    char *changed;
    CHECK(h != NULL, "create stream");
    if (!h)
        return;
    CHECK(etude_history_open(paths[0], schema, "other", "run", 1000000) == NULL && errno == EEXIST,
          "existing stream cannot be reopened or replaced");
    CHECK(append(h, 0, "{}") == -1, "missing plan fields refused");
    CHECK(append(h, 0, "{\"x\":0,\"\\u0078\":1}") == -1, "escaped duplicate members refused");
    CHECK(etude_history_append(h, "wrong", utc, "plan", examples[0]) == -1,
          "record identity allocated before execution");
    CHECK(etude_history_append(h, "writer/0", "2026-02-30T00:00:00Z", "plan", examples[0]) == -1,
          "invalid calendar date refused");
    changed = replace(examples[0], "\"purpose\":\"focused\"", "\"purpose\":\"phase_first_run\"");
    CHECK(append(h, 0, changed) == -1, "first run without its exemption refused");
    free(changed);
    changed = replace(examples[1], "\"elapsed_us\":{\"value\":1000000",
                      "\"elapsed_us\":{\"value\":1000001");
    CHECK(append(h, 1, changed) == -1, "inconsistent elapsed time refused");
    free(changed);
    changed = replace(examples[1], "\"cleanup\":\"confirmed\"", "\"cleanup\":\"unconfirmed\"");
    CHECK(append(h, 1, changed) == -1, "pass without cleanup refused");
    free(changed);
    changed = replace(examples[1], "\"end_us\":1100000", "\"end_us\":null");
    CHECK(append(h, 1, changed) == -1, "unobserved end cannot be a measured pass");
    free(changed);
    changed = replace(examples[1], "\"item_id\":\"example-item\"", "\"item_id\":null");
    CHECK(append(h, 1, changed) == -1, "shared preparation requires both null identities");
    free(changed);
    changed =
        replace(examples[0], "\"size_bytes\":18,\"locations\":[\"example://one-case-selection\"]",
                "\"size_bytes\":9007199254740992,\"locations\":[\"example://one-case-selection\"]");
    CHECK(append(h, 0, changed) == -1, "53-bit overflow refused");
    free(changed);
    for (i = 0; i < 5; i++)
        CHECK(append(h, (unsigned)i, examples[i]) == 0, "append each history kind");
    n = strlen(examples[1]) + 3;
    changed = malloc(n);
    if (!changed)
        exit(1);
    snprintf(changed, n, "\n%s\n", examples[1]);
    CHECK(append(h, 1, changed) == 0, "pretty JSON is compacted to one line");
    free(changed);
    CHECK(etude_history_close(h) == 0, "close stream");
    CHECK(etude_history_read(paths[0], schema, visited, &count, &bad) == 0 && count == 6 &&
              bad == 0,
          "all committed records survive rejected input");
    count = 0;
    CHECK(etude_history_read("tools/etude/schemas/history.example.jsonl", schema, visited, &count,
                             &bad) == 0 &&
              count == 7,
          "opaque IDs in existing complete examples");
    CHECK(etude_history_read("tools/etude/schemas/history.incomplete.example.jsonl", schema, NULL,
                             NULL, &bad) == 0,
          "incomplete outcomes are valid stored evidence");
}

/** Write exact fixture bytes, including partial records. */
static void write_bytes(const char *path, const char *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    if (!file || fwrite(data, 1, size, file) != size || fclose(file))
        exit(1);
}

/** Check interrupted tails, interior damage and conflicting identities. */
static void recovery(void)
{
    FILE *file = fopen(paths[0], "rb");
    char line[32768];
    size_t length, cut;
    uint64_t bad;
    char *changed;
    unsigned count;
    if (!file || !fgets(line, sizeof(line), file))
        exit(1);
    fclose(file);
    length = strlen(line);
    for (cut = 1; cut < length; cut += 97) {
        write_bytes(paths[1], line, cut);
        count = 0;
        CHECK(etude_history_read(paths[1], schema, visited, &count, &bad) == 1 && bad == 1 &&
                  count == 0,
              "every interrupted prefix is lost data");
    }
    write_bytes(paths[1], line, length - 1);
    CHECK(etude_history_read(paths[1], schema, NULL, NULL, &bad) == 1,
          "complete object without newline is not committed");
    write_bytes(paths[1], line, length);
    file = fopen(paths[1], "ab");
    if (!file)
        exit(1);
    fputs("{\"interrupted\":", file);
    fclose(file);
    count = 0;
    CHECK(etude_history_read(paths[1], schema, visited, &count, &bad) == 1 && bad == 2 &&
              count == 1,
          "complete prefix does not hide lost last record");
    file = fopen(paths[1], "ab");
    if (!file)
        exit(1);
    fputc('\n', file);
    fputs(line, file);
    fclose(file);
    CHECK(etude_history_read(paths[1], schema, NULL, NULL, &bad) == -1 && bad == 2,
          "interior damage invalidates the stream");
    write_bytes(paths[1], line, length);
    file = fopen(paths[1], "ab");
    if (!file)
        exit(1);
    fputs(line, file);
    fclose(file);
    count = 0;
    CHECK(etude_history_read(paths[1], schema, visited, &count, &bad) == 0 && count == 1,
          "exact replay does not become another sample");
    changed = replace(line, "\"sequence\":0", "\"sequence\":1");
    write_bytes(paths[1], changed, strlen(changed));
    CHECK(etude_history_read(paths[1], schema, NULL, NULL, &bad) == -1,
          "stream sequence must begin at zero");
    free(changed);
    write_bytes(paths[1], line, length);
    changed = replace(line, "\"sequence\":0", "\"sequence\":1");
    file = fopen(paths[1], "ab");
    if (!file)
        exit(1);
    fputs(changed, file);
    fclose(file);
    free(changed);
    CHECK(etude_history_read(paths[1], schema, NULL, NULL, &bad) == -1 && bad == 2,
          "duplicate record ID cannot become another sample");
    changed = replace(line, "\"schema_version\":1", "\"schema_version\":2");
    write_bytes(paths[1], changed, strlen(changed));
    CHECK(etude_history_read(paths[1], schema, NULL, NULL, &bad) == -1,
          "unsupported schema version refused");
    free(changed);
}

/** Reap a registered child without leaving a reused PID in the cleanup list. */
static int reap(unsigned index)
{
    int status;
    if (waitpid(children[index], &status, 0) != children[index])
        return -1;
    children[index] = 0;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/** Run separate writers together, and race exclusive creation of one path. */
static void concurrent(void)
{
    int gate[2];
    unsigned i;
    uint64_t bad;
    struct etude_history *parent = etude_history_open(paths[2], schema, "parent", "run", 100000);
    if (!parent || pipe(gate))
        exit(1);
    for (i = 0; i < 2; i++) {
        children[i] = fork();
        if (children[i] < 0)
            exit(1);
        if (!children[i]) {
            char byte, id[20];
            struct etude_history *h;
            close(gate[1]);
            if (read(gate[0], &byte, 1) != 1)
                _exit(1);
            close(gate[0]);
            if (append(parent, 0, examples[0]) != -1)
                _exit(2);
            snprintf(id, sizeof id, "child%u", i);
            h = etude_history_open(paths[3 + i], schema, id, "run", 100000);
            if (!h || append(h, 0, examples[0]) || append(h, 1, examples[1]) ||
                etude_history_close(h))
                _exit(3);
            h = etude_history_open(paths[5], schema, id, "run", 100000);
            if (!h)
                _exit(errno == EEXIST ? 10 : 4);
            if (append(h, 0, examples[0]) || etude_history_close(h))
                _exit(5);
            _exit(0);
        }
    }
    close(gate[0]);
    if (write(gate[1], "xx", 2) != 2)
        exit(1);
    close(gate[1]);
    {
        int a = reap(0), b = reap(1);
        CHECK((a == 0 && b == 10) || (a == 10 && b == 0), "one exclusive writer wins");
    }
    CHECK(append(parent, 0, examples[0]) == 0, "parent stream survives refused child write");
    CHECK(etude_history_close(parent) == 0, "close parent stream");
    for (i = 2; i <= 5; i++)
        CHECK(etude_history_read(paths[i], schema, NULL, NULL, &bad) == 0,
              "concurrent private streams remain valid");
}

/** A full stream and failed write preserve evidence and cannot be continued. */
static void limits(void)
{
    struct etude_history *h = etude_history_open(paths[6], schema, "bounded", "run", 1);
    uint64_t bad;
    CHECK(h && append(h, 0, examples[0]) == -1 && errno == ENOSPC,
          "retention limit refuses whole record before writing");
    etude_history_close(h);
    children[0] = fork();
    if (children[0] < 0)
        exit(1);
    if (!children[0]) {
        struct rlimit limit;
        signal(SIGXFSZ, SIG_IGN);
        limit.rlim_cur = limit.rlim_max = 128;
        if (setrlimit(RLIMIT_FSIZE, &limit))
            _exit(1);
        h = etude_history_open(paths[7], schema, "failed", "run", 100000);
        if (!h || append(h, 0, examples[0]) != -1 || append(h, 0, examples[0]) != -1)
            _exit(2);
        etude_history_close(h);
        _exit(0);
    }
    CHECK(reap(0) == 0, "I/O failure poisons writer");
    CHECK(etude_history_read(paths[7], schema, NULL, NULL, &bad) == 1,
          "failed write is retained as a lost tail");
}

/** Check missing data, overflow, CPU scope and simultaneous memory. */
static void measurements(void)
{
    struct etude_accounting account;
    struct etude_measurement values[2], result;
    struct etude_reading start, end;
    struct rusage usage;
    char json[4096];
    memset(&usage, 0, sizeof(usage));
    usage.ru_utime.tv_sec = 2;
    usage.ru_stime.tv_usec = 123;
    usage.ru_maxrss = 1234567;
    account = etude_history_wait_usage(&usage);
    CHECK(account.cpu.value == 2000123 && !strcmp(account.cpu.quality, "lower_bound") &&
              !account.cpu.error_known && account.memory.reason,
          "wait CPU is bounded by reaped descendants");
    CHECK(etude_measurement_json(&account.memory, json, sizeof(json)) == 0 &&
              strstr(json, "\"value\":null"),
          "unavailable memory is not written as zero");
    values[0] = values[1] = account.cpu;
    result = etude_history_cpu_sum(values, 2);
    CHECK(!result.reason && result.value == 4000246 && !strcmp(result.quality, "lower_bound"),
          "disjoint CPU sum keeps omissions");
    values[1].reason = "lost descendant";
    CHECK(etude_history_cpu_sum(values, 2).reason != NULL, "missing CPU cannot be added as zero");
    values[1] = account.cpu;
    values[1].value = ETUDE_HISTORY_MAX;
    CHECK(etude_history_cpu_sum(values, 2).reason != NULL, "CPU sum overflow unavailable");
    etude_history_accounting_init(&account);
    memset(values, 0, sizeof(values));
    values[0].quality = values[1].quality = "exact";
    values[0].value = 100;
    values[1].value = 200;
    etude_history_memory_sample(&account.memory, values, 2, 10);
    values[0].value = 250;
    values[1].value = 1;
    etude_history_memory_sample(&account.memory, values, 2, 20);
    CHECK(!account.memory.reason && account.memory.value == 300 &&
              account.memory.interval_us == 20 && !account.memory.error_known &&
              !strcmp(account.memory.quality, "sampled"),
          "combined peak is not sum of separate peaks");
    values[0].reason = "process disappeared";
    etude_history_memory_sample(&account.memory, values, 2, 20);
    values[0].reason = NULL;
    etude_history_memory_sample(&account.memory, values, 2, 20);
    CHECK(account.memory.reason != NULL, "lost resident sample stays unavailable");
    CHECK(etude_history_now(&start) == 0 && etude_history_now(&end) == 0,
          "UTC and monotonic clocks available");
    result = etude_history_elapsed(&start, &end);
    CHECK(!result.reason && result.value == end.monotonic_us - start.monotonic_us &&
              result.error_known,
          "elapsed time uses monotonic clock");
    CHECK(etude_history_elapsed(&start, NULL).reason != NULL, "missing end stays unavailable");
    start.monotonic_us = 10;
    end.monotonic_us = 9;
    CHECK(etude_history_elapsed(&start, &end).reason != NULL, "backwards clock stays unavailable");
    memset(&result, 0, sizeof(result));
    result.method = "measured-zero-v1";
    result.quality = "exact";
    result.error_known = 1;
    CHECK(etude_measurement_json(&result, json, sizeof(json)) == 0 && strstr(json, "\"value\":0"),
          "measured zero stays zero");
    result.reason = "lost \"sample\"\n";
    CHECK(etude_measurement_json(&result, json, sizeof(json)) == 0, "missing reason is escaped");
    {
        struct eh_json *parsed = eh_json_parse(json);
        CHECK(parsed != NULL, "escaped measurement is JSON");
        eh_json_free(parsed);
    }
    CHECK(etude_measurement_json(&result, json, 2) == -1, "small output buffer refused");
}

/** Reject malformed JSON and parse every incomplete prefix under sanitizers. */
static void decoder(void)
{
    static const char *bad[] = {"{",
                                "[",
                                "{\"x\"",
                                "{\"x\":",
                                "{\"x\":0,}",
                                "[1,]",
                                "01",
                                "1.5",
                                "1e3",
                                "true false",
                                "{\"x\":1,\"x\":2}",
                                "\"\\u0000\"",
                                "\"\\u0080\"",
                                "\"\\uD800\"",
                                "\"\\q\"",
                                "\"unterminated",
                                "9007199254740992"};
    size_t i, length;
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        struct eh_json *parsed = eh_json_parse(bad[i]);
        CHECK(parsed == NULL, "malformed JSON refused");
        eh_json_free(parsed);
    }
    length = strlen(examples[1]);
    for (i = 1; i < length; i++) {
        char *prefix = malloc(i + 1);
        struct eh_json *parsed;
        if (!prefix)
            exit(1);
        memcpy(prefix, examples[1], i);
        prefix[i] = 0;
        parsed = eh_json_parse(prefix);
        CHECK(parsed == NULL, "incomplete JSON object refused");
        eh_json_free(parsed);
        free(prefix);
    }
}

/** Exercise every storage failure with private fixtures and no guest boots. */
int main(void)
{
    fixtures();
    load_examples();
    decoder();
    store_records();
    recovery();
    concurrent();
    limits();
    measurements();
    return failures ? 1 : 0;
}
