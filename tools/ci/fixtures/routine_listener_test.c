#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "routine_producer.h"
#include "preparation_import.h"
#include "../../../tests/host/compat/fixdir_host.c"
static char *fixture_read(const char *path);
static int fixture_run(struct hp_context *host, char *const argv[], const char *out,
                       const char *err);
static time_t fixture_time(time_t *value);
#define hp_read fixture_read
#define hp_run  fixture_run
#define time    fixture_time
#include "../../etude/routine_jobs.c"
#undef hp_read
#undef hp_run
#undef time
static int failures;
#define CHECK(c)                                                 \
    do {                                                         \
        if (!(c)) {                                              \
            fprintf(stderr, "listener: %d: %s\n", __LINE__, #c); \
            failures++;                                          \
        }                                                        \
    } while (0)
static const char *fixture_id = "12345678-1234-1234-1234-123456789abc\n";
static const char *fixture_stat = "btime 1792454400\n";
static char listener_path[4096];
static int boot_reads, runner_reads, change_boot, change_server, change_file, busy_server;

static time_t fixture_time(time_t *value)
{
    time_t now = 1792800000;
    if (value)
        *value = now;
    return now;
}

static char *fixture_read(const char *path)
{
    if (!strcmp(path, "/proc/sys/kernel/random/boot_id")) {
        boot_reads++;
        const char *id =
            change_boot && boot_reads > 1 ? "22345678-1234-1234-1234-123456789abc\n" : fixture_id;
        return id ? strdup(id) : NULL;
    }
    if (!strcmp(path, "/proc/stat"))
        return fixture_stat ? strdup(fixture_stat) : NULL;
    char *raw = hp_read(path);
    if (change_file && !strcmp(path, listener_path)) {
        const char changed[] = "changed listener bytes\n";
        CHECK(!hp_write(path, changed, sizeof changed - 1));
    }
    return raw;
}

/* API replies are private files; no network command runs in this fixture. */
static int fixture_run(struct hp_context *host, char *const argv[], const char *out,
                       const char *err)
{
    (void)host;
    (void)err;
    CHECK(!strcmp(argv[0], "gh") && !strcmp(argv[1], "api"));
    const char *reply = "[{\"workflow_runs\":[]}]";
    if (strstr(argv[4], "/runners")) {
        runner_reads++;
        int busy = busy_server || (change_server && runner_reads > 1);
        reply = busy ? "[{\"runners\":[{\"name\":\"ci\",\"id\":1,"
                       "\"status\":\"online\",\"busy\":true}]}]"
                     : "[{\"runners\":[{\"name\":\"ci\",\"id\":1,"
                       "\"status\":\"online\",\"busy\":false}]}]";
    }
    return hp_write(out, reply, strlen(reply));
}

static struct json *parse(const char *bytes)
{
    struct json *value = NULL;
    char why[256];
    CHECK(!json_parse_data(bytes, strlen(bytes), 1, &value, why, sizeof why));
    return value;
}

static void accepted_jobs(void)
{
    struct json *jobs = parse("[{\"id\":8,\"name\":\"collection\",\"runner_name\":\"ci\","
                              "\"runner_id\":1,\"status\":\"in_progress\"}]");
    const char *name = NULL;
    CHECK(!own_job(jobs, "ci", &name) && same(name, "collection"));
    CHECK(own_job(jobs, "other", &name));
    struct json *other = parse("{\"id\":9,\"name\":\"other\",\"runner_name\":\"other\","
                               "\"runner_id\":2,\"status\":\"queued\"}");
    hp_item(jobs, other);
    CHECK(own_job(jobs, "ci", &name));
    hp_member(other, "runner_id", hp_number(0));
    CHECK(!own_job(jobs, "ci", &name));
    hp_member(other, "status", hp_string("in_progress"));
    hp_member(other, "runner_name", hp_string("ci"));
    CHECK(own_job(jobs, "ci", &name));
    json_free(jobs);
    struct json *server = parse("{\"runners\":{\"ci\":{\"id\":1,\"status\":\"online\","
                                "\"busy\":false}},\"workflows\":{},\"jobs\":[]}");
    struct json *before = server_identity(server), *after = server_identity(server);
    CHECK(json_equal(before, after));
    json_free(after);
    struct json *runner = (struct json *)json_member(json_member(server, "runners"), "ci");
    struct json *busy = hp_value(JSON_BOOLEAN);
    busy->integer = 1;
    hp_member(runner, "busy", busy);
    after = server_identity(server);
    CHECK(!json_equal(before, after));
    json_free(before);
    json_free(after);
    json_free(server);
}

static void boot_proofs(void)
{
    struct json *boot = boot_snapshot();
    CHECK(boot && !strcmp(json_text(boot, "boot_id"), "12345678-1234-1234-1234-123456789abc"));
    json_free(boot);
    const char *ids[] = {NULL, "unknown", fixture_id, fixture_id, fixture_id};
    const char *stats[] = {fixture_stat, fixture_stat, "", "btime 1792454400\nbtime 1792454401\n",
                           "btime 9999999999999\n"};
    const char *original_id = fixture_id, *original_stat = fixture_stat;
    for (size_t i = 0; i < sizeof ids / sizeof *ids; i++) {
        fixture_id = ids[i];
        fixture_stat = stats[i];
        boot = boot_snapshot();
        CHECK(!boot);
        json_free(boot);
    }
    fixture_id = original_id;
    fixture_stat = original_stat;
}

static void drain_proofs(void)
{
    struct hp_context host = {0};
    snprintf(host.tree, sizeof host.tree, "%s", fix_dir());
    snprintf(listener_path, sizeof listener_path, "%s/Runner_20261022.log", fix_dir());
    struct json *config = parse("{\"repository\":\"fixture/project\",\"listeners\":{}}");
    hp_member((struct json *)json_member(config, "listeners"), "ci", hp_string(fix_dir()));
    const char quiet[] = "[2026-10-22 16:27:44Z INFO Terminal] Listening for Jobs\n";
    for (int fault = 0; fault < 6; fault++) {
        boot_reads = runner_reads = 0;
        change_boot = fault == 1;
        change_server = fault == 2;
        busy_server = fault == 3;
        change_file = fault == 5;
        const char *bytes =
            fault == 4 ? "[2026-10-22 16:27:44Z INFO Terminal] Running job: unknown\n" : quiet;
        CHECK(!hp_write(listener_path, bytes, strlen(bytes)));
        struct json *proof = etude_routine_jobs_snapshot(&host, config, NULL, NULL, "fixture");
        if (!fault) {
            char hex[65];
            CHECK(proof && json_member(proof, "boot"));
            const struct json *log = json_member(json_member(proof, "listeners"), "ci");
            const struct json *files = json_member(log, "files");
            CHECK(!hp_hash(listener_path, hex));
            CHECK(files && files->count == 1 && same(json_text(files->item[0], "sha256"), hex));
        } else
            CHECK(!proof);
        json_free(proof);
    }
    json_free(config);
}

static void interrupted(int number)
{
    exit(128 + number);
}

int main(int argc, char **argv)
{
    signal(SIGTERM, interrupted);
    signal(SIGINT, interrupted);
#ifndef _WIN32
    signal(SIGHUP, interrupted);
#endif
    if (argc == 2 && !strcmp(argv[1], "--signal-exit")) {
        fix_dir();
        raise(SIGTERM);
    }
    const long long boot = 1792454400, now = 1792800000;
    const char *old = "[2026-10-17 22:44:01Z INFO Terminal] Running job: core (i686)\n";
    const char *start = "[2026-10-22 16:27:44Z INFO Terminal] Running job: ci\n";
    const char *end =
        "[2026-10-22 16:28:44Z INFO Terminal] Job ci completed with result: Succeeded\n";
    char complete[1024];
    snprintf(complete, sizeof complete, "%s%s%s", old, start, end);
    struct json *state = etude_routine_listener_state(complete, boot, now);
    CHECK(state && json_member(state, "active")->type == JSON_NULL);
    CHECK(state && json_member(state, "current_events")->count == 2);
    CHECK(state && json_member(state, "superseded_by_boot")->count == 1);
    json_free(state);
    state = etude_routine_listener_state(start, boot, now);
    CHECK(state && !strcmp(json_text(state, "active"), "ci"));
    json_free(state);
    const char *bad[] = {old,
                         "Running job: unknown",
                         end,
                         "[2026-10-22 16:27:44Z INFO Terminal] Running job:\n",
                         "[2026-99-22 16:27:44Z INFO Terminal] Running job: ci\n",
                         "[9999-10-22 16:27:44Z INFO Terminal] Running job: ci\n"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        state = etude_routine_listener_state(bad[i], boot, now);
        CHECK(!state);
        json_free(state);
    }
    snprintf(complete, sizeof complete, "%s%s", start, start);
    state = etude_routine_listener_state(complete, boot, now);
    CHECK(!state);
    json_free(state);
    accepted_jobs();
    boot_proofs();
    drain_proofs();
    if (!failures)
        fix_passed();
    puts(failures ? "listener: failed" : "listener: pass");
    return failures != 0;
}
