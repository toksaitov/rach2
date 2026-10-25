/* Stable retained files, compatible registry locking and atomic updates. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include "win/files.h"
#else
#include <sys/file.h>
#include <sys/stat.h>
#include <poll.h>
#endif
#include "retention.h"
#include "etude/exec.h"
#include "measurement_context.h"
#include "routine_proof.h"

static char retention_error[4096];

int ret_fail(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vsnprintf(retention_error, sizeof retention_error, format, ap);
    va_end(ap);
    return -1;
}
const char *ret_error(void)
{
    return retention_error;
}

char *ret_format(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int size = vsnprintf(NULL, 0, format, ap);
    va_end(ap);
    char *out = size < 0 ? NULL : json_alloc((size_t)size + 1);
    if (out) {
        va_start(ap, format);
        vsnprintf(out, (size_t)size + 1, format, ap);
        va_end(ap);
    }
    return out;
}

int ret_absolute(const char *path)
{
#ifdef _WIN32
    return etude_win_absolute(path);
#else
    return path && *path == '/';
#endif
}

#ifdef _WIN32
/* The native handle gives the stored spelling, including aliased drive case. */
static char *ret_native_path(const char *path)
{
    wchar_t *wide = etude_win_long_path(path);
    HANDLE file = wide ? CreateFileW(wide, FILE_READ_ATTRIBUTES,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                     OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL)
                       : INVALID_HANDLE_VALUE;
    free(wide);
    if (file == INVALID_HANDLE_VALUE) {
        etude_win_file_error();
        return NULL;
    }
    DWORD size = GetFinalPathNameByHandleW(file, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    wide = size ? json_alloc(((size_t)size + 1) * sizeof(*wide)) : NULL;
    DWORD count = wide ? GetFinalPathNameByHandleW(file, wide, size + 1,
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS)
                       : 0;
    char *out = NULL;
    if (count && count <= size) {
        if (!wcsncmp(wide, L"\\\\?\\UNC\\", 8)) {
            char *tail = etude_win_utf8(wide + 8);
            out = tail ? ret_format("//%s", tail) : NULL;
            free(tail);
        } else if (!wcsncmp(wide, L"\\\\?\\", 4))
            out = etude_win_utf8(wide + 4);
    }
    CloseHandle(file);
    free(wide);
    if (out)
        for (char *p = out; *p; p++)
            if (*p == '\\')
                *p = '/';
    return out;
}
#endif

/* Normalize future outputs through existing ancestors without making them. */
char *ret_path(const char *path, int existing)
{
    if (!path || !*path || strpbrk(path, "\r\n")) {
        ret_fail("invalid retained path");
        return NULL;
    }
#ifdef _WIN32
    char *resolved = ret_native_path(path);
#else
    char *resolved = realpath(path, NULL);
#endif
    if (resolved || existing || errno != ENOENT)
        return resolved;
    char *copy = strdup(path), *slash;
    if (!copy)
        return NULL;
    slash = strrchr(copy, '/');
#ifdef _WIN32
    char *back = strrchr(copy, '\\');
    if (back && (!slash || back > slash))
        slash = back;
#endif
    char *name = strdup(slash ? slash + 1 : copy);
    if (slash)
        *slash = 0;
    if (!name || !*name || !strcmp(name, ".") || !strcmp(name, "..")) {
        free(copy);
        free(name);
        return NULL;
    }
    char *parent = ret_path(slash ? (*copy ? copy : "/") : ".", 0);
    resolved = parent ? ret_format("%s/%s", parent, name) : NULL;
    free(parent);
    free(copy);
    free(name);
    return resolved;
}

int ret_inside(const char *root, const char *path)
{
    size_t n = strlen(root);
    return !strncmp(root, path, n) && path[n] == '/';
}

int ret_dirs(const char *path)
{
    char *copy = strdup(path);
    if (!copy)
        return -1;
    int rc = 0;
    for (char *p = copy + (copy[0] && copy[1] == ':' ? 3 : 1);; p++) {
        if (*p && *p != '/' && *p != '\\')
            continue;
        char saved = *p;
        *p = 0;
        struct stat st;
        if ((mkdir(copy, 0700) && errno != EEXIST) || lstat(copy, &st) || !S_ISDIR(st.st_mode))
            rc = -1;
        *p = saved;
        if (rc || !saved)
            break;
    }
    free(copy);
    return rc;
}

struct json *ret_new(enum json_type type)
{
    struct json *value = json_alloc(sizeof(*value));
    value->type = type;
    return value;
}
struct json *ret_string(const char *text)
{
    struct json *value = ret_new(JSON_STRING);
    value->string = strdup(text);
    return value;
}
struct json *ret_integer(int64_t n)
{
    struct json *value = ret_new(JSON_INTEGER);
    value->integer = n;
    return value;
}
struct json *ret_boolean(int yes)
{
    struct json *value = ret_new(JSON_BOOLEAN);
    value->integer = yes != 0;
    return value;
}
int ret_push(struct json *array, struct json *value)
{
    if (!array || array->type != JSON_ARRAY || !value)
        return ret_fail("invalid retained array");
    struct json **items = realloc(array->item, (array->count + 1) * sizeof(*items));
    if (!items)
        return -1;
    array->item = items;
    array->item[array->count++] = value;
    return 0;
}
int ret_set(struct json *object, const char *key, struct json *value)
{
    if (!object || object->type != JSON_OBJECT || !value)
        return ret_fail("invalid retained object");
    value->name = strdup(key);
    for (size_t i = 0; i < object->count; i++)
        if (!strcmp(object->item[i]->name, key)) {
            json_free(object->item[i]);
            object->item[i] = value;
            return 0;
        }
    struct json **items = realloc(object->item, (object->count + 1) * sizeof(*items));
    if (!items)
        return -1;
    object->item = items;
    object->item[object->count++] = value;
    return 0;
}
struct json *ret_clone(const struct json *value)
{
    struct jbuf text = {0};
    struct json *copy = NULL;
    char error[256];
    if (value) {
        json_canonical(value, &text);
        if (json_parse_data(text.data, text.len, 1, &copy, error, sizeof error))
            ret_fail("retained JSON cannot be copied: %s", error);
    }
    jbuf_free(&text);
    return copy;
}
int ret_has(const struct json *array, const char *text)
{
    if (array && array->type == JSON_ARRAY)
        for (size_t i = 0; i < array->count; i++)
            if (array->item[i]->type == JSON_STRING && !strcmp(array->item[i]->string, text))
                return 1;
    return 0;
}
int ret_add_name(struct json *array, const char *text)
{
    return ret_has(array, text) ? 0 : ret_push(array, ret_string(text));
}
int64_t ret_number(const struct json *object, const char *name)
{
    const struct json *value = json_member(object, name);
    return value && value->type == JSON_INTEGER ? value->integer : -1;
}

struct json *ret_document(const char *path)
{
    FILE *file = path ? fopen(path, "rb") : NULL;
    if (!file) {
        ret_fail("retained document unavailable: %s", path ? path : "(null)");
        return NULL;
    }
    struct jbuf bytes = {0};
    char block[65536], error[256];
    size_t size;
    int failed = 0;
    while ((size = fread(block, 1, sizeof block, file))) {
        if (bytes.len > (64u << 20) - size) {
            failed = 1;
            break;
        }
        jbuf_put(&bytes, block, size);
    }
    failed |= ferror(file);
    if (fclose(file))
        failed = 1;
    struct json *value = NULL;
    if (failed ||
        json_parse_data(bytes.data ? bytes.data : "", bytes.len, 1, &value, error, sizeof error))
        ret_fail("unreadable retained JSON: %s", path);
    jbuf_free(&bytes);
    return value;
}

int ret_write(const char *path, const struct json *value, int replace)
{
    char *temporary = ret_format("%s.retention-XXXXXX", path);
    int fd = temporary ? mkstemp(temporary) : -1;
    if (fd < 0) {
        free(temporary);
        return ret_fail("retained output cannot be created: %s", path);
    }
    struct jbuf bytes = {0};
    json_canonical(value, &bytes);
    jbuf_puts(&bytes, "\n");
    FILE *file = fdopen(fd, "wb");
    int failed = !file;
    if (file) {
        failed = fwrite(bytes.data, 1, bytes.len, file) != bytes.len || fflush(file) || fsync(fd);
        if (fclose(file))
            failed = 1;
    } else
        close(fd);
    if (!failed && (replace ? rename(temporary, path) : link(temporary, path)))
        failed = 1;
    if (unlink(temporary) && errno != ENOENT)
        failed = 1;
    free(temporary);
    jbuf_free(&bytes);
    return failed ? ret_fail("retained output cannot be published: %s", path) : 0;
}

struct json *ret_descriptor(const char *given)
{
    struct stat st;
    char digest[65];
    if (!given || lstat(given, &st) || !S_ISREG(st.st_mode)) {
        ret_fail("not a regular retained file: %s", given ? given : "(null)");
        return NULL;
    }
    char *path = ret_path(given, 1);
    if (!path || etude_context_digest(path, &st, digest)) {
        free(path);
        ret_fail("retained file changed or cannot be read: %s", given);
        return NULL;
    }
    struct json *value = ret_new(JSON_OBJECT);
    ret_set(value, "path", ret_string(path));
    ret_set(value, "sha256", ret_string(digest));
    ret_set(value, "size_bytes", ret_integer(st.st_size));
    free(path);
    return value;
}

char *ret_checked(const struct json *ref)
{
    const char *given = json_text(ref, "path"), *hash = json_text(ref, "sha256");
    struct json *actual = given ? ret_descriptor(given) : NULL;
    const char *digest = json_text(actual, "sha256");
    char *path = actual && hash && digest && !strcmp(hash, digest) &&
                         ret_number(actual, "size_bytes") == ret_number(ref, "size_bytes")
                     ? strdup(json_text(actual, "path"))
                     : NULL;
    if (!path)
        ret_fail("retained reference changed or is unavailable: %s", given ? given : "(null)");
    json_free(actual);
    return path;
}

/* Current age uses whole seconds rounded down. Recorded use rounds upward. */
int64_t ret_now(void)
{
    return (int64_t)time(NULL);
}
void ret_stamp(char out[64])
{
    time_t now = time(NULL);
    struct tm utc;
    if (!gmtime_r(&now, &utc) || !strftime(out, 64, "%Y-%m-%dT%H:%M:%SZ", &utc))
        *out = 0;
}

/* Gregorian civil days from 1970-01-01; no locale or local time enters. */
static int64_t ret_days(int year, int month, int day)
{
    year -= month <= 2;
    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned y = (unsigned)(year - era * 400);
    unsigned d = (153u * (unsigned)(month + (month > 2 ? -3 : 9)) + 2) / 5 + (unsigned)day - 1;
    return (int64_t)era * 146097 + y * 365 + y / 4 - y / 100 + d - 719468;
}
static int ret_date(const char *text, int values[6])
{
    int end = 0;
    if (!text || strlen(text) < 20 ||
        sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d%n", values, values + 1, values + 2, values + 3,
               values + 4, values + 5, &end) != 6 ||
        end != 19)
        return -1;
    for (size_t i = 0; i < 19; i++) {
        char expected = i == 4 || i == 7 ? '-' : i == 10 ? 'T' : i == 13 || i == 16 ? ':' : 0;
        if (expected ? text[i] != expected : text[i] < '0' || text[i] > '9')
            return -1;
    }
    return 0;
}
static int ret_calendar(const int values[6])
{
    int y = values[0], m = values[1], d = values[2];
    if (y < 1970 || m < 1 || m > 12 || d < 1 || values[3] > 23 || values[4] > 59 || values[5] > 59)
        return -1;
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int limit = days[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    return d > limit ? -1 : 0;
}
int64_t ret_seconds(const char *text)
{
    int values[6];
    if (ret_date(text, values) || ret_calendar(values))
        return -1;
    const char *tail = text + 19;
    int fraction = 0;
    if (*tail == '.') {
        const char *start = ++tail;
        while (*tail >= '0' && *tail <= '9') {
            fraction |= *tail != '0';
            tail++;
        }
        if (start == tail)
            return -1;
    }
    if (strcmp(tail, "Z") && strcmp(tail, "+00:00"))
        return -1;
    return ret_days(values[0], values[1], values[2]) * 86400 + values[3] * 3600 + values[4] * 60 +
           values[5] + fraction;
}

int64_t ret_timestamp_us(const char *text)
{
    int64_t seconds = ret_seconds(text);
    if (seconds < 0 || seconds > INT64_MAX / 1000000)
        return -1;
    const char *at = text + 19;
    unsigned fraction = 0, digits = 0;
    int nonzero = 0;
    if (*at == '.')
        for (at++; *at >= '0' && *at <= '9'; at++) {
            nonzero |= *at != '0';
            if (digits++ < 6)
                fraction = fraction * 10 + (unsigned)(*at - '0');
        }
    while (digits++ < 6)
        fraction *= 10;
    return (seconds - nonzero) * 1000000 + fraction;
}
int64_t ret_now_us(void)
{
    struct etude_reading now;
    return etude_history_now(&now) ? -1 : ret_timestamp_us(now.utc);
}

static int ret_sum(uint64_t *total, uint64_t bytes)
{
    if (bytes > JSON_INTEGER_MAX || *total > (uint64_t)JSON_INTEGER_MAX - bytes)
        return ret_fail("retained byte account overflows");
    *total += bytes;
    return 0;
}

void *ret_next(void *directory, int *status)
{
    errno = 0;
    struct dirent *entry = readdir((DIR *)directory);
    if (!entry && errno)
        *status = ret_fail("retained directory could not be read");
    return entry;
}

static int ret_walk(const char *root, const char *relative, struct json *files, uint64_t *size,
                    int64_t *latest, unsigned depth)
{
    if (depth > 64)
        return ret_fail("retained directory is too deep");
    char *path = *relative ? ret_format("%s/%s", root, relative) : strdup(root);
    struct stat st;
    if (!path || lstat(path, &st)) {
        free(path);
        return ret_fail("retained path unavailable");
    }
#ifdef _WIN32
    int64_t mtime = st.st_mtim.tv_sec + (st.st_mtim.tv_nsec != 0);
#else
    int64_t mtime = st.st_mtime + 1;
#endif
    if (latest && mtime > *latest)
        *latest = mtime;
    int rc = 0;
    if (S_ISREG(st.st_mode)) {
        rc = st.st_size < 0 || ret_sum(size, (uint64_t)st.st_size);
        if (!rc && files) {
            struct json *ref = ret_descriptor(path);
            rc = !ref || ret_set(files, relative, ref);
        }
    } else if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(path);
        struct dirent *entry;
        rc = !dir;
        while (!rc && (entry = ret_next(dir, &rc))) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
                continue;
            char *name =
                *relative ? ret_format("%s/%s", relative, entry->d_name) : strdup(entry->d_name);
            rc = !name || ret_walk(root, name, files, size, latest, depth + 1);
            free(name);
        }
        if (dir)
            closedir(dir);
    } else
        rc = ret_fail("retained path is a link or special file: %s", path);
    free(path);
    return rc;
}
struct json *ret_files(const char *root, uint64_t *size, int64_t *latest)
{
    struct json *files = ret_new(JSON_OBJECT);
    *size = 0;
    if (latest)
        *latest = 0;
    if (ret_walk(root, "", files, size, latest, 0)) {
        json_free(files);
        return NULL;
    }
    return files;
}
int ret_usage_tree(const char *root, uint64_t *size)
{
    *size = 0;
    return ret_walk(root, "", NULL, size, NULL, 0);
}
int ret_remove(const char *path)
{
    struct stat st;
    if (lstat(path, &st))
        return -1;
    if (S_ISREG(st.st_mode))
        return unlink(path);
    if (!S_ISDIR(st.st_mode))
        return ret_fail("refusing removal of a retained link or special file");
    DIR *dir = opendir(path);
    struct dirent *entry;
    int rc = !dir;
    while (!rc && (entry = ret_next(dir, &rc))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char *child = ret_format("%s/%s", path, entry->d_name);
        rc = !child || ret_remove(child);
        free(child);
    }
    if (dir)
        closedir(dir);
    return rc ? rc : rmdir(path);
}

int ret_policy_check(const struct json *policy, const char *site)
{
    const char *format = json_text(policy, "format"), *expected = json_text(policy, "site_sha256");
    struct json *actual = ret_descriptor(site);
    const char *hash = json_text(actual, "sha256");
    int valid = format && !strcmp(format, "etude-retention-policy") &&
                ret_number(policy, "schema_version") == 1 && hash && expected &&
                !strcmp(hash, expected);
    json_free(actual);
    if (!valid)
        return ret_fail("retention policy belongs to a different site or format");
    const char *limits[] = {"max_bytes", "max_age_days", "reserve_bytes",
                            "issue_snapshot_max_age_seconds"};
    for (size_t i = 0; i < 4; i++)
        if (ret_number(policy, limits[i]) <= 0)
            return ret_fail("invalid retention limit: %s", limits[i]);
    if (ret_number(policy, "reserve_bytes") > ret_number(policy, "max_bytes"))
        return ret_fail("run reservation exceeds retention limit");
    const struct json *baselines = json_member(policy, "baselines"),
                      *anchors = json_member(policy, "anchors");
    if (!baselines || baselines->type != JSON_ARRAY || !anchors || anchors->type != JSON_ARRAY)
        return ret_fail("retention protection arrays are absent");
    return 0;
}

static int ret_lock(struct retention *r)
{
    char *path = ret_format("%s/.registry-lock", r->root);
    r->lock = path ? open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600) : -1;
    free(path);
    if (r->lock < 0)
        return ret_fail("retention registry lock is unavailable");
    struct stat st;
    if (fstat(r->lock, &st) || !S_ISREG(st.st_mode))
        return -1;
    if (!st.st_size && (write(r->lock, "\0", 1) != 1 || fsync(r->lock)))
        return -1;
    for (;;) {
        if (r->stop && *r->stop)
            return ret_fail("retention registry wait cancelled");
#ifdef _WIN32
        if (!etude_win_file_lock(r->lock, F_WRLCK, 0))
            return 0;
        if (errno != EACCES && errno != EAGAIN)
            return -1;
        Sleep(50);
#else
        if (!flock(r->lock, LOCK_EX | LOCK_NB))
            return 0;
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
            return -1;
        /* The kernel lock decides readiness; the wait only yields the CPU. */
        poll(NULL, 0, 50);
#endif
    }
}

int ret_registry_open(struct retention *r, const char *policy, const char *site,
                      volatile sig_atomic_t *stop)
{
    memset(r, 0, sizeof(*r));
    retention_error[0] = 0;
    r->lock = -1;
    r->stop = stop;
    if (stop && *stop)
        return ret_fail("retention operation cancelled");
    r->policy = ret_document(policy);
    if (!r->policy || ret_policy_check(r->policy, site))
        goto failed;
    const char *given = json_text(r->policy, "root");
    r->root = given && ret_absolute(given) ? ret_path(given, 1) : NULL;
    struct stat st;
    if (!r->root || lstat(r->root, &st) || !S_ISDIR(st.st_mode))
        goto failed;
    const char *directories[] = {"registry", "runs", "accepted"};
    for (size_t i = 0; i < 3; i++) {
        char *path = ret_format("%s/%s", r->root, directories[i]);
        int bad = !path || lstat(path, &st) || !S_ISDIR(st.st_mode);
        free(path);
        if (bad)
            goto failed;
    }
    char *path = ret_format("%s/.etude-retention-root.json", r->root);
    struct json *marker = path ? ret_document(path) : NULL, *expected = ret_new(JSON_OBJECT);
    ret_set(expected, "format", ret_string("etude-retention-root"));
    ret_set(expected, "schema_version", ret_integer(1));
    ret_set(expected, "root", ret_string(r->root));
    ret_set(expected, "site_sha256", ret_string(json_text(r->policy, "site_sha256")));
    int bad = !marker || !json_equal(marker, expected);
    free(path);
    json_free(marker);
    json_free(expected);
    if (bad || ret_lock(r))
        goto failed;
    return 0;
failed:
    ret_registry_close(r);
    return *retention_error
               ? -1
               : ret_fail("retention root, policy, ownership marker or lock is unavailable");
}
void ret_registry_close(struct retention *r)
{
    if (r->lock >= 0)
        close(r->lock);
    json_free(r->policy);
    free(r->root);
    r->root = NULL;
    r->policy = NULL;
    r->lock = -1;
}

static int ret_program(const char *name, char out[4096])
{
#ifdef _WIN32
    return etude_win_program(name, out, 4096);
#else
    if (strchr(name, '/'))
        return realpath(name, out) ? 0 : -1;
    const char *path = getenv("PATH");
    for (const char *p = path ? path : "/usr/bin:/bin";;) {
        size_t n = strcspn(p, ":");
        int size = snprintf(out, 4096, "%.*s/%s", (int)n, p, name);
        if (size >= 0 && size < 4096 && !access(out, X_OK))
            return 0;
        if (!p[n])
            break;
        p += n + 1;
    }
    return -1;
#endif
}
#ifdef _WIN32
#define environ _environ
#else
extern char **environ;
#endif

static void ret_command_error(const struct etude_exec_result *result, const char *output)
{
    if (!output && result->err.kept)
        fwrite(result->err.data, 1, result->err.kept, stderr);
}

int ret_execute(char *const argv[], const char *cwd, const char *output, char **captured,
                volatile sig_atomic_t *stop, uint64_t deadline_us)
{
    struct etude_exec *executor = etude_exec_new();
    struct etude_proc *process = NULL;
    char program[4096];
    size_t count = 0;
    while (argv[count])
        count++;
    char **args = calloc(count + 1, sizeof(*args));
    int status = -1;
    if ((stop && *stop) || !executor || !args || ret_program(argv[0], program))
        goto done;
    memcpy(args, argv, count * sizeof(*args));
    args[0] = program;
    struct etude_exec_spec spec = {.attempt_id = "retained-proof-command",
                                   .argv = args,
                                   .envp = environ,
                                   .cwd = cwd,
                                   .stdout_path = output,
                                   .stderr_path = output,
                                   .output_max = 64u << 20,
                                   .grace_us = 1000000};
    struct etude_reading start, now;
    if (etude_history_now(&start) || etude_exec_start(executor, &spec, &process))
        goto done;
    while (etude_exec_wait(executor, 100) != process) {
        if ((stop && *stop) ||
            (deadline_us &&
             (etude_history_now(&now) || now.monotonic_us - start.monotonic_us >= deadline_us)))
            etude_exec_cancel(process);
    }
    const struct etude_exec_result *result = etude_proc_result(process);
    if (result->end != ETUDE_END_EXITED || result->cleanup != ETUDE_CLEANUP_CONFIRMED ||
        result->output_error || (captured && result->out.kept != result->out.total))
        goto done;
    status = result->value;
    if (captured && !status) {
        *captured = json_alloc(result->out.kept + 1);
        memcpy(*captured, result->out.data, result->out.kept);
    }
    ret_command_error(result, output);
done:
    if (executor)
        etude_exec_free(executor);
    free(args);
    return status < 0 ? ret_fail("retained-proof command failed or cleanup is unconfirmed")
                      : status;
}
