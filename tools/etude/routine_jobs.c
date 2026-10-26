/* Match accepted Actions jobs to retained listener events from this boot. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "routine_producer.h"
#include "preparation_import.h"
#include "history_json.h"
#include "sha256.h"

static const char *text(const struct json *object, const char *name)
{
    return json_text(object, name);
}

static int same(const char *a, const char *b)
{
    return a && b && !strcmp(a, b);
}

static struct json *failed(struct json *value)
{
    json_free(value);
    return NULL;
}

static int stamp(char out[21], long long seconds)
{
    time_t at = (time_t)seconds;
    struct tm *utc = gmtime(&at);
    return !utc || strftime(out, 21, "%Y-%m-%dT%H:%M:%SZ", utc) != 20 ? -1 : 0;
}

static int listener_event(const char *line, char **active)
{
    const char *start = strstr(line, "Running job: "), *job = strstr(line, "Job ");
    const char *end = job ? strstr(job, " completed with result: ") : NULL;
    if (start) {
        if (*active || !start[13])
            return -1;
        *active = strdup(start + 13);
    } else if (end) {
        char *name = strndup(job + 4, (size_t)(end - job - 4));
        const char *result = end + strlen(" completed with result: ");
        int bad = !*active || !same(*active, name) || !isalpha((unsigned char)*result);
        while (isalpha((unsigned char)*result))
            result++;
        while (isspace((unsigned char)*result))
            result++;
        bad |= *result != 0;
        free(name);
        if (bad)
            return -1;
        free(*active);
        *active = NULL;
    } else
        return -1;
    return 0;
}

struct listener {
    char before[21], current[21], *active;
    struct json *events, *old;
    int seen;
};

static int listener_line(struct listener *s, char *line)
{
    size_t n = strlen(line);
    if (n && line[n - 1] == '\r')
        line[--n] = 0;
    int event = strstr(line, "Running job:") ||
                (strstr(line, "Job ") && strstr(line, "completed with result:"));
    if (n < 22 || line[0] != '[' || line[21] != ' ')
        return event ? -1 : 0;
    char at[21];
    memcpy(at, line + 1, 20);
    at[10] = 'T';
    at[20] = 0;
    if (line[11] != ' ' || !eh_utc(at) || strcmp(at, s->current) > 0)
        return -1;
    if (strcmp(at, s->before) < 0) {
        if (event)
            hp_item(s->old, hp_string(line));
        return 0;
    }
    s->seen = 1;
    if (event) {
        if (listener_event(line, &s->active))
            return -1;
        hp_item(s->events, hp_string(line));
    }
    return 0;
}

struct json *etude_routine_listener_state(const char *bytes, long long boot, long long now)
{
    if (!bytes)
        return NULL;
    struct listener s = {0};
    s.events = hp_value(JSON_ARRAY);
    s.old = hp_value(JSON_ARRAY);
    char *copy = strdup(bytes), *line = copy;
    int bad = boot < 1 || now < boot || stamp(s.before, boot) || stamp(s.current, now);
    while (!bad && line && *line) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        bad = listener_line(&s, line);
        line = next;
    }
    struct json *state = hp_value(JSON_OBJECT);
    hp_member(state, "active", s.active ? hp_string(s.active) : hp_value(JSON_NULL));
    hp_member(state, "current_events", s.events);
    hp_member(state, "superseded_by_boot", s.old);
    free(s.active);
    free(copy);
    return bad || !s.seen ? failed(state) : state;
}

static struct json *boot_snapshot(void)
{
    char *id = hp_read("/proc/sys/kernel/random/boot_id"), *raw = hp_read("/proc/stat");
    long long boot = 0;
    int count = 0;
    if (id)
        id[strcspn(id, "\n\r")] = 0;
    int bad = !id || strlen(id) != 36;
    for (size_t i = 0; !bad && i < 36; i++)
        bad = i == 8 || i == 13 || i == 18 || i == 23 ? id[i] != '-'
                                                      : strchr("0123456789abcdef", id[i]) == NULL;
    char *line = raw;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        if (!strncmp(line, "btime ", 6)) {
            char *end;
            boot = strtoll(line + 6, &end, 10);
            bad |= *end != 0 || boot <= 0;
            count++;
        }
        line = next;
    }
    struct json *value = hp_value(JSON_OBJECT);
    char btime[64];
    snprintf(btime, sizeof btime, "btime %lld", boot);
    hp_member(value, "boot_id", hp_string(id ? id : ""));
    hp_member(value, "boot_unix_seconds", hp_number(boot));
    hp_member(value, "btime", hp_string(btime));
    free(id);
    free(raw);
    return bad || count != 1 || boot > (long long)time(NULL) ? failed(value) : value;
}

struct jobs_api {
    struct hp_context *host;
    const char *repository, *tag;
    unsigned serial;
};

static struct json *api(struct jobs_api *a, const char *suffix)
{
    struct hp_words command = {0};
    char endpoint[4096], output[4096], error[4096];
    snprintf(endpoint, sizeof endpoint, "repos/%s/actions/%s", a->repository, suffix);
    snprintf(output, sizeof output, "%s/%s.api.%u.json", a->host->tree, a->tag, a->serial);
    snprintf(error, sizeof error, "%s/%s.api.%u.err", a->host->tree, a->tag, a->serial++);
    hp_split(&command, "gh api --paginate --slurp");
    hp_add(&command, endpoint);
    int bad = hp_run(a->host, command.v, output, error);
    hp_words_free(&command);
    struct json *pages = !bad ? pi_document(output) : NULL;
    return pages && pages->type == JSON_ARRAY ? pages : failed(pages);
}

static int append_pages(struct json *into, const struct json *pages, const char *key)
{
    if (!pages)
        return -1;
    for (size_t i = 0; i < pages->count; i++) {
        const struct json *rows = json_member(pages->item[i], key);
        if (!rows || rows->type != JSON_ARRAY)
            return -1;
        for (size_t j = 0; j < rows->count; j++)
            hp_item(into, hp_clone(rows->item[j]));
    }
    return 0;
}

static int runners_read(struct jobs_api *a, struct json *runners)
{
    struct json *pages = api(a, "runners"), *rows = hp_value(JSON_ARRAY);
    int bad = append_pages(rows, pages, "runners");
    for (size_t i = 0; !bad && i < rows->count; i++) {
        const char *name = text(rows->item[i], "name");
        if (!name || !*name || json_member(runners, name))
            bad = 1;
        else
            hp_member(runners, name, hp_clone(rows->item[i]));
    }
    json_free(rows);
    json_free(pages);
    return bad;
}

static int workflows_read(struct jobs_api *a, struct json *workflows)
{
    const char *states[] = {"in_progress", "queued", "waiting", "pending", "requested"};
    int bad = 0;
    for (size_t i = 0; !bad && i < 5; i++) {
        char suffix[80];
        snprintf(suffix, sizeof suffix, "runs?status=%s", states[i]);
        struct json *pages = api(a, suffix), *rows = hp_value(JSON_ARRAY);
        bad = append_pages(rows, pages, "workflow_runs");
        for (size_t j = 0; !bad && j < rows->count; j++) {
            const struct json *id = json_member(rows->item[j], "id");
            char key[32];
            if (!id || id->type != JSON_INTEGER || id->integer <= 0)
                bad = 1;
            else {
                snprintf(key, sizeof key, "%lld", (long long)id->integer);
                hp_member(workflows, key, hp_clone(rows->item[j]));
            }
        }
        json_free(rows);
        json_free(pages);
    }
    return bad;
}

static int own_job(const struct json *jobs, const char *runner, const char **name)
{
    const struct json *own = NULL;
    for (size_t i = 0; i < jobs->count; i++)
        if (same(text(jobs->item[i], "runner_name"), runner) &&
            same(text(jobs->item[i], "status"), "in_progress")) {
            if (own)
                return -1;
            own = jobs->item[i];
        }
    if (!own || !text(own, "name") || !*text(own, "name"))
        return -1;
    const struct json *id = json_member(own, "id");
    if (!id || id->type != JSON_INTEGER)
        return -1;
    for (size_t i = 0; i < jobs->count; i++) {
        const struct json *accepted = json_member(jobs->item[i], "runner_id");
        if (!json_equal(id, json_member(jobs->item[i], "id")) &&
            !same(text(jobs->item[i], "status"), "completed") && accepted &&
            (accepted->type != JSON_INTEGER || accepted->integer != 0))
            return -1;
    }
    *name = text(own, "name");
    return 0;
}

static struct json *server_snapshot(struct jobs_api *a, const char *run, const char *runner)
{
    struct json *server = hp_value(JSON_OBJECT), *runners = hp_value(JSON_OBJECT);
    struct json *flows = hp_value(JSON_OBJECT), *jobs = hp_value(JSON_ARRAY);
    int bad = runners_read(a, runners) || workflows_read(a, flows);
    for (size_t i = 0; !bad && i < flows->count; i++)
        bad = !same(flows->item[i]->name, run);
    const char *name = NULL;
    if (!bad && run) {
        char suffix[128];
        snprintf(suffix, sizeof suffix, "runs/%s/jobs", run);
        struct json *pages = api(a, suffix);
        bad = !same(text(json_member(flows, run), "status"), "in_progress") ||
              append_pages(jobs, pages, "jobs") || own_job(jobs, runner, &name);
        json_free(pages);
    }
    hp_member(server, "coordinating_job", name ? hp_string(name) : hp_value(JSON_NULL));
    hp_member(server, "runners", runners);
    hp_member(server, "workflows", flows);
    hp_member(server, "jobs", jobs);
    return bad ? failed(server) : server;
}

static struct json *server_identity(const struct json *server)
{
    struct json *identity = hp_value(JSON_ARRAY);
    const char *groups[] = {"runners", "workflows", "jobs"};
    const char *fields[] = {"id", "status", "busy", "runner_id", "runner_name", "name"};
    for (size_t i = 0; i < 3; i++) {
        const struct json *rows = json_member(server, groups[i]);
        struct json *group = hp_value(JSON_OBJECT);
        for (size_t j = 0; rows && j < rows->count; j++) {
            struct json *item = hp_value(JSON_OBJECT);
            for (size_t k = 0; k < 6; k++) {
                int used = i == 0 ? k < 3 : i == 1 ? k == 1 : k != 2;
                if (used)
                    hp_member(item, fields[k], hp_clone(json_member(rows->item[j], fields[k])));
            }
            if (i == 2) {
                const struct json *id = json_member(rows->item[j], "id");
                char key[32];
                snprintf(key, sizeof key, "%lld", (long long)(id ? id->integer : 0));
                hp_member(group, key, item);
            } else
                hp_member(group, rows->item[j]->name, item);
        }
        hp_item(identity, group);
    }
    return identity;
}

static int order(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int listener_file(struct jbuf *bytes, struct json *files, const char *path)
{
    struct json *value = pi_describe(path);
    char *raw = hp_read(path), hex[65];
    struct sha256 hash;
    unsigned char digest[32];
    int bad = !value || !raw;
    if (!bad) {
        sha256_init(&hash);
        sha256_update(&hash, (const unsigned char *)raw, strlen(raw));
        sha256_final(&hash, digest);
        sha256_hex(digest, hex);
        const struct json *size = json_member(value, "size_bytes");
        bad = !size || size->integer != (long long)strlen(raw) ||
              !same(text(value, "sha256"), hex) || pi_verify(path, value);
    }
    if (!bad) {
        struct json *string = hp_string(raw), *checked = NULL;
        struct jbuf encoded = {0};
        char why[256];
        json_canonical(string, &encoded);
        bad = json_parse_data(encoded.data, encoded.len, 1, &checked, why, sizeof why);
        json_free(checked);
        json_free(string);
        jbuf_free(&encoded);
    }
    if (!bad) {
        hp_member(value, "path", hp_string(path));
        hp_item(files, value);
        value = NULL;
        jbuf_puts(bytes, raw);
    }
    json_free(value);
    free(raw);
    return bad;
}

static struct json *listener_logs(const char *directory, long long boot, const char *runner,
                                  const char *job)
{
    DIR *dir = opendir(directory);
    struct hp_words paths = {0};
    struct dirent *entry;
    while (dir && (entry = readdir(dir))) {
        size_t n = strlen(entry->d_name);
        if (!strncmp(entry->d_name, "Runner_", 7) && n > 11 &&
            !strcmp(entry->d_name + n - 4, ".log")) {
            char *path = pi_path(directory, entry->d_name);
            hp_add(&paths, path);
            free(path);
        }
    }
    if (dir)
        closedir(dir);
    if (paths.n > 1)
        qsort(paths.v, paths.n, sizeof(*paths.v), order);
    struct jbuf bytes = {0};
    struct json *files = hp_value(JSON_ARRAY);
    int bad = !paths.n;
    for (size_t i = 0; !bad && i < paths.n; i++)
        bad = listener_file(&bytes, files, paths.v[i]);
    struct json *state = !bad ? etude_routine_listener_state(bytes.data, boot, time(NULL)) : NULL;
    const char *active = text(state, "active");
    if (active && (!same(active, job) || !runner))
        state = failed(state);
    if (state) {
        struct sha256 hash;
        unsigned char raw[32];
        char hex[65];
        sha256_init(&hash);
        sha256_update(&hash, (const unsigned char *)bytes.data, bytes.len);
        sha256_final(&hash, raw);
        sha256_hex(raw, hex);
        hp_member(state, "files", files);
        files = NULL;
        hp_member(state, "sha256", hp_string(hex));
    }
    json_free(files);
    hp_words_free(&paths);
    jbuf_free(&bytes);
    return state;
}

static int listener_snapshots(const struct json *listeners, const struct json *server,
                              long long boot, const char *run, const char *runner,
                              struct json *logs)
{
    for (size_t i = 0; i < listeners->count; i++) {
        const struct json *listener = listeners->item[i];
        const struct json *state = json_member(json_member(server, "runners"), listener->name);
        const struct json *busy = json_member(state, "busy");
        const char *job = same(listener->name, runner) ? text(server, "coordinating_job") : NULL;
        if (listener->type != JSON_STRING || !same(text(state, "status"), "online") || !busy ||
            busy->type != JSON_BOOLEAN || (busy->integer && (!run || !job)))
            return -1;
        struct json *log = listener_logs(listener->string, boot,
                                         same(listener->name, runner) ? runner : NULL, job);
        if (!log)
            return -1;
        hp_member(logs, listener->name, log);
    }
    return 0;
}

struct json *etude_routine_jobs_snapshot(struct hp_context *host, const struct json *config,
                                         const char *run, const char *runner, const char *tag)
{
    const char *repository = text(config, "repository");
    const struct json *listeners = json_member(config, "listeners");
    if (!repository || *repository == '/' || !strchr(repository, '/') ||
        !strchr(repository, '/')[1] || strchr(strchr(repository, '/') + 1, '/') ||
        strspn(repository, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-/") !=
            strlen(repository) ||
        !listeners || listeners->type != JSON_OBJECT ||
        (run && (!*run || strspn(run, "0123456789") != strlen(run))))
        return NULL;
    struct jobs_api a = {host, repository, tag, 0};
    struct json *boot = boot_snapshot(), *server = boot ? server_snapshot(&a, run, runner) : NULL;
    struct json *logs = hp_value(JSON_OBJECT);
    const struct json *seconds = json_member(boot, "boot_unix_seconds");
    int bad = !server || !seconds ||
              listener_snapshots(listeners, server, seconds->integer, run, runner, logs);
    struct json *after = !bad ? server_snapshot(&a, run, runner) : NULL;
    struct json *last_boot = boot_snapshot(), *old_id = server_identity(server);
    struct json *new_id = server_identity(after);
    bad =
        bad || !after || !last_boot || !json_equal(boot, last_boot) || !json_equal(old_id, new_id);
    if (bad) {
        json_free(boot);
        json_free(server);
        json_free(after);
        json_free(logs);
        json_free(last_boot);
        json_free(old_id);
        json_free(new_id);
        return NULL;
    }
    struct json *proof = hp_value(JSON_OBJECT);
    hp_member(proof, "boot", boot);
    hp_member(proof, "server_before", server);
    hp_member(proof, "server_after", after);
    hp_member(proof, "listeners", logs);
    hp_member(proof, "coordinating_run", run ? hp_string(run) : hp_value(JSON_NULL));
    hp_member(proof, "coordinating_runner", runner ? hp_string(runner) : hp_value(JSON_NULL));
    const char *job = text(server, "coordinating_job");
    hp_member(proof, "coordinating_job", job ? hp_string(job) : hp_value(JSON_NULL));
    json_free(last_boot);
    json_free(old_id);
    json_free(new_id);
    return bad ? failed(proof) : proof;
}
