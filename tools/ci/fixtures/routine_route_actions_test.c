/* Exercise fixed native route contracts without an endpoint or authority. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../etude/routine_route.c"
#include "../../../tests/host/compat/fixdir_host.c"

static int failures, put_calls, get_calls, fail_get;
static char names[32][128];
#define CHECK(c)                                              \
    do {                                                      \
        if (!(c)) {                                           \
            fprintf(stderr, "route: %d: %s\n", __LINE__, #c); \
            failures++;                                       \
        }                                                     \
    } while (0)

/* The fixture replaces only the path and remote transport boundaries. */
int routine_path(char out[4096], const char *root, const char *name)
{
    int n = snprintf(out, 4096, "%s/%s", root, name);
    return n < 0 || n >= 4096 ? -1 : 0;
}

int etude_routine_remote_put(struct etude_routine_remote *remote, const char *name,
                             const void *bytes, size_t size, const char *mode)
{
    CHECK(remote != NULL && bytes != NULL && size > 0);
    CHECK(!strcmp(name, "producer-etude") && !strcmp(mode, "0755"));
    put_calls++;
    return 0;
}

int etude_routine_remote_get(struct etude_routine_remote *remote, const char *token,
                             const char *name, size_t maximum, char **bytes, size_t *size)
{
    CHECK(remote != NULL && !strcmp(token, "fixture-token") && maximum > 64);
    CHECK(get_calls < 32 && strlen(name) < sizeof names[0]);
    snprintf(names[get_calls++], sizeof names[0], "%s", name);
    if (fail_get && !strcmp(name, "producer.json"))
        return -1;
    *bytes = strdup("fixture proof bytes\n");
    *size = strlen(*bytes);
    return 0;
}

static struct json *parse(const char *bytes)
{
    struct json *value = NULL;
    char why[256];
    CHECK(!json_parse(bytes, strlen(bytes), 1, &value, why, sizeof why));
    return value;
}

static void operation_contract(struct route *r)
{
    struct json *value = remote_operation(r, "produce", 1, "/state/result");
    CHECK(pi_same(field(value, "commit"), r->in.commit));
    CHECK(json_member(value, "clean")->integer == 1);
    const struct json *args = json_member(value, "args");
    CHECK(args->count == 13 && pi_same(args->item[0]->string, "produce"));
    CHECK(pi_same(args->item[2]->string, "${WORKSPACE}/config.json"));
    CHECK(pi_same(args->item[11]->string, "--") && pi_same(args->item[12]->string, "all"));
    json_free(value);
    value = remote_transport(r, 1, "/state/grant", "/state/result");
    const struct json *grant = json_member(value, "_transport_admission");
    CHECK(pi_same(field(grant, "claim"), "fixture:producer:prepare"));
    CHECK(json_member(grant, "seconds")->integer == 540);
    json_free(value);
    value = remote_transport(r, 0, "/state/grant", "/state/result");
    CHECK(!json_member(value, "_transport_admission"));
    json_free(value);
    value = remote_environment(r, 1, "/state/grant");
    CHECK(pi_same(field(value, "X"), "fixed"));
    CHECK(pi_same(field(value, "ETUDE_PREPARATION_GRANT"), "/state/grant"));
    CHECK(pi_same(field(value, "ETUDE_ADMITTED"), "/site"));
    json_free(value);
    struct json *env =
        (struct json *)json_member(json_member(r->config, "producer"), "environment");
    hp_member(env, "wrong", hp_number(1));
    CHECK(!remote_environment(r, 1, "/state/grant"));
}

static void fetched_contract(struct route *r, const char *root, const char *mode,
                             const char *action, int admitted, int expected)
{
    char directory[4096];
    CHECK(!path(directory, root, "fetched") && !mkdir(directory, 0700));
    r->mode = mode;
    get_calls = 0;
    CHECK(!fetch_results(r, (void *)1, "fixture-token", action, admitted, directory));
    CHECK(get_calls == expected);
    for (int i = 0; i < get_calls; i++) {
        char file[4096];
        CHECK(!path(file, directory, names[i]));
        CHECK(!unlink(file));
    }
    CHECK(!rmdir(directory));
}

static void failed_fetch(struct route *r, const char *root)
{
    char directory[4096];
    CHECK(!path(directory, root, "partial") && !mkdir(directory, 0700));
    r->mode = "macos-import";
    get_calls = 0;
    fail_get = 1;
    CHECK(fetch_results(r, (void *)1, "fixture-token", "produce", 1, directory));
    CHECK(get_calls == 1);
    CHECK(!rmdir(directory));
    fail_get = 0;
}

static void actor_contract(struct route *r, const char *root)
{
    char file[4096];
    CHECK(!path(file, root, "actor"));
    const char bytes[] = "native actor fixture bytes\n";
    CHECK(!hp_write(file, bytes, sizeof bytes - 1));
    struct json *actor = pi_describe(file);
    CHECK(actor != NULL);
    hp_member(actor, "path", hp_string(file));
    struct json *p = (struct json *)json_member(r->config, "producer");
    hp_member(p, "actor", actor);
    put_calls = 0;
    CHECK(!put_actor(r, (void *)1) && put_calls == 1);
    hp_member(actor, "sha256",
              hp_string("0000000000000000000000000000000000000000000000000000000000000000"));
    CHECK(put_actor(r, (void *)1) && put_calls == 1);
    CHECK(!unlink(file));
}

static void request_contract(struct route *r)
{
    struct json *request = hp_value(JSON_OBJECT);
    const char *keys[] = {"purpose", "commit", "run", "arches", "accel"};
    const char *values[] = {r->in.purpose, r->in.commit, r->in.run, r->in.arches, r->in.accel};
    for (size_t i = 0; i < 5; i++)
        hp_member(request, keys[i], hp_string(values[i]));
    hp_member(request, "selection", parse("[\"all\"]"));
    hp_member(request, "preparation", hp_clone(r->preparation));
    hp_member(request, "out", hp_string("/shared/results/sample"));
    CHECK(!direct_request(r, request));
    hp_member(request, "out", hp_string("/shared/results-other/sample"));
    CHECK(direct_request(r, request));
    hp_member(request, "out", hp_string("/shared/results/sample"));
    hp_member(request, "selection", parse("[\"host\"]"));
    CHECK(direct_request(r, request));
    hp_member(request, "selection", parse("[\"all\"]"));
    hp_member(request, "preparation", parse("{\"clean\":false,\"selection_export\":null}"));
    CHECK(direct_request(r, request));
    json_free(request);
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
    const char *scratch = fix_dir();
    if (argc == 2 && !strcmp(argv[1], "--signal-exit"))
        raise(SIGTERM);
    struct route r = {0};
    r.mode = "macos-import";
    r.in.run = "fixture";
    strcpy(r.in.commit, "1111111111111111111111111111111111111111");
    r.in.purpose = "phase_first_run";
    r.in.arches = "both";
    r.in.accel = "whpx";
    char *selection[] = {"all"};
    r.in.selection = selection;
    r.in.count = 1;
    r.preparation = parse("{\"clean\":true,\"selection_export\":null}");
    r.config = parse("{\"producer\":{\"site\":\"/site\",\"environment\":{\"X\":\"fixed\"},"
                     "\"policy\":{\"seconds\":\"540\"}},"
                     "\"physical\":{\"linux_output_root\":\"/shared/results\"}}");
    operation_contract(&r);
    fetched_contract(&r, scratch, "macos-import", "produce", 1, 4);
    fetched_contract(&r, scratch, "windows-import", "produce", 0, 12);
    fetched_contract(&r, scratch, "windows-import", "freeze", 0, 8);
    fetched_contract(&r, scratch, "macos-import", "transport-cleanup", 0, 3);
    failed_fetch(&r, scratch);
    actor_contract(&r, scratch);
    request_contract(&r);
    json_free(r.config);
    json_free(r.preparation);
    if (!failures)
        fix_passed();
    if (failures)
        fprintf(stderr, "route fixture evidence: %s\n", scratch);
    else
        puts("route actions: pass");
    return failures != 0;
}
