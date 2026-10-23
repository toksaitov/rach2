/* Check native collection selection, prepared execution and learned bounds.
 * host-deps: utils/sha256.c
 * host-cflags: -I utils -Wall -Wextra -Werror
 * host-inputs: build/tools/etude/etude
 * host-inputs: tools/etude/fixtures/verdict/cases/fixture.default.json
 * host-inputs: tools/etude/fixtures/verdict/cases/fixture.host.json
 * host-inputs: tools/etude/schemas/case.schema.json
 * host-inputs: tools/etude/schemas/history.schema.json
 * host-inputs: tools/etude/schemas/results.schema.json
 *
 * The real etude selects a fixture collection without a shell or Python,
 * then runs this binary as its prepared host case. Successful runs teach
 * its bounds; a failure and a different processor profile do not. The
 * complete output survives the executor's capture bound. A script artifact
 * and a changed artifact are refused before execution.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "compat/fixdir_host.c"
#include "compat/pathmax_host.h"
#include "sha256.h"

static int failures;
static char etude[PATH_MAX], self[PATH_MAX], tree[PATH_MAX];
#define CHECK(ok, why)                                      \
    do {                                                    \
        if (!(ok)) {                                        \
            fprintf(stderr, "etude-collection: %s\n", why); \
            failures++;                                     \
        }                                                   \
    } while (0)

static void stopped(int sig)
{
    exit(128 + sig);
}

static void copy(const char *from, const char *to)
{
    char bytes[8192];
    size_t n;
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    CHECK(in && out, "fixture file opens");
    if (in && out)
        while ((n = fread(bytes, 1, sizeof(bytes), in)) != 0)
            CHECK(fwrite(bytes, 1, n, out) == n, "fixture file copies whole");
    if (in)
        fclose(in);
    if (out)
        fclose(out);
}

static void put(const char *name, const char *text)
{
    FILE *f = fopen(fix_path(name), "wb");
    CHECK(f != NULL, "fixture text opens");
    if (f) {
        CHECK(fputs(text, f) >= 0, "fixture text writes");
        CHECK(fclose(f) == 0, "fixture text closes");
    }
}

static void make_record(void)
{
    const char *program = fix_path("build/tests/host/sha256_hosttest");
    struct stat st;
    unsigned char bytes[8192], hash[32];
    char hex[65];
    size_t n;
    struct sha256 digest;
    FILE *f = fopen(program, "rb"), *record;
    CHECK(f != NULL && stat(program, &st) == 0, "prepared binary exists");
    if (!f)
        return;
    sha256_init(&digest);
    while ((n = fread(bytes, 1, sizeof(bytes), f)) != 0)
        sha256_update(&digest, bytes, n);
    fclose(f);
    sha256_final(&digest, hash);
    sha256_hex(hash, hex);
    record = fopen(fix_path("build/tests/prepare.record"), "wb");
    CHECK(record != NULL, "prepared record opens");
    if (record) {
        /* A deployed record may come from another production path. */
        fprintf(record,
                "etude-prepare 1\ntree /producer/rach2\narch i686\nkernel build/rach2/kernel\n"
                "file %s %llu 0 0 build/tests/host/sha256_hosttest\n",
                hex, (unsigned long long)st.st_size);
        fclose(record);
    }
}

static int run(char *const *options, const char *cases, const char *rows, const char *failure,
               char *out, size_t room)
{
    char *argv[40];
    size_t n = 0, got;
    int status;
    pid_t child;
    argv[n++] = etude;
    argv[n++] = "run";
    argv[n++] = "-t";
    argv[n++] = tree;
    for (size_t i = 0; options[i]; i++)
        argv[n++] = options[i];
    argv[n] = NULL;
    child = fork();
    CHECK(child >= 0, "collection child starts");
    if (child == 0) {
        int fd = open(fix_path("output"), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0)
            _exit(120);
        close(fd);
        unsetenv("RACH2_CASES");
        unsetenv("RACH2_AGENTS_ROWS");
        unsetenv("RACH2_ACCEPT_AGENTS");
        unsetenv("RACH2_PORT_OFFSET");
        unsetenv("SSH_TEST_PORT");
        unsetenv("ETUDE_SITE");
        unsetenv("ETUDE_ADMITTED");
        unsetenv("RACH2_TREE_HELD");
        unsetenv("RACH2_WORKERS");
        unsetenv("RACH2_MEMORY_MIB");
        unsetenv("RACH2_CPUS");
        if (cases)
            setenv("RACH2_CASES", cases, 1);
        if (rows)
            setenv("RACH2_AGENTS_ROWS", rows, 1);
        if (failure)
            setenv("ETUDE_COLLECTION_FAIL", failure, 1);
        else
            unsetenv("ETUDE_COLLECTION_FAIL");
        execv(etude, argv);
        _exit(127);
    }
    if (child < 0)
        return -1;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    FILE *f = fopen(fix_path("output"), "rb");
    got = f ? fread(out, 1, room - 1, f) : 0;
    if (f)
        fclose(f);
    out[got] = 0;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int occurrences(const char *text, const char *word)
{
    int n = 0;
    for (const char *p = text; (p = strstr(p, word)) != NULL; p += strlen(word))
        n++;
    return n;
}

static void fixture(void)
{
    static const char *const dirs[] = {"tests",       "tests/cases",         "tools",
                                       "tools/etude", "tools/etude/schemas", "build",
                                       "build/tests", "build/tests/host"};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        CHECK(mkdir(fix_path(dirs[i]), 0700) == 0, "fixture directory is made");
    put("tests/registry", "default|-|core\nhost|-|core\nagents|-|full|||x86_64\n");
    copy("tools/etude/fixtures/verdict/cases/fixture.default.json",
         fix_path("tests/cases/fixture.default.json"));
    copy("tools/etude/fixtures/verdict/cases/fixture.host.json",
         fix_path("tests/cases/fixture.host.json"));
    copy("tools/etude/schemas/case.schema.json", fix_path("tools/etude/schemas/case.schema.json"));
    copy("tools/etude/schemas/history.schema.json",
         fix_path("tools/etude/schemas/history.schema.json"));
    copy("tools/etude/schemas/results.schema.json",
         fix_path("tools/etude/schemas/results.schema.json"));
    copy(self, fix_path("build/tests/host/sha256_hosttest"));
    CHECK(chmod(fix_path("build/tests/host/sha256_hosttest"), 0700) == 0,
          "prepared binary is executable");
    make_record();
}

int main(int argc, char **argv)
{
    static char out[131072];
    char *both[] = {"-N", "-a", "both", "core", NULL};
    char *single[] = {"-N", "default", "host", NULL};
    char *duplicate[] = {"-N", "host", "host", NULL};
    char *unknown[] = {"-N", "absent", NULL};
    char *native[] = {"-A", "tcg", "-C", "2", "host", NULL};
    char *other[] = {"-A", "tcg", "-C", "3", "host", NULL};
    if (argc == 1 && getenv("ETUDE_CASE") &&
        !strcmp(getenv("ETUDE_CASE"), "fixture.host.vectors")) {
        printf("@etude 1 begin fixture.host.vectors\n");
        for (size_t i = 0; i < 10000; i++)
            putchar('x');
        printf("\n@etude 1 assert empty %s\n@etude 1 assert million pass\n@etude 1 end 2\n",
               getenv("ETUDE_COLLECTION_FAIL") ? "fail" : "pass");
        return getenv("ETUDE_COLLECTION_FAIL") ? 1 : 0;
    }
    if (!realpath(argv[0], self) || !realpath("build/tools/etude/etude", etude))
        return 1;
    signal(SIGINT, stopped);
    signal(SIGTERM, stopped);
    signal(SIGHUP, stopped);
    snprintf(tree, sizeof(tree), "%s", fix_dir());
    fixture();
    CHECK(run(both, NULL, NULL, NULL, out, sizeof(out)) == 0 &&
              strstr(out, "default i686 fixture.default.errno") &&
              strstr(out, "default x86_64 fixture.default.errno"),
          "both architectures share the selection");
    CHECK(run(single, "fixture.default.errno fixture.host.vectors", NULL, NULL, out, sizeof(out)) ==
              0,
          "identities select one case of every named suite");
    CHECK(run(single, "fixture.default.errno", NULL, NULL, out, sizeof(out)) == 2,
          "a selection cannot leave one named suite empty");
    CHECK(run(single, "fixture.default.errno fixture.default.errno", NULL, NULL, out,
              sizeof(out)) == 2,
          "duplicate identities are refused");
    CHECK(run(duplicate, NULL, NULL, NULL, out, sizeof(out)) == 2, "duplicate suites are refused");
    CHECK(run(unknown, NULL, NULL, NULL, out, sizeof(out)) == 2, "unknown suites are refused");
    for (size_t i = 0; i < 6; i++) {
        int rc = run(native, NULL, NULL, NULL, out, sizeof(out));
        if (rc != 0)
            fprintf(stderr, "%s", out);
        CHECK(rc == 0, "prepared native case passes with complete output");
        CHECK(occurrences(out, "xxxxx") > 1000,
              "output beyond the capture bound reaches evidence and the judge");
    }
    CHECK(strstr(out, "(5 samples)") != NULL && strstr(out, "case 30000000 us") == NULL,
          "five matching successes change the planned case and gap bounds");
    CHECK(run(other, NULL, NULL, NULL, out, sizeof(out)) == 0 &&
              strstr(out, "case 30000000 us (0 samples)"),
          "a different processor profile cannot teach the bound");
    CHECK(run(native, NULL, NULL, "yes", out, sizeof(out)) == 1,
          "a native failed assertion remains a failure");
    CHECK(run(native, NULL, NULL, NULL, out, sizeof(out)) == 0 && strstr(out, "(6 samples)"),
          "the failed attempt adds no successful sample");
    put("build/tests/host/sha256_hosttest", "#!/bin/sh\nexit 0\n");
    make_record();
    CHECK(run(native, NULL, NULL, NULL, out, sizeof(out)) == 3 &&
              strstr(out, "requires a compiled program"),
          "portable execution rejects scripts even on POSIX");
    put("build/tests/host/sha256_hosttest", "changed\n");
    CHECK(run(native, NULL, NULL, NULL, out, sizeof(out)) == 3 && strstr(out, "changed"),
          "prepared artifacts are checked by digest before launch");
    if (failures)
        return 1;
    fix_passed();
    puts("etude_collection_hosttest: pass");
    return 0;
}
