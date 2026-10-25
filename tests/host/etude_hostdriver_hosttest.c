/* Check prepared host execution, inventory completeness and input identity.
 * host-deps: utils/sha256.c
 * host-cflags: -I utils -Wall -Wextra -Werror
 * host-inputs: build/tests/cases/host/host
 * host-inputs: tests/cases/host.host.json tests/cases/host/host.list
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
static const char *extra_input;
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

static void inventory(const char *group, int omit, const char *unsupported)
{
    char manifest[65], program[65], source[65], extra[65];
    hash("tests/cases/host.host.json", manifest);
    hash("build/tests/probe", program);
    hash("tests/host/a_hosttest.c", source);
    hash("tests/host/\303\251.txt", extra);
    char path[160];
    snprintf(path, sizeof(path), "build/tests/host-suite/%s.inventory.json", group);
    FILE *f = fopen(fix_path(path), "wb");
    CHECK(f != NULL, "inventory opens");
    if (!f)
        return;
    fprintf(f,
            "{\"schema_version\":1,\"group\":\"%s\",\"architecture\":\"%s\","
            "\"manifest_sha256\":\"%s\",\"checks\":[",
            group, !strcmp(group, "tests") ? "i686" : "none", manifest);
    for (int i = 0; i < (omit ? 1 : 2); i++) {
        fprintf(f,
                "%s{\"assertion_id\":\"%c\",\"label\":\"probe %c\",\"mode\":\"shared\","
                "\"program\":\"build/tests/probe\",\"sha256\":\"%s\","
                "\"arguments\":[\"--probe\"],\"inputs\":[{\"path\":\"tests/host/a_hosttest.c\","
                "\"sha256\":\"%s\"}",
                i ? "," : "", 'a' + i, 'a' + i, program, source);
        if (extra_input)
            fprintf(f, ",{%s,\"sha256\":\"%s\"}", extra_input, extra);
        fprintf(f, "],\"unsupported\":\"%s\"}", unsupported ? unsupported : "");
    }
    fputs("]}\n", f);
    CHECK(fclose(f) == 0, "inventory closes");
}

static int run_group(char *output, size_t room, const char *group, const char *only)
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
        char case_id[128];
        snprintf(case_id, sizeof(case_id), "host.host.%s", group);
        if (only)
            unsetenv("ETUDE_CASE");
        else
            setenv("ETUDE_CASE", case_id, 1);
        setenv("ARCH", "i686", 1);
        setenv("TMPDIR", tree, 1);
        setenv("RACH2_WORKERS", "2", 1);
        setenv("ASAN_OPTIONS", "halt_on_error=0:exitcode=0", 1);
        setenv("UBSAN_OPTIONS", "halt_on_error=0:exitcode=0", 1);
        if (only)
            execl(driver, driver, "--one", group, only, (char *)NULL);
        else
            execl(driver, driver, group, (char *)NULL);
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

static int run(char *output, size_t room)
{
    return run_group(output, room, "tests", NULL);
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
    put("tests/host/\303\251.txt", "non-ASCII fixture name\n");
    put("tests/cases/host.host.json",
        "{\"cases\":[{\"case_id\":\"host.host.tests\","
        "\"assertions\":[{\"assertion_id\":\"listed\"},{\"assertion_id\":\"a\"},"
        "{\"assertion_id\":\"b\"}]}]}\n");
    copy_probe();
}

static void encoded_inputs(void)
{
    char output[32768];
    static const char *bad[] = {"\"path_hex\":\"74657374732f686f73742fc3a92e74787\"",
                                "\"path_hex\":\"gg\"",
                                "\"path_hex\":\"610062\"",
                                "\"path_hex\":\"2e2e2f6f757473696465\"",
                                "\"path\":\"tests/host/a_hosttest.c\",\"path_hex\":\"61\"",
                                "\"path\":\"tests/host/a_hosttest.c\",\"path_hex\":null"};
    extra_input = "\"path_hex\":\"74657374732f686f73742fc3a92e747874\"";
    inventory("tests", 0, NULL);
    CHECK(run(output, sizeof(output)) == 0, "encoded filename bytes are verified");
    put("tests/host/\303\251.txt", "changed fixture\n");
    CHECK(run(output, sizeof(output)) == 1 && strstr(output, "input changed"),
          "changed encoded input is refused before execution");
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        extra_input = bad[i];
        inventory("tests", 0, NULL);
        CHECK(run(output, sizeof(output)) == 1 && strstr(output, "input changed"),
              "malformed or unsafe encoded input is rejected");
    }
    extra_input = NULL;
}

static void checks(void)
{
    char output[32768];
    CHECK(run(output, sizeof(output)) == 2, "unprepared inventory is rejected");
    inventory("tests", 0, NULL);
    CHECK(run(output, sizeof(output)) == 0 && strstr(output, "assert a pass") &&
              strstr(output, "assert b pass") && strstr(output, "assert listed pass"),
          "prepared binaries execute with isolated scratch and fatal sanitizer settings");
    encoded_inputs();
    inventory("tests", 0, NULL);
    setenv("HOST_PROBE_FAIL", "1", 1);
    CHECK(run(output, sizeof(output)) == 1 && strstr(output, "assert a fail") &&
              strstr(output, "probe complete"),
          "a failed binary retains its whole evidence");
    unsetenv("HOST_PROBE_FAIL");
    put("tests/host/a_hosttest.c", "/* changed source */\n");
    CHECK(run(output, sizeof(output)) == 1 && strstr(output, "input changed"),
          "changed source is refused before execution");
    inventory("tests", 1, NULL);
    CHECK(run(output, sizeof(output)) == 1 && strstr(output, "assert b fail") &&
              strstr(output, "assert listed fail"),
          "missing prepared assertion fails explicitly");
    inventory("tests", 0, "a native adapter is required");
    CHECK(run(output, sizeof(output)) == 1 && strstr(output, "native adapter is required") &&
              !strstr(output, "probe complete"),
          "unsupported checks cannot become passes");
    inventory("tests", 0, NULL);
    put("tests/host/c_hosttest.c", "/* new source */\n");
    CHECK(run(output, sizeof(output)) == 1 && strstr(output, "assert listed fail"),
          "new source cannot disappear behind an old prepared inventory");
    CHECK(unlink(fix_path("tests/host/c_hosttest.c")) == 0, "new source removed");
    put("build/tests/probe", "#!/bin/sh\nexit 0\n");
    inventory("tests", 0, NULL);
    CHECK(run(output, sizeof(output)) == 1 && strstr(output, "program is a script"),
          "a matching script digest cannot bypass native execution");
}

#include "compat/host_partition_host.c"
#include "compat/native_script_partition.c"

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
        partitions();
        script_driver_partitions();
        native_script_partition();
    }
    if (!failures)
        fix_passed();
    return failures ? 1 : 0;
}
