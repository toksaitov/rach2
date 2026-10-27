/* flow_launch_parallel.c: shared launches overlap; exclusive ones wait. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "flow_launch.h"
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
static unsigned long long physical_memory(void)
{
#ifdef __APPLE__
    unsigned long long memory = 0;
    size_t bytes = sizeof(memory);
    if (sysctlbyname("hw.memsize", &memory, &bytes, NULL, 0))
        return 0;
    return memory;
#else
    return (unsigned long long)sysconf(_SC_PHYS_PAGES) * (unsigned long long)sysconf(_SC_PAGESIZE);
#endif
}
static unsigned count(const char *s, const char *word)
{
    unsigned n = 0;
    for (; (s = strstr(s, word)); s += strlen(word))
        n++;
    return n;
}
static int alive(struct fx_child *children, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (fx_poll(&children[i]))
            return 0;
    return 1;
}
/** Wait until a payload has written its whole ready line, its port offset and
 * a newline, since the file exists before its text does. Fail when one of the
 * `n` launches ends first. */
static int published(struct launch_fixture *u, struct fx_child *p, unsigned n, const char *path)
{
    char *text;
    while (!(text = fx_read(path)) || !strchr(text, '\n')) {
        free(text);
        if (!alive(p, n))
            return fx_check(u->f, 0, "launch ended before publishing ready state");
        fx_pause();
    }
    free(text);
    return 1;
}
static int blocked(struct launch_fixture *u, struct fx_child *p, unsigned n)
{
    for (;;) {
        char *text = fx_read(flow_path(u->f, "ledger/claims"));
        int found = text && (strstr(text, "state blocked") || strstr(text, "state waiting"));
        free(text);
        if (found)
            return 1;
        if (!alive(p, n))
            return fx_check(u->f, 0, "launch ended before recording blocked state");
        fx_pause();
    }
}
static int concurrent_start(struct launch_fixture *u, struct fx_child *p, unsigned i,
                            const char *kind, const char *suite)
{
    struct fixture *f = u->f;
    char ready[4096], release[4096], workers[40];
    const char *a[] = {u->launcher, "legacy", "tests/run", suite, NULL};
    const char *env[] = {ready, release, workers, NULL};
    snprintf(ready, sizeof(ready), "TEST_READY=%s/ready-%s-%u", f->work, kind, i);
    snprintf(release, sizeof(release), "TEST_RELEASE=%s/release-%s-%s", f->work,
             i == 2 ? "only" : "shared", kind);
    snprintf(workers, sizeof(workers), "TEST_WORKERS=%u",
             i == 2 && !strcmp(kind, "serial") ? 1 : 2);
    return fx_start(f, p, a, u->tree, env, NULL);
}
/** Check that two launches hold claims of their own, each of this account. */
static int ordinary_claims(struct launch_fixture *u)
{
    char *text = fx_read(flow_path(u->f, "ledger/claims")), account[300];
    struct passwd *pw = getpwuid(geteuid());
    snprintf(account, sizeof(account), "account %s\n", pw ? pw->pw_name : "");
    int ok = text && pw && count(text, "state running") == 2 && count(text, "claim launch:") == 2 &&
             count(text, account) == 2 && count(text, "resource memory_bytes 5368709120\n") == 2;
    free(text);
    return fx_check(u->f, ok, "two ordinary launches hold independent shared claims");
}
static int exclusive_claim(struct launch_fixture *u)
{
    char *text = fx_read(flow_path(u->f, "ledger/claims"));
    int ok = text && count(text, "state running") == 1 &&
             strstr(text, "resource memory_bytes 103079215104\n");
    free(text);
    return fx_check(u->f, ok, "exclusive launch takes whole site capacity");
}
static int parallel_row(struct launch_fixture *u, const char *kind)
{
    struct fixture *f = u->f;
    struct fx_child children[3];
    char path[4096], ready[3][4096], registry[300];
    unsigned i;
    unsigned long long memory;
    if (!strcmp(kind, "serial"))
        strcpy(registry, "host|-|full||384\nexclusive|-|full||128||serial\n");
    else {
        memory = physical_memory();
        if (!memory)
            return 0;
        snprintf(registry, sizeof(registry), "host|-|full||384\nexclusive|-|full||%llu\n",
                 memory / 1048576 / 2 + 1);
    }
    flow_put(f, "checkout/tests/registry", registry);
    for (i = 0; i < 3; i++)
        snprintf(ready[i], sizeof(ready[i]), "%s/ready-%s-%u", f->work, kind, i);
    for (i = 0; i < 2; i++)
        if (!concurrent_start(u, &children[i], i, kind, "host"))
            return 0;
    for (i = 0; i < 2; i++)
        if (!published(u, children, 2, ready[i]))
            return 0;
    if (!ordinary_claims(u) || !fx_check(f, !fx_same(ready[0], ready[1]), "port blocks differ") ||
        !concurrent_start(u, &children[2], 2, kind, "exclusive") || !blocked(u, children, 3) ||
        !fx_check(f, !fx_exists(ready[2]), "exclusive payload waits behind ordinary launches"))
        return 0;
    snprintf(path, sizeof(path), "%s/release-shared-%s", f->work, kind);
    fx_text(path, "");
    for (i = 0; i < 2; i++)
        if (fx_wait(f, &children[i]))
            return 0;
    if (!published(u, &children[2], 1, ready[2]) || !exclusive_claim(u))
        return 0;
    snprintf(path, sizeof(path), "%s/release-only-%s", f->work, kind);
    fx_text(path, "");
    return !fx_wait(f, &children[2]) && launch_clear(u);
}
static int cancellation(struct launch_fixture *u)
{
    struct fixture *f = u->f;
    struct fx_child children[2];
    const char *first[] = {u->launcher, "alone", f->self, "--launch-wait", NULL};
    const char *second[] = {u->launcher, "alone", f->self, "--launch-observer", NULL};
    char *observed;
    int ok;
    fx_env("EXPECT_MEMORY", "103079215104");
    fx_env("TEST_READY", flow_path(f, "ready"));
    fx_env("TEST_RELEASE", flow_path(f, "release"));
    if (!fx_start(f, &children[0], first, u->tree, NULL, NULL) ||
        !published(u, children, 1, flow_path(f, "ready")) ||
        !fx_start(f, &children[1], second, u->tree, NULL, NULL) || !blocked(u, children, 2))
        return 0;
    observed = fx_read(flow_path(f, "observed"));
    ok = fx_check(f, observed && count(observed, "\n") == 1, "blocked payload never executes");
    free(observed);
    /* The original cancellation sends TERM to the launcher, not its group. */
    kill(children[1].pid, SIGTERM);
    ok = fx_check(f, fx_wait(f, &children[1]) == 143, "waiting cancellation status") && ok;
    kill(children[0].pid, SIGTERM);
    ok = fx_check(f, fx_wait(f, &children[0]) == 143, "running cancellation status") && ok;
    return launch_clear(u) && ok;
}
int launch_parallel(struct launch_fixture *u)
{
    fx_env("FLOW_LAUNCH_MODE", "parallel");
    fx_copy(u->f->self, flow_path(u->f, "checkout/tests/run"));
    return parallel_row(u, "serial") && parallel_row(u, "large") && cancellation(u);
}
