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
int main(int argc, char **argv)
{
    struct fixture f;
    const char *name = strrchr(argv[0], '/'), *id;
    char value[8200];
    int ok;
    name = name ? name + 1 : argv[0];
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
    if (strcmp(name, "host-flow-check")) {
        if (getenv("FLOW_PROGRESS"))
            return flow_progress_mock(argc, argv);
        if (getenv("FLOW_TESTING"))
            return flow_testing_mock(argc, argv);
        if (getenv("FLOW_DEPLOY"))
            return flow_deploy_mock(argc, argv);
        if (getenv("FLOW_VERSION"))
            return flow_version_mock(argc, argv);
        if (getenv("FLOW_WORKER"))
            return flow_worker_mock(argc, argv);
        if (getenv("FLOW_SPACE"))
            return flow_space_mock(argc, argv);
        return flow_basic_mock(argc, argv);
    }
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
    if (!strcmp(id, "progress"))
        ok = flow_progress(&f);
    else if (!strcmp(id, "test_status"))
        ok = flow_status(&f);
    else if (!strcmp(id, "testing_run"))
        ok = flow_testing(&f);
    else if (!strcmp(id, "etude_deploy"))
        ok = flow_deploy(&f);
    else if (!strcmp(id, "etude_version"))
        ok = flow_version(&f);
    else if (!strcmp(id, "tidy"))
        ok = flow_tidy(&f);
    else if (!strcmp(id, "leftovers"))
        ok = flow_leftovers(&f);
    else if (!strcmp(id, "worker_park"))
        ok = flow_worker(&f);
    else if (!strcmp(id, "space"))
        ok = flow_space(&f);
    else
        ok = flow_basic(&f, id);
    fx_check(&f, ok, id);
    ok = !f.failed;
    fx_destroy(&f);
    return ok ? 0 : 1;
}
