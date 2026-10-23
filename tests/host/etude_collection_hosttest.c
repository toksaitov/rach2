/* Check native collection selection, prepared execution and learned bounds.
 * host-deps: utils/sha256.c tools/etude/posix/process.c
 * host-cflags: -I utils -I tools/etude/include -I tools/etude/posix -Wall
 * host-cflags: -Wextra -Werror
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
#include "etude/exec.h"

static int failures;
static pid_t collection_child;
static char etude[PATH_MAX], self[PATH_MAX], tree[PATH_MAX];
#define CHECK(ok, why)                                      \
    do {                                                    \
        if (!(ok)) {                                        \
            fprintf(stderr, "etude-collection: %s\n", why); \
            failures++;                                     \
        }                                                   \
    } while (0)

/* Only this fixture uses this private lock directory. End a detached holder
 * after its collection has ended, retaining the failed ledger as evidence. */
static void collection_cleanup(void)
{
    if (collection_child > 0) {
        kill(collection_child, SIGTERM);
        while (waitpid(collection_child, NULL, 0) < 0 && errno == EINTR) {
        }
        collection_child = 0;
    }
    int fd = open(fix_path("rach2-host.lock"), O_RDWR);
    if (fd < 0)
        return;
    struct flock lock;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_GETLK, &lock) == 0 && lock.l_type != F_UNLCK) {
        struct etude_pid first, current;
        if (etude_process_state(lock.l_pid, &first) == 1 && first.start_known &&
            fcntl(fd, F_GETLK, &lock) == 0 && lock.l_type != F_UNLCK &&
            etude_process_state(lock.l_pid, &current) == 1 && current.start_known &&
            first.pid == current.pid && first.start == current.start) {
            fprintf(stderr, "fixture cleanup: detached holder %ld creation %llu\n", first.pid,
                    (unsigned long long)first.start);
            int ended = kill(first.pid, SIGKILL);
            CHECK(ended == 0, "private detached holder ends");
            if (!ended) {
                lock.l_type = F_WRLCK;
                while (fcntl(fd, F_SETLKW, &lock) < 0 && errno == EINTR) {
                }
                CHECK(etude_process_state(first.pid, &current) == 0 || current.start != first.start,
                      "private detached holder cleanup confirmed");
            }
        }
    }
    close(fd);
}

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

static void case_environment(const char *cases, const char *rows, const char *failure)
{
    unsetenv("RACH2_CASES");
    unsetenv("RACH2_AGENTS_ROWS");
    unsetenv("RACH2_ACCEPT_AGENTS");
    unsetenv("RACH2_PORT_OFFSET");
    unsetenv("SSH_TEST_PORT");
    unsetenv("ETUDE_SITE");
    unsetenv("ETUDE_ADMITTED");
    unsetenv("RACH2_TREE_HELD");
    setenv("RACH2_PORT_DIR", tree, 1);
    unsetenv("RACH2_WORKERS");
    unsetenv("RACH2_MEMORY_MIB");
    unsetenv("RACH2_CPUS");
    if (cases)
        setenv("RACH2_CASES", cases, 1);
    if (rows)
        setenv("RACH2_AGENTS_ROWS", rows, 1);
    if (failure && !strcmp(failure, "wait")) {
        setenv("ETUDE_COLLECTION_READY", "yes", 1);
        unsetenv("ETUDE_COLLECTION_FAIL");
    } else {
        unsetenv("ETUDE_COLLECTION_READY");
        if (failure)
            setenv("ETUDE_COLLECTION_FAIL", failure, 1);
        else
            unsetenv("ETUDE_COLLECTION_FAIL");
    }
}

/* The fixture reports readiness on the collection's owned output pipe. EOF
 * also ends this wait when preparation or load startup fails. */
static void interrupt_output(int fd, pid_t child)
{
    FILE *input = fdopen(fd, "r"), *output = fopen(fix_path("output"), "wb");
    char line[4096];
    int ready = 0;
    CHECK(input && output, "interruption output opens");
    if (!input || !output)
        kill(child, SIGTERM);
    if (!input)
        close(fd);
    if (input && output)
        while (fgets(line, sizeof(line), input)) {
            fputs(line, output);
            if (!ready && strstr(line, "fixture: interrupt-ready")) {
                ready = 1;
                CHECK(kill(child, SIGTERM) == 0, "the loaded collection receives interruption");
            }
        }
    CHECK(ready, "the loaded compiled case is running before interruption");
    if (input)
        fclose(input);
    if (output)
        fclose(output);
}

static int run(char *const *options, const char *cases, const char *rows, const char *failure,
               char *out, size_t room)
{
    char *argv[40];
    size_t n = 0, got;
    int status;
    pid_t child;
    int stream[2] = {-1, -1};
    int interrupt = failure && !strcmp(failure, "wait");
    if (interrupt && pipe(stream))
        return -1;
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
        int fd =
            interrupt ? stream[1] : open(fix_path("output"), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (stream[0] >= 0)
            close(stream[0]);
        if (fd < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0)
            _exit(120);
        close(fd);
        case_environment(cases, rows, failure);
        execv(etude, argv);
        _exit(127);
    }
    if (child < 0) {
        close(stream[0]);
        close(stream[1]);
        return -1;
    }
    collection_child = child;
    if (interrupt) {
        close(stream[1]);
        interrupt_output(stream[0], child);
    }
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    collection_child = 0;
    collection_cleanup();
    FILE *ledger = fopen(fix_path("build/tests/results/ledger/claims"), "rb");
    if (ledger) {
        char claims[128];
        size_t count = fread(claims, 1, sizeof(claims), ledger);
        CHECK(count == strlen("etude-ledger 1\n") && !memcmp(claims, "etude-ledger 1\n", count),
              "every case and load claim is released");
        fclose(ledger);
    }
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

static void repetitions(char *out, size_t room)
{
    char *first[] = {"-N",   "-J", (char *)fix_path("first-selection.json"), "-S", "42", "-R", "6",
                     "host", NULL};
    char *second[] = {
        "-N", "-J", (char *)fix_path("second-selection.json"), "-S", "42", "-R", "6", "host", NULL};
    CHECK(!run(first, NULL, NULL, NULL, out, room) && !run(second, NULL, NULL, NULL, out, room),
          "seeded public allocator exports without artifacts or execution");
    char a[16384], b[16384];
    FILE *f = fopen(fix_path("first-selection.json"), "rb");
    size_t na = f ? fread(a, 1, sizeof(a) - 1, f) : 0;
    if (f)
        fclose(f);
    f = fopen(fix_path("second-selection.json"), "rb");
    size_t nb = f ? fread(b, 1, sizeof(b) - 1, f) : 0;
    if (f)
        fclose(f);
    a[na] = b[nb] = 0;
    CHECK(na && na == nb && !strcmp(a, b) && occurrences(a, "\"repetition\":") >= 6,
          "the same seed retains exact item order and every repetition");
    char *bad[] = {"-N", "-R", "3", "host", NULL};
    CHECK(run(bad, NULL, NULL, NULL, out, room) == 2, "repetitions require an explicit seed");
    char *shuffled[] = {"-A", "tcg", "-S", "42", "-R", "6", "host", NULL};
    CHECK(!run(shuffled, NULL, NULL, NULL, out, room), "the public shuffled collection passes");
    const char *cursor = out;
    const char *order[] = {"vectors r4: starting", "vectors r3: starting", "vectors: starting",
                           "vectors r2: starting", "vectors r5: starting", "vectors r1: starting"};
    for (size_t i = 0; i < 6; i++) {
        const char *next = cursor ? strstr(cursor, order[i]) : NULL;
        CHECK(next != NULL, "compiled cases execute the exported seed42 order");
        cursor = next ? next + strlen(order[i]) : NULL;
    }
    char *interrupted[] = {"-A", "tcg", "-S", "42", "-R", "2", "-L", "cpu:1", "host", NULL};
    CHECK(run(interrupted, NULL, NULL, "wait", out, room) == 143,
          "interruption retains cancelled repetitions and confirms case and load cleanup");
    const char *loads[] = {"cpu:1", "memory:65536", "disk:65536"};
    for (size_t i = 0; i < 3; i++) {
        char *loaded[] = {"-A", "tcg", "-S", "42", "-R", "2", "-L", (char *)loads[i], "host", NULL};
        int status = run(loaded, NULL, NULL, NULL, out, room);
        if (status || occurrences(out, ": starting\n") != 2)
            fprintf(stderr, "%s", out);
        CHECK(!status && occurrences(out, ": starting\n") == 2,
              "every repeated compiled case runs beside its declared ready native load");
        CHECK(strstr(out, "case 30000000 us (0 samples)"),
              "unloaded successful history cannot teach the loaded profile");
        CHECK(run(loaded, NULL, NULL, "yes", out, room) == 1,
              "failed repetitions stay failed instead of becoming a passing retry");
    }
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
        const char *ready = getenv("ETUDE_COLLECTION_READY");
        if (ready) {
            puts("fixture: interrupt-ready");
            fflush(stdout);
            for (;;)
                pause();
        }
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
    atexit(collection_cleanup);
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
    repetitions(out, sizeof(out));
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
