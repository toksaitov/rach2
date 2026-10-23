/* host_scripts_scenario.c: all host.list identities in one declared case. */
#include "host_scripts.h"
#include "host_image.h"
#include "host_build.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct row {
    char id[80];
    int held;
};
static const char *kernel[] = {"chown",     "krffs_open", "krffs_root_lock", "krffs_stat",
                               "pagecache", "kstack",     "endothers",       NULL};
static const char *image[] = {"mkext2_overlay", "krffs_tools",      "install_accounts",
                              "tooling_image",  "collection_tools", NULL};
static const char *build[] = {"doom_libc",       "run_scripts",    "commit_msg",  "prereq",
                              "check_asm",       "check_ldscript", "together",    "gnu_cmake_jobs",
                              "gnu_port_resume", "host_suite",     "case_select", NULL};
static const char *flow[] = {"machine_measure",
                             "ci",
                             "ci_run",
                             "board",
                             "progress",
                             "limited",
                             "worker_park",
                             "release_resume",
                             "tidy",
                             "leftovers",
                             "agents_guest",
                             "testing_run",
                             "space",
                             "etude_version",
                             "etude_deploy",
                             "etude_launch",
                             "test_status",
                             "etude_tooling",
                             "agents_credentials",
                             NULL};
static int in(const char *const *list, const char *id)
{
    while (*list)
        if (!strcmp(*list++, id))
            return 1;
    return 0;
}
static int protocol(const char *id)
{
    return !strcmp(id, "anthropic") || !strcmp(id, "gemini") || !strcmp(id, "fakeapi");
}
static int rows(struct etude_case *c, struct row out[100], size_t *count)
{
    const char *path = etude_artifact(c, "host-list");
    FILE *f = path ? fopen(path, "rb") : NULL;
    char line[4096];
    size_t n = 0, i;
    int ok = 1;
    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        char *end, *at = line;
        at += strspn(at, " \t\r\n");
        if (!*at || *at == '#')
            continue;
        end = strchr(at, '|');
        if (!end || end == at || (size_t)(end - at) >= sizeof(out[0].id) || n == 100) {
            ok = 0;
            break;
        }
        *end = 0;
        for (i = 0; i < n; i++)
            if (!strcmp(out[i].id, at))
                ok = 0;
        if (!in(kernel, at) && !in(image, at) && !in(build, at) && !in(flow, at) && !protocol(at))
            ok = 0;
        strcpy(out[n].id, at);
        out[n++].held = 0;
    }
    if (ferror(f))
        ok = 0;
    fclose(f);
    for (i = 0; kernel[i]; i++) {
        size_t j;
        for (j = 0; j < n && strcmp(out[j].id, kernel[i]); j++)
            ;
        if (j == n)
            ok = 0;
    }
    for (i = 0; image[i]; i++) {
        size_t j;
        for (j = 0; j < n && strcmp(out[j].id, image[i]); j++)
            ;
        if (j == n)
            ok = 0;
    }
    for (i = 0; build[i]; i++) {
        size_t j;
        for (j = 0; j < n && strcmp(out[j].id, build[i]); j++)
            ;
        if (j == n)
            ok = 0;
    }
    for (i = 0; flow[i]; i++) {
        size_t j;
        for (j = 0; j < n && strcmp(out[j].id, flow[i]); j++)
            ;
        if (j == n)
            ok = 0;
    }
    *count = n;
    return ok && n == 45;
}
static int execute(struct etude_case_machine *m, const char *const args[], const char *label)
{
    struct etude_guest_result r;
    return etude_guest_command(m, args, NULL, NULL, label, &r) == ETUDE_OK && r.complete &&
           r.termination.end == ETUDE_END_EXITED && r.termination.value == 0 &&
           r.cleanup == ETUDE_CLEANUP_CONFIRMED;
}
static int prepare(struct etude_case *c, struct etude_case_machine *m)
{
    const char *archive = etude_artifact(c, "scripts-assets");
    const char *mkdir[] = {"/bin/mkdir", "-p", "/tmp/etude-host-scripts/tree", NULL};
    const char *untar[] = {"/bin/tar",
                           "-xf",
                           "/tmp/etude-host-scripts/assets.tar",
                           "-C",
                           "/tmp/etude-host-scripts/tree",
                           NULL};
    const char *readable[] = {"/bin/chmod", "-R", "a+rX", "/tmp/etude-host-scripts/tree", NULL};
    return archive && execute(m, mkdir, "products-workspace") &&
           etude_guest_put(m, archive, "/tmp/etude-host-scripts/assets.tar", "0600") == ETUDE_OK &&
           execute(m, untar, "products-assets") && execute(m, readable, "products-source-modes");
}
static int native(struct etude_case *c, const char *id)
{
    struct etude_termination end;
    char *args[] = {(char *)(protocol(id) ? "fakeapi" : id), NULL};
    return etude_host(c, args, id, &end) == ETUDE_OK && end.end == ETUDE_END_EXITED && !end.value;
}
int etude_scenario(struct etude_case *c)
{
    struct row inventory[100];
    struct etude_case_machine *m = NULL;
    struct etude_termination end;
    size_t n = 0, i;
    int listed, ready = 0, failed = 0;
    listed = rows(c, inventory, &n);
    etude_step(c, "listed");
    etude_assert(c, "listed", listed,
                 "host.list and the compiled dispatch contain the same checks");
    if (!listed)
        return 1;
    if (etude_step(c, "products") == ETUDE_OK &&
        etude_boot(c, "products", NULL, &m, &end) == ETUDE_OK &&
        etude_step(c, "prepare") == ETUDE_OK)
        ready = prepare(c, m);
    for (i = 0; i < n; i++) {
        const char *id = inventory[i].id;
        int held = 0;
        if (etude_step(c, id) != ETUDE_OK) {
            etude_notrun(c, id, "the case stopped before this check");
            failed = 1;
            continue;
        }
        if (in(kernel, id) || protocol(id))
            held = native(c, id);
        else if (!ready) {
            etude_notrun(c, id, "the products helper or its source assets did not start");
            failed = 1;
            continue;
        } else if (in(image, id))
            held = host_image_check(c, m, id);
        else if (in(build, id))
            held = host_build_check(c, m, id);
        else
            held = host_guest_check(c, m, "/host-products/host-flow-check", id);
        etude_assert(c, id, held,
                     held ? "the prepared C controller passed every row"
                          : "the C controller failed; its complete output is evidence");
        if (!held)
            failed = 1;
        etude_progress(c, "checks", i + 1);
    }
    if (ready && !failed) {
        const char *clean[] = {"/bin/rm", "-rf", "/tmp/etude-host-scripts", NULL};
        if (!execute(m, clean, "products-cleanup"))
            failed = 1;
    }
    return failed;
}
