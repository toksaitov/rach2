/* flow_check.c: each row runs with an empty home and private command path. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static char paths[32][4096];
static unsigned next_path;
static const char *path(const char *root, const char *name)
{
    char *v = paths[next_path++ % 32];
    if (snprintf(v, 4096, "%s/%s", root, name) >= 4096) {
        fprintf(stderr, "fixture: path too long\n");
        exit(2);
    }
    return v;
}
const char *flow_path(struct fixture *f, const char *name)
{
    return path(f->work, name);
}
const char *flow_source(struct fixture *f, const char *name)
{
    return path(f->tree, name);
}
int flow_put(struct fixture *f, const char *name, const char *text)
{
    return fx_text(flow_path(f, name), text);
}
int flow_copy(struct fixture *f, const char *source, const char *name)
{
    return fx_copy(flow_source(f, source), flow_path(f, name));
}
static int mock_command(int argc, char **argv)
{
    static const struct {
        const char *environment;
        int (*run)(int, char **);
    } mocks[] = {
        {"FLOW_LAUNCH", flow_launch_mock},   {"FLOW_TOOLING", flow_tooling_mock},
        {"FLOW_RELEASE", flow_release_mock}, {"FLOW_CI_RUN", flow_ci_run_mock},
        {"FLOW_CI", flow_ci_mock},           {"FLOW_BOARD", flow_board_mock},
        {"FLOW_MEASURE", flow_measure_mock}, {"FLOW_TESTING", flow_testing_mock},
        {"FLOW_DEPLOY", flow_deploy_mock},   {"FLOW_VERSION", flow_version_mock},
        {"FLOW_WORKER", flow_worker_mock},   {"FLOW_SPACE", flow_space_mock},
    };
    unsigned i;
    for (i = 0; i < sizeof(mocks) / sizeof(mocks[0]); i++)
        if (getenv(mocks[i].environment))
            return mocks[i].run(argc, argv);
    return flow_basic_mock(argc, argv);
}
static int controller(struct fixture *f, const char *id)
{
    static const struct {
        const char *id;
        int (*run)(struct fixture *);
    } controllers[] = {
        {"etude_launch", flow_launch},
        {"etude_tooling", flow_tooling},
        {"release_resume", flow_release},
        {"ci_run", flow_ci_run},
        {"ci", flow_ci},
        {"board", flow_board},
        {"machine_measure", flow_measure},
        {"test_status", flow_status},
        {"testing_run", flow_testing},
        {"etude_deploy", flow_deploy},
        {"etude_version", flow_version},
        {"tidy", flow_tidy},
        {"leftovers", flow_leftovers},
        {"worker_park", flow_worker},
        {"space", flow_space},
    };
    unsigned i;
    for (i = 0; i < sizeof(controllers) / sizeof(controllers[0]); i++)
        if (!strcmp(id, controllers[i].id))
            return controllers[i].run(f);
    return flow_basic(f, id);
}
int main(int argc, char **argv)
{
    struct fixture f;
    const char *name = strrchr(argv[0], '/'), *id;
    char value[8200];
    int ok;
    name = name ? name + 1 : argv[0];
    if (argc > 1 && !strncmp(argv[1], "--launch-", 9))
        return flow_launch_aux(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "--status-lock"))
        return flow_status_lock(argc, argv);
    if ((argc == 2 && !strcmp(argv[1], "--hold")) || (argc == 3 && !strcmp(argv[2], "--hold"))) {
        if (getenv("FLOW_WAIT_READY"))
            fx_text(getenv("FLOW_WAIT_READY"), "");
        for (;;)
            pause();
    }
    if (argc == 2 && !strcmp(argv[1], "--check"))
        return flow_check_workload(argc, argv);
    if (strcmp(name, "host-flow-check"))
        return mock_command(argc, argv);
    if (argc != 3) {
        fprintf(stderr, "usage: host-flow-check ID ROOT\n");
        return 2;
    }
    id = argv[1];
    if (!fx_init(&f, argv[2], id, argv[0]))
        return 2;
    snprintf(value, sizeof(value), "%s:/usr/bin:/bin:/usr/sbin:/sbin", f.bin);
    fx_env("PATH", value);
    fx_env("HOME", flow_path(&f, "home"));
    fx_mkdir(getenv("HOME"));
    fx_env("TMPDIR", flow_path(&f, "tmp"));
    fx_mkdir(getenv("TMPDIR"));
    fx_env("LC_ALL", "C");
    fx_env("FLOW_WORK", f.work);
    fx_env("FLOW_SELF", f.self);
    ok = controller(&f, id);
    fx_check(&f, ok, id);
    ok = !f.failed;
    fx_destroy(&f);
    return ok ? 0 : 1;
}
