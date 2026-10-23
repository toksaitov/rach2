/* Check prepared host execution, inventory completeness and input identity.
 * host-deps: utils/sha256.c
 * host-cflags: -I utils -Wall -Wextra -Werror
 * host-inputs: build/tests/cases/host/host
 */
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
static pid_t active;
static char driver[PATH_MAX], self[PATH_MAX], tree[PATH_MAX];
#define CHECK(ok, why)                                      \
    do {                                                    \
        if (!(ok)) {                                        \
            fprintf(stderr, "etude-hostdriver: %s\n", why); \
            failures++;                                     \
        }                                                   \
    } while (0)

static void stopped(int sig)
{
    if (active > 0) {
        kill(active, SIGTERM);
        waitpid(active, NULL, 0);
    }
    exit(128 + sig);
}

static void put(const char *name, const char *text)
{
    FILE *f = fopen(fix_path(name), "wb");
    CHECK(f != NULL, "fixture file opens");
    if (f) {
        CHECK(fputs(text, f) >= 0, "fixture writes");
        CHECK(fclose(f) == 0, "fixture closes");
    }
}

static void hash(const char *name, char hex[65])
{
    struct sha256 sha;
    unsigned char bytes[8192], result[32];
    size_t n;
    FILE *f = fopen(fix_path(name), "rb");
    CHECK(f != NULL, "hashed input exists");
    sha256_init(&sha);
    if (f) {
        while ((n = fread(bytes, 1, sizeof(bytes), f)) != 0)
            sha256_update(&sha, bytes, n);
        CHECK(!ferror(f), "hashed input reads whole");
        fclose(f);
    }
    sha256_final(&sha, result);
    sha256_hex(result, hex);
}

static void copy_probe(void)
{
    char bytes[8192];
    size_t n;
    FILE *in = fopen(self, "rb"), *out = fopen(fix_path("build/tests/probe"), "wb");
    CHECK(in && out, "probe copy opens");
    if (in && out)
        while ((n = fread(bytes, 1, sizeof(bytes), in)) != 0)
            CHECK(fwrite(bytes, 1, n, out) == n, "probe copies whole");
    if (in)
        fclose(in);
    if (out)
        fclose(out);
    CHECK(chmod(fix_path("build/tests/probe"), 0700) == 0, "probe executable");
}

static void inventory(int omit, const char *unsupported)
{
    char manifest[65], program[65], source[65];
    hash("tests/cases/host.host.json", manifest);
    hash("build/tests/probe", program);
    hash("tests/host/a_hosttest.c", source);
    FILE *f = fopen(fix_path("build/tests/host-suite/tests.inventory.json"), "wb");
    CHECK(f != NULL, "inventory opens");
    if (!f)
        return;
    fprintf(f,
            "{\"schema_version\":1,\"group\":\"tests\",\"architecture\":\"i686\","
            "\"manifest_sha256\":\"%s\",\"checks\":[",
            manifest);
    for (int i = 0; i < (omit ? 1 : 2); i++)
        fprintf(f,
                "%s{\"assertion_id\":\"%c\",\"label\":\"probe %c\",\"mode\":\"shared\","
                "\"program\":\"build/tests/probe\",\"sha256\":\"%s\","
                "\"arguments\":[\"--probe\"],\"inputs\":[{\"path\":\"tests/host/a_hosttest.c\","
                "\"sha256\":\"%s\"}],\"unsupported\":\"%s\"}",
                i ? "," : "", 'a' + i, 'a' + i, program, source, unsupported ? unsupported : "");
    fputs("]}\n", f);
    CHECK(fclose(f) == 0, "inventory closes");
}

static int run(char *output, size_t room)
{
    pid_t child = fork();
    int status;
    CHECK(child >= 0, "native driver child starts");
    if (child == 0) {
        int fd = open(fix_path("output"), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0)
            _exit(120);
        close(fd);
        setenv("ETUDE_TREE", tree, 1);
        setenv("ETUDE_CASE", "host.host.tests", 1);
        setenv("ARCH", "i686", 1);
        setenv("TMPDIR", tree, 1);
        setenv("RACH2_WORKERS", "2", 1);
        setenv("ASAN_OPTIONS", "halt_on_error=0:exitcode=0", 1);
        setenv("UBSAN_OPTIONS", "halt_on_error=0:exitcode=0", 1);
        execl(driver, driver, "tests", (char *)NULL);
        _exit(121);
    }
    if (child < 0)
        return -1;
    active = child;
    CHECK(waitpid(child, &status, 0) == child, "native driver child reaped");
    active = 0;
    FILE *f = fopen(fix_path("output"), "rb");
    size_t n = f ? fread(output, 1, room - 1, f) : 0;
    output[n] = 0;
    if (f)
        fclose(f);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int probe(void)
{
    const char *tmp = getenv("TMPDIR"), *asan = getenv("ASAN_OPTIONS"),
               *ubsan = getenv("UBSAN_OPTIONS");
    char marker[PATH_MAX];
    if (!tmp || !asan || !ubsan || strcmp(asan, "halt_on_error=1:exitcode=1") ||
        strcmp(ubsan, "halt_on_error=1:print_stacktrace=1:exitcode=1") || getenv("ETUDE_TREE"))
        return 11;
    if (snprintf(marker, sizeof(marker), "%s/exclusive", tmp) >= (int)sizeof(marker))
        return 12;
    int fd = open(marker, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return 13;
    close(fd);
    puts("probe complete");
    return getenv("HOST_PROBE_FAIL") ? 17 : 0;
}

static void fixtures(void)
{
    const char *dirs[] = {"tests", "tests/cases", "tests/host",
                          "build", "build/tests", "build/tests/host-suite",
                          NULL};
    snprintf(tree, sizeof(tree), "%s", fix_dir());
    for (size_t i = 0; dirs[i]; i++)
        CHECK(mkdir(fix_path(dirs[i]), 0700) == 0, "fixture directory created");
    put("tests/host/a_hosttest.c", "/* prepared source a */\n");
    put("tests/host/b_hosttest.c", "/* prepared source b */\n");
    put("tests/cases/host.host.json",
        "{\"cases\":[{\"case_id\":\"host.host.tests\","
        "\"assertions\":[{\"assertion_id\":\"listed\"},{\"assertion_id\":\"a\"},"
        "{\"assertion_id\":\"b\"}]}]}\n");
    copy_probe();
}

static void checks(void)
{
    char output[32768];
    CHECK(run(output, sizeof(output)) == 2, "unprepared inventory is rejected");
    inventory(0, NULL);
    CHECK(run(output, sizeof(output)) == 0 && strstr(output, "assert a pass") &&
              strstr(output, "assert b pass") && strstr(output, "assert listed pass"),
          "prepared binaries execute with isolated scratch and fatal sanitizer settings");
    setenv("HOST_PROBE_FAIL", "1", 1);
    CHECK(run(output, sizeof(output)) == 1 && strstr(output, "assert a fail") &&
              strstr(output, "probe complete"),
          "a failed binary retains its whole evidence");
    unsetenv("HOST_PROBE_FAIL");
    put("tests/host/a_hosttest.c", "/* changed source */\n");
    CHECK(run(output, sizeof(output)) == 1 && strstr(output, "input changed"),
          "changed source is refused before execution");
    inventory(1, NULL);
    CHECK(run(output, sizeof(output)) == 1 && strstr(output, "assert b fail") &&
              strstr(output, "assert listed fail"),
          "missing prepared assertion fails explicitly");
    inventory(0, "a native adapter is required");
    CHECK(run(output, sizeof(output)) == 1 && strstr(output, "native adapter is required") &&
              !strstr(output, "probe complete"),
          "unsupported checks cannot become passes");
    inventory(0, NULL);
    put("tests/host/c_hosttest.c", "/* new source */\n");
    CHECK(run(output, sizeof(output)) == 1 && strstr(output, "assert listed fail"),
          "new source cannot disappear behind an old prepared inventory");
    CHECK(unlink(fix_path("tests/host/c_hosttest.c")) == 0, "new source removed");
    put("build/tests/probe", "#!/bin/sh\nexit 0\n");
    inventory(0, NULL);
    CHECK(run(output, sizeof(output)) == 1 && strstr(output, "program is a script"),
          "a matching script digest cannot bypass native execution");
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--probe"))
        return probe();
    signal(SIGINT, stopped);
    signal(SIGTERM, stopped);
    CHECK(realpath("build/tests/cases/host/host", driver) != NULL, "native driver is prepared");
    CHECK(realpath(argv[0], self) != NULL, "probe path resolves");
    if (!failures) {
        fixtures();
        checks();
    }
    if (!failures)
        fix_passed();
    return failures ? 1 : 0;
}
