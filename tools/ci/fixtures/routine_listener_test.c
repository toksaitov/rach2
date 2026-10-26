#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "routine_producer.h"
#include "../../etude/routine_jobs.c"
static int failures;
#define CHECK(c)                                                 \
    do {                                                         \
        if (!(c)) {                                              \
            fprintf(stderr, "listener: %d: %s\n", __LINE__, #c); \
            failures++;                                          \
        }                                                        \
    } while (0)
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

int main(void)
{
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
    puts(failures ? "listener: failed" : "listener: pass");
    return failures != 0;
}
