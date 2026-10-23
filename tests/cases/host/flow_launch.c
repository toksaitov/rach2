/* flow_launch.c: compiled command fixtures observe actual admission locks. */
#define _POSIX_C_SOURCE 200809L
#include "flow_launch.h"
#include "sha256.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static const char *ev(const char *key)
{
    const char *v = getenv(key);
    return v ? v : "";
}
static const char *lp(const char *name)
{
    static char paths[16][4096];
    static unsigned next;
    char *p = paths[next++ % 16];
    if (snprintf(p, 4096, "%s/%s", ev("FLOW_LAUNCH"), name) >= 4096)
        exit(2);
    return p;
}
static unsigned occurrences(const char *text, const char *word)
{
    unsigned n = 0;
    for (; (text = strstr(text, word)); text += strlen(word))
        n++;
    return n;
}
static int locked(const char *path)
{
    struct flock lock;
    int fd = open(path, O_RDWR), ok;
    if (fd < 0)
        return 0;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    ok = fcntl(fd, F_SETLK, &lock) < 0 && (errno == EACCES || errno == EAGAIN);
    close(fd);
    return ok;
}
static int observer(void)
{
    char *text = fx_read(lp("ledger/claims")), wanted[120], name[100], *end;
    unsigned char digest[32];
    struct sha256 sha;
    char hex[65];
    long offset = strtol(ev("RACH2_PORT_OFFSET"), &end, 10);
    FILE *out;
    int ok;
    if (!text)
        return 1;
    snprintf(wanted, sizeof(wanted), "resource memory_bytes %s\n", ev("EXPECT_MEMORY"));
    ok = occurrences(text, "claim ") == 1 && strstr(text, "process case ") &&
         strstr(text, "state running") && !strstr(text, "resource cpu_millis ") &&
         !strstr(text, "resource disk_bytes ") && strstr(text, wanted) &&
         !strcmp(ev("RACH2_WORKERS"), "2") && !strcmp(ev("RACH2_MEMORY_MIB"), "96") &&
         *ev("RACH2_PORT_OFFSET") && !*end && offset >= 0 && offset <= 9000 &&
         offset % 1000 == 0 && locked(lp("locks/rach2-host.lock"));
    snprintf(name, sizeof(name), "locks/rach2-ports.%ld", offset / 1000);
    ok = ok && locked(lp(name));
    if (!ok) {
        fprintf(stderr, "launcher observer: invalid claim or missing lock:\n%s", text);
        free(text);
        return 1;
    }
    sha256_init(&sha);
    sha256_update(&sha, (const unsigned char *)text, strlen(text));
    sha256_final(&sha, digest);
    sha256_hex(digest, hex);
    free(text);
    out = fopen(lp("observed"), "a");
    if (!out)
        return 1;
    ok = fprintf(out, "%ld %s\n", offset, hex) > 0;
    return fclose(out) || !ok;
}
static int waiting_workload(int observe)
{
    if (observe && observer())
        return 1;
    if (!observe && strcmp(ev("RACH2_WORKERS"), *ev("TEST_WORKERS") ? ev("TEST_WORKERS") : "2"))
        return 1;
    if (!fx_text(ev("TEST_READY"), ev("RACH2_PORT_OFFSET")))
        return 1;
    while (!fx_exists(ev("TEST_RELEASE")))
        fx_pause();
    return 0;
}
int flow_launch_aux(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    if (!strcmp(argv[1], "--launch-observer"))
        return observer();
    if (!strcmp(argv[1], "--launch-wait"))
        return waiting_workload(1);
    return 2;
}
static int synthetic_run(int argc, char **argv)
{
    const char *mode = ev("FLOW_LAUNCH_MODE");
    int i;
    if (!strcmp(mode, "parallel"))
        return waiting_workload(0);
    if (!strcmp(mode, "ci") && !*ev("ETUDE_ADMITTED")) {
        char **a = calloc((size_t)argc + 3, sizeof(*a));
        if (!a)
            return 2;
        a[0] = (char *)lp("launch");
        a[1] = "legacy";
        for (i = 0; i < argc; i++)
            a[i + 2] = argv[i];
        execv(a[0], a);
        free(a);
        return 127;
    }
    if (observer())
        return 3;
    for (i = 1; i < argc; i++)
        printf("run: %s: PASS in 0s\n", !strcmp(argv[i], "core") ? "default" : argv[i]);
    return 0;
}
static int owner_ssh(int argc, char **argv)
{
    char b[4096];
    if (argc != 5)
        return 255;
    if (strstr(argv[4], "bash -s ")) {
        puts("held");
        fflush(stdout);
        while (read(0, b, sizeof(b)) > 0) {
        }
        return 0;
    }
    execl("/bin/bash", "bash", "-c", argv[4], (char *)NULL);
    return 127;
}
static int record_make(int argc, char **argv)
{
    FILE *out = fopen(lp("job-args"), "w");
    int i, ok = 1;
    if (!out)
        return 1;
    for (i = 1; i < argc; i++)
        if (fprintf(out, "%s\n", argv[i]) < 0)
            ok = 0;
    return fclose(out) || !ok;
}
static int launch_commands(int argc, char **argv, const char *name)
{
    int i;
    if (!strcmp(name, "make")) {
        if (!strcmp(ev("FLOW_LAUNCH_MODE"), "jobs"))
            return record_make(argc, argv);
        if (!strcmp(ev("FLOW_LAUNCH_MODE"), "ci"))
            execl(lp("launch"), "launch", "build", ev("FLOW_SELF"), "--launch-observer",
                  (char *)NULL);
        return observer();
    }
    if (!strcmp(name, "cc"))
        return observer() ? 3 : 19;
    if (!strcmp(name, "old-run"))
        return observer();
    if (!strcmp(name, "record-make"))
        return record_make(argc, argv);
    if (!strcmp(name, "gh")) {
        for (i = 1; i + 1 < argc; i++)
            if (!strcmp(argv[i], "--json") && !strcmp(argv[i + 1], "state"))
                puts("OPEN");
        return 0;
    }
    if (!strcmp(name, "git") && argc > 1) {
        if (!strcmp(argv[1], "rev-parse"))
            puts("bad");
        else if (!strcmp(argv[1], "rev-list"))
            puts("good");
        else if (!strcmp(argv[1], "log"))
            puts("bad fixture");
        else if (strcmp(argv[1], "checkout"))
            return 2;
        return 0;
    }
    return 97;
}
int flow_launch_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    int r;
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "run"))
        return synthetic_run(argc, argv);
    if (!strcmp(name, "sh")) {
        if (argc > 1 && !strcmp(argv[1], "tests/run") &&
            strcmp(ev("FLOW_LAUNCH_MODE"), "worker"))
            return synthetic_run(argc - 1, argv + 1);
        execv("/bin/sh", argv);
        return 127;
    }
    if (!strcmp(name, "ssh"))
        return owner_ssh(argc, argv);
    if (!strcmp(name, "rsync")) {
        if (argc == 2 && !strcmp(argv[1], "--version"))
            puts("rsync  version 3.2.7");
        return 0;
    }
    if (!strcmp(name, "wsl.exe") && argc > 2) {
        execvp(argv[2], argv + 2);
        return 127;
    }
    if (!strcmp(name, "timeout") && argc > 2) {
        int first = !strcmp(argv[1], "-k") ? 4 : 2;
        if (first >= argc)
            return 2;
        execvp(argv[first], argv + first);
        return 127;
    }
    if (!strcmp(name, "uname") && argc == 2 && !strcmp(argv[1], "-s")) {
        puts("Linux");
        return 0;
    }
    if (!strcmp(name, "id") && argc == 2 && !strcmp(argv[1], "-un")) {
        puts(*ev("TEST_ACCOUNT") ? ev("TEST_ACCOUNT") : "test");
        return 0;
    }
    if (!strcmp(name, "uname") || !strcmp(name, "id")) {
        char executable[80];
        snprintf(executable, sizeof(executable), "/usr/bin/%s", name);
        execv(executable, argv);
        return 127;
    }
    r = launch_commands(argc, argv, name);
    return r;
}
int launch_run(struct launch_fixture *u, const char *const argv[], int status)
{
    return fx_check(u->f, fx_run(u->f, argv, u->tree, NULL, NULL) == status,
                    "launcher command status");
}
int launch_clear(struct launch_fixture *u)
{
    int ok = fx_check(u->f, fx_equal(flow_path(u->f, "ledger/claims"), "etude-ledger 1\n"),
                      "launcher released durable claims");
    fx_remove(flow_path(u->f, "observed"));
    return ok;
}
int launch_saw(struct launch_fixture *u, unsigned count, int same)
{
    char *text = fx_read(flow_path(u->f, "observed")), *save = NULL, *line;
    char hashes[32][65];
    unsigned n = 0, i;
    int ok = text != NULL;
    for (line = text ? strtok_r(text, "\n", &save) : NULL; line;
         line = strtok_r(NULL, "\n", &save)) {
        char *hash = strchr(line, ' ');
        if (!hash || strlen(hash + 1) != 64) {
            ok = 0;
            continue;
        }
        if (n == 32) {
            ok = 0;
            break;
        }
        for (i = 0; i < n; i++)
            if ((same && strcmp(hashes[i], hash + 1)) ||
                (!same && !strcmp(hashes[i], hash + 1)))
                ok = 0;
        strcpy(hashes[n++], hash + 1);
    }
    free(text);
    return fx_check(u->f, ok && n == count, "claim count and nested identity") && launch_clear(u);
}
static int setup(struct launch_fixture *u)
{
    struct fixture *f = u->f;
    static const char *clear[] = {"ETUDE_ADMITTED", "ETUDE_SITE", "RACH2_PORT_OFFSET",
        "SSH_TEST_PORT", "RACH2_RUN_WORK", "RACH2_RUN_STATUS", "MAKEFLAGS", "MFLAGS",
        "MAKELEVEL", "MAKEOVERRIDES", "ETUDE_BUILD_JOBS", "JOBS", NULL};
    static const char *mock[] = {"uname", "cc", "make", "gh", "git", "timeout", "id", "sh",
        "ssh", "wsl.exe", "rsync", "record-make", NULL};
    unsigned i;
    for (i = 0; clear[i]; i++)
        fx_env(clear[i], NULL);
    snprintf(u->tree, sizeof(u->tree), "%s/checkout", f->work);
    fx_mkdir(u->tree);
    fx_mkdir(flow_path(f, "ledger"));
    fx_mkdir(flow_path(f, "locks"));
    flow_put(f, "ledger/claims", "etude-ledger 1\n");
    if (!flow_copy(f, "tools/etude/launch.sh", "launch") ||
        !flow_copy(f, "tools/etude/make.sh", "etude-bin/make"))
        return 0;
    chmod(flow_path(f, "launch"), 0755);
    chmod(flow_path(f, "etude-bin/make"), 0755);
    snprintf(u->policy, sizeof(u->policy),
        "etude-site 2\nendpoint linux\nlauncher %s/launch\nledger %s/ledger\nlocks %s/locks\n"
        "etude /host-products/etude\ncapacity 24000,103079215104,171798691840\nwait 3\nseconds 30\n",
        f->work, f->work, f->work);
    flow_put(f, "site", u->policy);
    fx_env("FLOW_LAUNCH", f->work);
    fx_env("ETUDE_SITE", flow_path(f, "site"));
    fx_env("ETUDE_STATE", flow_path(f, "private-state"));
    fx_env("RACH2_WORKERS", "2");
    fx_env("RACH2_MEMORY_MIB", "96");
    fx_env("RACH2_CPUS", "2");
    fx_env("EXPECT_MEMORY", "5368709120");
    for (i = 0; mock[i]; i++)
        if (!fx_mock(f, mock[i]))
            return 0;
    return flow_put(f, "checkout/tests/system/suites.mk", "# RACH2_PORT_OFFSET\n") &&
           flow_put(f, "checkout/tests/registry", "style|-|full\nhost|-|full\ndefault|qemu|core\n");
}
int flow_launch(struct fixture *f)
{
    struct launch_fixture u;
    u.f = f;
    if (!setup(&u))
        return 0;
    if (!launch_entries(&u) || !launch_refusals(&u))
        return 0;
    return launch_parallel(&u);
}
