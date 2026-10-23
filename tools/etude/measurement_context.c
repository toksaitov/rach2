/* measurement_context.c: hash-bound snapshots and preparation receipts. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#endif
#include "measurement_context.h"
#include "history_json.h"
#include "json.h"
#include "../../utils/sha256.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#ifdef _WIN32
#include "win/files.h"
#define context_sync _commit
#define fdopen       _fdopen
#else
#define context_sync fsync
#endif
#define CONTEXT_MAX (32U * 1024U * 1024U)
#define INPUT_MAX   65536U

struct binding {
    char *path;
    char hash[65];
    uint64_t size;
};
struct etude_measurement_context {
    struct binding *files;
    size_t count;
    char *run_id;
    char *text[10];
    struct {
        char *path;
        char hash[65];
        uint64_t device, inode;
    } observer[2];
    int eligible, has_preparation;
    struct etude_preparation_receipt preparation;
};
static const char *const keys[] = {
    "purpose",     "source",    "artifacts",         "site_profile",         "tools",
    "cache_state", "exemption", "outer_observation", "physical_observation", "preparation"};

static int fail(char *error, size_t size, const char *message)
{
    if (error && size)
        snprintf(error, size, "%s", message);
    errno = EINVAL;
    return -1;
}
static int hex(const char *text, size_t size)
{
    size_t i;
    if (!text || strlen(text) != size)
        return 0;
    for (i = 0; i < size; i++)
        if (!strchr("0123456789abcdef", text[i]))
            return 0;
    return 1;
}
static void token(char *s)
{
    size_t i;
    for (i = 0; s[i]; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '.' && s[i] != '_' && s[i] != '-')
            s[i] = '-';
    if (!isalnum((unsigned char)s[0]))
        s[0] = 'x';
}
int etude_context_clock(struct etude_context_clock *clock)
{
    memset(clock, 0, sizeof(*clock));
#ifdef _WIN32
    DWORD n = sizeof(clock->machine_id);
    if (!GetComputerNameA(clock->machine_id, &n) ||
        etude_win_boot_id(clock->boot_id, sizeof(clock->boot_id)))
        return -1;
#else
    if (gethostname(clock->machine_id, sizeof(clock->machine_id) - 1))
        return -1;
#ifdef __APPLE__
    size_t n = sizeof(clock->boot_id);
    if (sysctlbyname("kern.bootsessionuuid", clock->boot_id, &n, NULL, 0))
        return -1;
#elif defined(__linux__)
    FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
    if (!f)
        return -1;
    int ok = fgets(clock->boot_id, sizeof(clock->boot_id), f) != NULL;
    fclose(f);
    if (!ok)
        return -1;
    clock->boot_id[strcspn(clock->boot_id, "\r\n")] = 0;
#else
    return -1;
#endif
#endif
    clock->boot_id[sizeof(clock->boot_id) - 1] = 0;
    if (!clock->machine_id[0] || !clock->boot_id[0])
        return -1;
    token(clock->machine_id);
    token(clock->boot_id);
    return 0;
}
static int hash_file(const char *path, char hash[65], uint64_t *size)
{
    unsigned char buf[65536], digest[32];
    struct sha256 sha;
    struct stat st;
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    int ok = !fstat(fileno(f), &st) && S_ISREG(st.st_mode);
    sha256_init(&sha);
    *size = 0;
    while (ok && (n = fread(buf, 1, sizeof(buf), f)) != 0) {
        if (*size > ETUDE_HISTORY_MAX - n) {
            ok = 0;
            break;
        }
        *size += n;
        sha256_update(&sha, buf, n);
    }
    ok = ok && !ferror(f);
    if (fclose(f))
        ok = 0;
    if (!ok)
        return -1;
    sha256_final(&sha, digest);
    sha256_hex(digest, hash);
    return 0;
}
static struct json *document(const char *path)
{
    FILE *f = fopen(path, "rb");
    struct json *doc = NULL;
    char *bytes, error[256];
    size_t n;
    if (!f)
        return NULL;
    bytes = malloc(CONTEXT_MAX + 1);
    n = bytes ? fread(bytes, 1, CONTEXT_MAX + 1, f) : 0;
    if (bytes && n <= CONTEXT_MAX && !ferror(f) && !memchr(bytes, 0, n))
        json_parse(bytes, n, 1, &doc, error, sizeof(error));
    free(bytes);
    fclose(f);
    return doc;
}
static char *canonical(const struct json *doc)
{
    struct jbuf b = {0};
    if (doc)
        json_canonical(doc, &b);
    return b.data;
}
static int number(const struct json *o, const char *key, uint64_t *value)
{
    const struct json *v = json_member(o, key);
    if (!v || v->type != JSON_INTEGER || v->integer < 0)
        return -1;
    *value = (uint64_t)v->integer;
    return 0;
}
static int version(const struct json *doc)
{
    uint64_t v;
    return doc && doc->type == JSON_OBJECT && !number(doc, "schema_version", &v) && v == 1;
}
static int bound(struct etude_measurement_context *c, const struct json *v, const char *path)
{
    const char *hash = json_text(v, "sha256");
    char actual[65];
    uint64_t size, actual_size;
    if (!path || !hex(hash, 64) || number(v, "size_bytes", &size) ||
        hash_file(path, actual, &actual_size) || size != actual_size || strcmp(hash, actual) ||
        c->count == INPUT_MAX)
        return -1;
    struct binding *files = realloc(c->files, (c->count + 1) * sizeof(*files));
    if (!files)
        return -1;
    c->files = files;
    struct binding *b = &c->files[c->count];
    b->path = strdup(path);
    if (!b->path)
        return -1;
    strcpy(b->hash, hash);
    b->size = size;
    c->count++;
    return 0;
}
static int absolute(const char *path)
{
#ifdef _WIN32
    return etude_win_absolute(path);
#else
    return path && path[0] == '/';
#endif
}
static int relative(const char *path)
{
    if (!path || !*path || absolute(path) || strchr(path, '\\') || strchr(path, ':'))
        return 0;
    for (const char *p = path; *p;) {
        const char *q = strchr(p, '/');
        size_t n = q ? (size_t)(q - p) : strlen(p);
        if (!n || (n == 1 && p[0] == '.') || (n == 2 && !strncmp(p, "..", 2)))
            return 0;
        p += n;
        if (*p)
            p++;
    }
    return 1;
}
static int inputs(struct etude_measurement_context *c, const struct json *doc, const char *tree,
                  const char *program)
{
    const struct json *a = json_member(doc, "inputs");
    int found = program == NULL;
    if (!a || a->type != JSON_ARRAY || !a->count)
        return -1;
    for (size_t i = 0; i < a->count; i++) {
        const char *p = json_text(a->item[i], "path");
        char path[8192];
        if (!p || (tree ? !relative(p) : !absolute(p)))
            return -1;
        int n = snprintf(path, sizeof(path), "%s%s%s", tree ? tree : "", tree ? "/" : "", p);
        if (n < 0 || (size_t)n >= sizeof(path) || bound(c, a->item[i], path))
            return -1;
        found |= program && !strcmp(p, program);
    }
    return found ? 0 : -1;
}
static struct json *snapshot(struct etude_measurement_context *c, const struct json *root,
                             const char *key)
{
    const struct json *v = json_member(root, key);
    const char *path = json_text(v, "path");
    if (!path || !absolute(path) || bound(c, v, path))
        return NULL;
    struct json *doc = document(path);
    if (!version(doc)) {
        json_free(doc);
        return NULL;
    }
    return doc;
}
static int quote(struct jbuf *b, const char *text)
{
    struct json v;
    if (!text)
        return -1;
    memset(&v, 0, sizeof(v));
    v.type = JSON_STRING;
    v.string = (char *)text;
    json_canonical(&v, b);
    return 0;
}
static int reading(const struct json *doc, struct etude_reading *r)
{
    const char *utc = json_text(doc, "utc");
    if (!utc || !eh_utc(utc) || strlen(utc) >= sizeof(r->utc) ||
        number(doc, "monotonic_us", &r->monotonic_us) ||
        number(doc, "resolution_us", &r->resolution_us) || !r->resolution_us)
        return -1;
    strcpy(r->utc, utc);
    return 0;
}
static void put_reading(struct jbuf *b, const struct etude_reading *r)
{
    char numbers[180];
    jbuf_puts(b, "{\"utc\":");
    quote(b, r->utc);
    snprintf(numbers, sizeof(numbers), ",\"monotonic_us\":%llu,\"resolution_us\":%llu}",
             (unsigned long long)r->monotonic_us, (unsigned long long)r->resolution_us);
    jbuf_puts(b, numbers);
}
static int create_document(const char *path, struct jbuf *b)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600), ok = 0;
    FILE *f = fd < 0 ? NULL : fdopen(fd, "wb");
    if (f) {
        ok = fwrite(b->data, 1, b->len, f) == b->len && !fflush(f) && !context_sync(fd);
        if (fclose(f))
            ok = 0;
    } else if (fd >= 0)
        close(fd);
    jbuf_free(b);
    return ok ? 0 : -1;
}
static int valid_receipt(const struct etude_preparation_receipt *r)
{
    return r && r->passed && r->cleanup_confirmed && eh_identity(r->run_id) && hex(r->commit, 40) &&
           hex(r->record_sha256, 64) && eh_identity(r->clock.machine_id) &&
           eh_identity(r->clock.boot_id) && eh_utc(r->start.utc) && eh_utc(r->end.utc) &&
           r->start.resolution_us && r->end.resolution_us &&
           r->end.monotonic_us >= r->start.monotonic_us && r->end.monotonic_us <= ETUDE_HISTORY_MAX;
}
int etude_context_receipt_write(const char *path, const struct etude_preparation_receipt *r)
{
    struct jbuf b = {0};
    if (!valid_receipt(r)) {
        errno = EINVAL;
        return -1;
    }
    jbuf_puts(&b, "{\"schema_version\":1,\"format\":\"etude-preparation-receipt\",\"outcome\":"
                  "\"pass\",\"cleanup\":\"confirmed\",\"run_id\":");
    quote(&b, r->run_id);
    jbuf_puts(&b, ",\"commit\":");
    quote(&b, r->commit);
    jbuf_puts(&b, ",\"record_sha256\":");
    quote(&b, r->record_sha256);
    jbuf_puts(&b, ",\"machine_id\":");
    quote(&b, r->clock.machine_id);
    jbuf_puts(&b, ",\"boot_id\":");
    quote(&b, r->clock.boot_id);
    jbuf_puts(&b, ",\"start\":");
    put_reading(&b, &r->start);
    jbuf_puts(&b, ",\"end\":");
    put_reading(&b, &r->end);
    jbuf_puts(&b, "}\n");
    return create_document(path, &b);
}
static int copy_field(const struct json *doc, const char *key, char *out, size_t size)
{
    const char *v = json_text(doc, key);
    if (!v || strlen(v) >= size)
        return -1;
    strcpy(out, v);
    return 0;
}
int etude_context_grant_write(const char *path, const char *claim,
                              const struct etude_preparation_grant *g)
{
    struct jbuf b = {0};
    if (!absolute(path) || !eh_identity(claim) || !g || !eh_identity(g->clock.machine_id) ||
        !eh_identity(g->clock.boot_id) || !eh_utc(g->start.utc) || !g->start.resolution_us ||
        g->start.monotonic_us > ETUDE_HISTORY_MAX) {
        errno = EINVAL;
        return -1;
    }
    jbuf_puts(&b, "{\"schema_version\":1,\"format\":\"etude-preparation-grant\",\"claim_id\":");
    quote(&b, claim);
    jbuf_puts(&b, ",\"machine_id\":");
    quote(&b, g->clock.machine_id);
    jbuf_puts(&b, ",\"boot_id\":");
    quote(&b, g->clock.boot_id);
    jbuf_puts(&b, ",\"start\":");
    put_reading(&b, &g->start);
    jbuf_puts(&b, "}\n");
    return create_document(path, &b);
}
int etude_context_grant_read(const char *path, const char *claim, struct etude_preparation_grant *g)
{
    struct etude_context_clock clock;
    struct etude_reading now;
    struct stat st;
    if (!g || !absolute(path) || !eh_identity(claim) || lstat(path, &st) || !S_ISREG(st.st_mode) ||
        st.st_size > 4096) {
        errno = EINVAL;
        return -1;
    }
    memset(g, 0, sizeof *g);
    struct json *doc = document(path);
    const char *format = json_text(doc, "format"), *id = json_text(doc, "claim_id");
    int ok = version(doc) && format && !strcmp(format, "etude-preparation-grant") && id &&
             !strcmp(id, claim) &&
             !copy_field(doc, "machine_id", g->clock.machine_id, sizeof g->clock.machine_id) &&
             !copy_field(doc, "boot_id", g->clock.boot_id, sizeof g->clock.boot_id) &&
             !reading(json_member(doc, "start"), &g->start) && !etude_context_clock(&clock) &&
             !etude_history_now(&now) && !strcmp(clock.machine_id, g->clock.machine_id) &&
             !strcmp(clock.boot_id, g->clock.boot_id) && g->start.monotonic_us <= now.monotonic_us;
    json_free(doc);
    if (!ok)
        errno = EINVAL;
    return ok ? 0 : -1;
}
static int receipt_fields(const struct json *doc, struct etude_preparation_receipt *r)
{
    const char *outcome = json_text(doc, "outcome"), *cleanup = json_text(doc, "cleanup");
    const char *format = json_text(doc, "format");
    return doc && format && !strcmp(format, "etude-preparation-receipt") && outcome && cleanup &&
           !strcmp(outcome, "pass") && !strcmp(cleanup, "confirmed") &&
           !copy_field(doc, "run_id", r->run_id, sizeof(r->run_id)) &&
           !copy_field(doc, "commit", r->commit, sizeof(r->commit)) &&
           !copy_field(doc, "record_sha256", r->record_sha256, sizeof(r->record_sha256)) &&
           !copy_field(doc, "machine_id", r->clock.machine_id, sizeof(r->clock.machine_id)) &&
           !copy_field(doc, "boot_id", r->clock.boot_id, sizeof(r->clock.boot_id)) &&
           !reading(json_member(doc, "start"), &r->start) &&
           !reading(json_member(doc, "end"), &r->end);
}
static int preparation(struct etude_measurement_context *c, const struct json *root,
                       const char *commit, const char *record_hash)
{
    const struct json *v = json_member(root, "preparation");
    c->text[9] = strdup("null");
    if (!v || v->type == JSON_NULL)
        return 0;
    struct json *doc = snapshot(c, root, "preparation");
    struct etude_preparation_receipt *r = &c->preparation;
    struct etude_context_clock clock;
    struct etude_reading now;
    int ok = receipt_fields(doc, r);
    r->passed = r->cleanup_confirmed = ok;
    ok = ok && valid_receipt(r) && !strcmp(c->run_id, r->run_id) && !strcmp(commit, r->commit) &&
         !strcmp(record_hash, r->record_sha256) && !etude_context_clock(&clock) &&
         !strcmp(clock.machine_id, r->clock.machine_id) &&
         !strcmp(clock.boot_id, r->clock.boot_id) && !etude_history_now(&now) &&
         r->end.monotonic_us <= now.monotonic_us;
    if (ok) {
        r->elapsed = etude_history_elapsed(&r->start, &r->end);
        r->same_run = c->has_preparation = 1;
        ok = r->elapsed.reason == NULL;
        if (ok) {
            free(c->text[9]);
            c->text[9] = canonical(doc);
        }
    }
    json_free(doc);
    return ok ? 0 : -1;
}
static int source_tools(struct etude_measurement_context *c, const struct json *root,
                        const char *tree, const char *commit, const char *program)
{
    struct json *source = snapshot(c, root, "source"), *tools = snapshot(c, root, "tools");
    const char *got = json_text(source, "commit");
    int ok = source && tools && got && hex(commit, 40) && !strcmp(got, commit) &&
             !inputs(c, source, tree, NULL) && !inputs(c, tools, NULL, program);
    if (ok) {
        c->text[1] = canonical(source);
        c->text[4] = canonical(tools);
    }
    json_free(source);
    json_free(tools);
    return ok ? 0 : -1;
}
static int site_cache(struct etude_measurement_context *c, const struct json *root,
                      const char *site)
{
    struct json *s = snapshot(c, root, "site_profile"), *cache = snapshot(c, root, "cache_state");
    const struct json *policy = json_member(s, "policy");
    const struct json *controlled = json_member(cache, "controlled");
    const char *method = json_text(cache, "method");
    int ok = s && cache && site && !bound(c, policy, site);
    if (ok) {
        c->text[3] = canonical(s);
        c->text[5] = canonical(cache);
        c->eligible = controlled && controlled->type == JSON_BOOLEAN && controlled->integer &&
                      method && *method;
    }
    json_free(s);
    json_free(cache);
    return ok ? 0 : -1;
}
static int artifacts(struct etude_measurement_context *c, const struct json *root,
                     const char *record)
{
    const struct json *v = json_member(root, "artifacts");
    const char *path = json_text(v, "path");
    struct jbuf b = {0};
    char size[80];
    if (!path || strcmp(path, record) || bound(c, v, record))
        return -1;
    jbuf_puts(&b, "[{\"sha256\":");
    quote(&b, json_text(v, "sha256"));
    snprintf(size, sizeof(size), ",\"size_bytes\":%llu,\"locations\":[",
             (unsigned long long)c->files[c->count - 1].size);
    jbuf_puts(&b, size);
    quote(&b, record);
    jbuf_puts(&b, "]}]");
    c->text[2] = b.data;
    return preparation(c, root, json_text(root, "commit"), json_text(v, "sha256"));
}
/* Only the immutable first line is hashed while the sampler writes. */
static struct json *observer_header(const char *path, char hash[65], struct stat *st)
{
    FILE *f = fopen(path, "rb");
    struct json *doc = NULL;
    char line[65536], error[256];
    if (!f)
        return NULL;
    if (!fstat(fileno(f), st) && S_ISREG(st->st_mode) && fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        if (n && line[n - 1] == '\n' && !json_parse(line, n, 1, &doc, error, sizeof(error))) {
            struct sha256 sha;
            unsigned char digest[32];
            sha256_init(&sha);
            sha256_update(&sha, (unsigned char *)line, n);
            sha256_final(&sha, digest);
            sha256_hex(digest, hash);
        }
    }
    fclose(f);
    return doc;
}
static int decimal(const char *s)
{
    return s && *s && strspn(s, "0123456789") == strlen(s);
}
static int observer_start(const struct json *h, const char *run_id)
{
    const char *kind = json_text(h, "kind"), *method = json_text(h, "method");
    const char *run = json_text(h, "run_id"), *platform = json_text(h, "platform");
    const char *machine = json_text(h, "machine_id"), *boot = json_text(h, "boot_id");
    uint64_t root, observer, interval, limit;
    if (!version(h) || !kind || strcmp(kind, "start") || !method ||
        strcmp(method, "native-load-v1") || !run || strcmp(run, run_id) || !platform ||
        !eh_identity(machine) || !eh_identity(boot) || number(h, "root_pid", &root) || !root ||
        number(h, "observer_pid", &observer) || !observer ||
        number(h, "requested_interval_us", &interval) || interval != 1000000 ||
        number(h, "process_limit", &limit) || limit != 4096 ||
        !decimal(json_text(h, "root_creation")) || !decimal(json_text(h, "observer_creation")))
        return -1;
    return 0;
}
static int observer_identity(const struct json *h, const char *run_id, int physical)
{
    if (observer_start(h, run_id))
        return -1;
    const char *platform = json_text(h, "platform");
    const char *machine = json_text(h, "machine_id"), *boot = json_text(h, "boot_id");
    if (physical)
        return strcmp(platform, "windows-native-counters-v1") ? -1 : 0;
    struct etude_context_clock clock;
#ifdef _WIN32
    const char *expected = "windows-native-counters-v1";
#elif defined(__APPLE__)
    const char *expected = "macos-mach-libproc-v1";
#else
    const char *expected = "linux-proc-counters-v1";
#endif
    return etude_context_clock(&clock) || strcmp(platform, expected) ||
                   strcmp(machine, clock.machine_id) || strcmp(boot, clock.boot_id)
               ? -1
               : 0;
}
static int observation(struct etude_measurement_context *c, const struct json *root, int physical)
{
    size_t index = (size_t)physical;
    const struct json *v = json_member(root, keys[7 + index]);
    c->text[7 + index] = v ? canonical(v) : strdup("null");
    if (!v || v->type == JSON_NULL)
        return 0;
    const char *path = json_text(v, "trace"), *stop = json_text(v, "stop_request");
    const char *hash = json_text(v, "header_sha256");
    struct stat st, unused;
    char actual[65];
    if (!path || !stop || !absolute(path) || !absolute(stop) || !hex(hash, 64) ||
        lstat(stop, &unused) == 0 || errno != ENOENT)
        return -1;
    struct json *header = observer_header(path, actual, &st);
    int ok = header && !strcmp(hash, actual) && !observer_identity(header, c->run_id, physical);
    if (ok) {
        c->observer[index].path = strdup(path);
        strcpy(c->observer[index].hash, hash);
        c->observer[index].device = (uint64_t)st.st_dev;
        c->observer[index].inode = (uint64_t)st.st_ino;
        ok = c->observer[index].path != NULL;
    }
    json_free(header);
    return ok ? 0 : -1;
}
static int context_header(struct etude_measurement_context *c, const struct json *doc,
                          const char *commit)
{
    const char *id = json_text(doc, "run_id"), *purpose = json_text(doc, "purpose");
    const char *got = json_text(doc, "commit");
    const char *format = json_text(doc, "format");
    int ok = version(doc) && format && !strcmp(format, "etude-measurement-context") &&
             eh_identity(id) && purpose && got && commit && !strcmp(got, commit);
    if (ok) {
        c->run_id = strdup(id);
        struct jbuf b = {0};
        quote(&b, purpose);
        c->text[0] = b.data;
        ok = !strcmp(purpose, "push") || !strcmp(purpose, "ci") || !strcmp(purpose, "release") ||
             !strcmp(purpose, "focused") || !strcmp(purpose, "phase_first_run");
        const struct json *exemption = json_member(doc, "exemption");
        c->text[6] = exemption ? canonical(exemption) : strdup("null");
        ok = ok && c->run_id;
    }
    return ok;
}
int etude_context_open(const char *path, const struct etude_context_inputs *inputs,
                       struct etude_measurement_context **out, char *error, size_t size)
{
    *out = NULL;
    struct etude_measurement_context *c = calloc(1, sizeof(*c));
    char original_hash[65];
    uint64_t original_size;
    int original_ok = !hash_file(path, original_hash, &original_size);
    struct json *doc = original_ok ? document(path) : NULL;
    const char *purpose = json_text(doc, "purpose");
    int ok = c && original_ok && context_header(c, doc, inputs->commit) &&
             !source_tools(c, doc, inputs->tree, inputs->commit, inputs->program) &&
             !site_cache(c, doc, inputs->site) && !artifacts(c, doc, inputs->record) &&
             !observation(c, doc, 0) && !observation(c, doc, 1);
    if (ok) {
        struct json *check = NULL;
        char e[128];
        /* Check the exemption with the history schema when the plan is built. */
        ok = !json_parse(c->text[6], strlen(c->text[6]), 1, &check, e, sizeof(e)) &&
             (!strcmp(purpose, "phase_first_run") ? check->type == JSON_OBJECT
                                                  : check->type == JSON_NULL);
        json_free(check);
    }
    json_free(doc);
    if (ok) {
        struct binding *files = realloc(c->files, (c->count + 1) * sizeof(*files));
        if (!files)
            ok = 0;
        else {
            c->files = files;
            struct binding *b = &files[c->count];
            b->path = strdup(path);
            strcpy(b->hash, original_hash);
            b->size = original_size;
            if (b->path)
                c->count++;
            else
                ok = 0;
        }
        ok = ok && !etude_context_recheck(c, error, size);
    }
    if (!ok) {
        etude_context_close(c);
        return fail(error, size, "measurement context or a bound input is invalid");
    }
    *out = c;
    return 0;
}
const char *etude_context_text(const struct etude_measurement_context *c, const char *key)
{
    if (c)
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            if (!strcmp(key, keys[i]))
                return c->text[i];
    return NULL;
}
const char *etude_context_run_id(const struct etude_measurement_context *c)
{
    return c ? c->run_id : NULL;
}
const struct etude_preparation_receipt *
etude_context_preparation(const struct etude_measurement_context *c)
{
    return c && c->has_preparation ? &c->preparation : NULL;
}
int etude_context_eligible(const struct etude_measurement_context *c)
{
    return c && c->eligible && c->observer[0].path;
}
int etude_context_recheck(const struct etude_measurement_context *c, char *error, size_t size)
{
    if (!c)
        return 0;
    for (size_t i = 0; i < c->count; i++) {
        char hash[65];
        uint64_t bytes;
        if (hash_file(c->files[i].path, hash, &bytes) || bytes != c->files[i].size ||
            strcmp(hash, c->files[i].hash))
            return fail(error, size, "a measurement input changed during the run");
    }
    for (size_t i = 0; i < 2; i++) {
        if (!c->observer[i].path)
            continue;
        struct stat st;
        char hash[65];
        struct json *header = observer_header(c->observer[i].path, hash, &st);
        int ok = header && !strcmp(hash, c->observer[i].hash) &&
                 (uint64_t)st.st_dev == c->observer[i].device &&
                 (uint64_t)st.st_ino == c->observer[i].inode;
        json_free(header);
        if (!ok)
            return fail(error, size, "an outer observation header changed during the run");
    }
    return 0;
}
void etude_context_close(struct etude_measurement_context *c)
{
    if (!c)
        return;
    for (size_t i = 0; i < c->count; i++)
        free(c->files[i].path);
    free(c->files);
    free(c->run_id);
    for (size_t i = 0; i < 2; i++)
        free(c->observer[i].path);
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
        free(c->text[i]);
    free(c);
}
