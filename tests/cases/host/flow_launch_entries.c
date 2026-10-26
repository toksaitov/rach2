/* flow_launch_entries.c: real build, worker, owner and CI entry programs. */
#define _POSIX_C_SOURCE 200809L
#include "flow_launch.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int build(struct launch_fixture *u)
{
    struct fixture *f = u->f;
    char *text = fx_read(flow_source(f, "Makefile")), *at, *output;
    const char *plain[] = {"/usr/bin/make", "ARG=two words", NULL};
    const char *seven[] = {u->launcher, "build", "/usr/bin/make", "-j", "7", "ARG=two words", NULL};
    const char *sixteen[] = {u->launcher, "build", "/usr/bin/make", "-j16", "ARG=two words", NULL};
    int ok;
    if (!text || !(at = strstr(text, "# The architecture (mk/arch.mk):"))) {
        free(text);
        return 0;
    }
    *at = 0;
    output = malloc(strlen(text) + strlen(f->self) + 150);
    if (!output) {
        free(text);
        return 0;
    }
    sprintf(output, "%sall:\n\t@'%s' --launch-observer\n\t@test '$(ARG)' = 'two words'\nendif\n",
            text, f->self);
    ok = flow_put(f, "checkout/Makefile", output) &&
         flow_copy(f, "tools/etude/launch.sh", "checkout/tools/etude/launch.sh");
    free(text);
    free(output);
    chmod(flow_path(f, "checkout/tools/etude/launch.sh"), 0755);
    if (!ok || !launch_run(u, plain, 0) || !launch_saw(u, 1, 1))
        return 0;
    fx_env("EXPECT_MEMORY", "8589934592");
    if (!launch_run(u, seven, 0) || !launch_saw(u, 1, 1))
        return 0;
    fx_env("EXPECT_MEMORY", "18253611008");
    if (!launch_run(u, sixteen, 0) || !launch_saw(u, 1, 1))
        return 0;
    fx_env("EXPECT_MEMORY", "5368709120");
    return 1;
}
static int job_row(struct launch_fixture *u, const char *const args[], const char *wanted)
{
    return launch_run(u, args, 0) &&
           fx_check(u->f, fx_equal(flow_path(u->f, "job-args"), wanted), "make job count cap");
}
static int jobs(struct launch_fixture *u)
{
    struct fixture *f = u->f;
    char wrapper[4096];
    const char *unbounded[] = {wrapper, "-j", "components", NULL};
    const char *plain[] = {wrapper, "components", NULL};
    const char *one[] = {wrapper, "-j", "1", "components", NULL};
    const char *two[] = {wrapper, "--jobs=2", "components", NULL};
    const char *nine[] = {wrapper, "-j9", "components", NULL};
    const char *twelve[] = {wrapper, "-j12", "components", NULL};
    const char *nested[] = {wrapper, "all", NULL};
    char path[16384];
    strcpy(wrapper, flow_path(f, "etude-bin/make"));
    fx_env("FLOW_LAUNCH_MODE", "jobs");
    fx_copy(f->self, flow_path(f, "record-make"));
    chmod(flow_path(f, "record-make"), 0755);
    fx_env("ETUDE_MAKE_REAL", flow_path(f, "record-make"));
    fx_env("ETUDE_BUILD_JOBS", "3");
    fx_env("MAKELEVEL", "0");
    if (!job_row(u, unbounded, "-j\ncomponents\n-j3\n") ||
        !job_row(u, plain, "components\n-j1\n") || !job_row(u, one, "-j\n1\ncomponents\n-j1\n") ||
        !job_row(u, two, "--jobs=2\ncomponents\n-j2\n") ||
        !job_row(u, nine, "-j9\ncomponents\n-j3\n") ||
        !job_row(u, twelve, "-j12\ncomponents\n-j3\n"))
        return 0;
    fx_env("MAKEFLAGS", "-j2 -- JOBS=9");
    if (!job_row(u, plain, "components\n-j2\n"))
        return 0;
    fx_env("MAKEFLAGS", NULL);
    fx_env("ETUDE_BUILD_JOBS", NULL);
    fx_env("ETUDE_MAKE_REAL", NULL);
    fx_copy(f->self, flow_path(f, "unwrapped-bin/make"));
    chmod(flow_path(f, "unwrapped-bin/make"), 0755);
    snprintf(path, sizeof(path), "%s/etude-bin:%s/unwrapped-bin:%s:/usr/bin:/bin:/usr/sbin:/sbin",
             f->work, f->work, f->bin);
    fx_env("PATH", path);
    if (!job_row(u, unbounded, "-j\ncomponents\n-j1\n"))
        return 0;
    snprintf(path, sizeof(path), "%s:/usr/bin:/bin:/usr/sbin:/sbin", f->bin);
    fx_env("PATH", path);
    fx_env("ETUDE_MAKE_REAL", flow_path(f, "record-make"));
    fx_env("ETUDE_BUILD_JOBS", "3");
    fx_env("MAKELEVEL", "1");
    if (!job_row(u, nested, "all\n"))
        return 0;
    fx_env("ETUDE_MAKE_REAL", NULL);
    fx_env("ETUDE_BUILD_JOBS", NULL);
    fx_env("MAKELEVEL", NULL);
    return 1;
}
static int worker(struct launch_fixture *u)
{
    struct fixture *f = u->f;
    const char *a[] = {"sh", "tests/run", "style", NULL};
    fx_env("FLOW_LAUNCH_MODE", "worker");
    return flow_copy(f, "tests/run", "checkout/tests/run") &&
           flow_copy(f, "tools/etude/hold.sh", "checkout/tools/etude/hold.sh") &&
           launch_run(u, a, 1) && launch_saw(u, 1, 1);
}
static int ci_unconfigured(struct launch_fixture *u)
{
    struct fixture *f = u->f;
    char script[4096], output[4096];
    const char *a[] = {"sh", script, "core", output, NULL};
    strcpy(script, flow_path(f, "checkout/tools/ci/run.sh"));
    if (!flow_copy(f, "tools/ci/run.sh", "checkout/tools/ci/run.sh") ||
        (!fx_has(script, "results=native\n") && !fx_has(script, "results=lines\n")) ||
        !fx_replace(script, script, "results=lines\n", "results=native\n") ||
        !fx_has(script, "results=native\n"))
        return 0;
    strcpy(output, flow_path(f, "ci"));
    fx_env("FLOW_LAUNCH_MODE", "ci");
    if (!fx_mkdir(flow_path(f, "ci")) || !launch_run(u, a, 2))
        return 0;
    return fx_check(f,
                    strstr(f->err, "ci: refused: selected-site retention policy is absent") &&
                        !fx_exists(flow_path(f, "ci/start")) &&
                        !fx_exists(flow_path(f, "observed")),
                    "native CI refuses missing retention before starting work") &&
           launch_clear(u);
}
static int owner(struct launch_fixture *u)
{
    struct fixture *f = u->f;
    char command[32768], script[4096];
    const char *a[] = {"sh", script, "run", command, NULL};
    int ok;
    strcpy(script, flow_path(f, "owner/tools/dev/testing.sh"));
    if (!flow_copy(f, "tools/dev/testing.sh", "owner/tools/dev/testing.sh"))
        return 0;
    fx_env("FLOW_LAUNCH_MODE", "owner");
    fx_env("RACH2_TESTING_COPY", u->tree);
    snprintf(command, sizeof(command),
             "RACH2_WORKERS=2 RACH2_MEMORY_MIB=96 '%s/launch' build bash -c "
             "'\"%s\" --launch-observer || exit 3; printf \"a\\047b\\n\"; cat; exit 7'",
             f->work, f->self);
    flow_put(f, "owner-input", "owner input\n");
    ok = fx_run(f, a, u->tree, NULL, flow_path(f, "owner-input")) == 7 &&
         strstr(f->out, "a'b\nowner input\n");
    return fx_check(f, ok, "owner command preserves quotes, stdin and status") &&
           launch_saw(u, 1, 1);
}
static int legacy(struct launch_fixture *u)
{
    struct fixture *f = u->f;
    char result[4096], program[4096];
    const char *a[] = {u->launcher, "-r", result, "legacy", program, NULL};
    struct flock lock;
    int fd, ok;
    char *seen;
    strcpy(result, flow_path(f, "result"));
    strcpy(program, flow_path(f, "old-run"));
    fx_copy(f->self, program);
    chmod(flow_path(f, "old-run"), 0755);
    fd = open(flow_path(f, "locks/rach2-ports.0"), O_CREAT | O_RDWR, 0600);
    if (fd < 0)
        return 0;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    ok = !fcntl(fd, F_SETLK, &lock) && launch_run(u, a, 0);
    close(fd);
    seen = fx_read(flow_path(f, "observed"));
    ok = fx_check(f,
                  ok && seen && !strncmp(seen, "1000 ", 5) &&
                      fx_equal(flow_path(f, "result"), "exited 0\n"),
                  "legacy shares reserved port block") &&
         ok;
    free(seen);
    return ok && launch_saw(u, 1, 1);
}
static int bisection(struct launch_fixture *u)
{
    struct fixture *f = u->f;
    char script[4096];
    const char *a[] = {"sh", script,    "-e", "/host-products/etude", "core", "i686", "good", "bad",
                       "1",  "default", NULL};
    char machine[8192];
    int ok;
    strcpy(script, flow_source(f, "tools/ci/bisect.sh"));
    fx_env("FLOW_LAUNCH_MODE", "bisect");
    flow_put(f, "private-ledger/claims", "etude-ledger 1\n");
    snprintf(machine, sizeof(machine),
             "etude-machine 1\nledger %s/private-ledger\ncpus 16\nmemory_bytes 268435456\n",
             f->work);
    flow_put(f, "private-state/machine", machine);
    flow_put(f, "checkout/tools/ci/core.env", "# Core settings.\nRACH2_WORKERS=2\n");
    fx_env("GITHUB_REPOSITORY", "test/rach2");
    fx_env("GITHUB_RUN_ID", "1");
    fx_env("RUNNER_TEMP", flow_path(f, "ci-temp"));
    fx_env("RACH2_WORKERS", NULL);
    ok = launch_run(u, a, 0) && launch_saw(u, 2, 0) &&
         fx_equal(flow_path(f, "private-ledger/claims"), "etude-ledger 1\n");
    fx_env("RACH2_WORKERS", "2");
    return fx_check(f, ok, "bisection uses installed policy before private machine record");
}
static int nested(struct launch_fixture *u)
{
    struct fixture *f = u->f;
    char command[32768];
    const char *a[] = {u->launcher, "build", "/bin/sh", "-c", command, NULL};
    snprintf(command, sizeof(command),
             "'%s' --launch-observer && '%s/launch' -r '%s/result' "
             "legacy '%s' --launch-observer",
             f->self, f->work, f->work, f->self);
    if (!launch_run(u, a, 0) || !fx_equal(flow_path(f, "result"), "exited 0\n") ||
        !launch_saw(u, 2, 1))
        return 0;
    snprintf(command, sizeof(command),
             "'%s' --launch-observer && '%s/launch' -r '%s/result' "
             "legacy /bin/sh -c 'kill -TERM $$'",
             f->self, f->work, f->work);
    if (!launch_run(u, a, 143) || !fx_equal(flow_path(f, "result"), "incomplete\n") ||
        !launch_saw(u, 1, 1))
        return 0;
    flow_put(f, "checkout/tests/system/suites.mk", "# fixed ports\n");
    snprintf(command, sizeof(command), "'%s/launch' -r '%s/result' legacy '%s' --launch-observer",
             f->work, f->work, f->self);
    if (!launch_run(u, a, 2) || !fx_equal(flow_path(f, "result"), "refused\n") ||
        fx_exists(flow_path(f, "observed")) || !launch_clear(u))
        return 0;
    return flow_put(f, "checkout/tests/system/suites.mk", "# RACH2_PORT_OFFSET\n");
}
int launch_entries(struct launch_fixture *u)
{
    return build(u) && jobs(u) && worker(u) && ci_unconfigured(u) && owner(u) && legacy(u) &&
           bisection(u) && nested(u);
}
static int refuse(struct launch_fixture *u, const char *policy)
{
    struct fixture *f = u->f;
    const char *a[] = {u->launcher, "build", f->self, "--launch-observer", NULL};
    flow_put(f, "site", policy);
    return launch_run(u, a, 2) && fx_check(f, !fx_exists(flow_path(f, "observed")),
                                           "refused launch never executes command");
}
static int bad_policies(struct launch_fixture *u)
{
    static const char *from[] = {"etude-site 2", "endpoint linux", "/host-products/etude",
                                 "24000,103079215104,171798691840", NULL};
    static const char *to[] = {"etude-site 1", "endpoint windows", "/missing-etude",
                               "24000,0,171798691840"};
    struct fixture *f = u->f;
    char policy[33000];
    unsigned i;
    for (i = 0; from[i]; i++) {
        char *text;
        flow_put(f, "site", u->policy);
        if (!fx_replace(flow_path(f, "site"), flow_path(f, "bad-site"), from[i], to[i]))
            return 0;
        text = fx_read(flow_path(f, "bad-site"));
        if (!text)
            return 0;
        if (!refuse(u, text)) {
            free(text);
            return 0;
        }
        free(text);
    }
    snprintf(policy, sizeof(policy), "%scapacity 1,1,1\n", u->policy);
    return refuse(u, policy);
}
static int bad_environment(struct launch_fixture *u)
{
    struct fixture *f = u->f;
    static const char *key[] = {"ETUDE_SITE", "RACH2_PORT_OFFSET", "SSH_TEST_PORT",
                                "ETUDE_ADMITTED"};
    static const char *value[] = {"/missing-site", "0", "2222", "/other/site"};
    unsigned i;
    flow_put(f, "site", u->policy);
    for (i = 0; i < 4; i++) {
        fx_env(key[i], value[i]);
        if (!refuse(u, u->policy))
            return 0;
        fx_env(key[i], !i ? flow_path(f, "site") : NULL);
    }
    flow_put(f, "ledger/claims", "etude-ledger 2\n");
    if (!refuse(u, u->policy))
        return 0;
    return flow_put(f, "ledger/claims", "etude-ledger 1\n");
}
int launch_refusals(struct launch_fixture *u)
{
    struct fixture *f = u->f;
    const char *a[] = {u->launcher, "legacy", f->self, "--launch-observer", NULL};
    const char *build[] = {u->launcher, "build", f->self, "--launch-observer", NULL};
    static const char *value[] = {"bigmem-boundary-v1", "yes", "bigmem-boundary-v2"};
    char policy[33000];
    unsigned i;
    if (!bad_policies(u) || !bad_environment(u))
        return 0;
    flow_put(f, "checkout/tests/system/suites.mk", "# fixed ports\n");
    if (!launch_run(u, a, 2) || fx_exists(flow_path(f, "observed")))
        return 0;
    flow_put(f, "checkout/tests/system/suites.mk", "# RACH2_PORT_OFFSET\n");
    for (i = 0; i < 3; i++) {
        snprintf(policy, sizeof(policy), "%sboundary_lazy %s\n", u->policy, value[i]);
        flow_put(f, "site", policy);
        if (!launch_run(u, build, i ? 2 : 0))
            return 0;
        if (!i && !launch_saw(u, 1, 1))
            return 0;
        if (i && fx_exists(flow_path(f, "observed")))
            return 0;
    }
    return flow_put(f, "site", u->policy);
}
