/* Check native collection selection, prepared execution and learned bounds.
 * host-deps: tools/etude/routine_charge.c
 * host-deps: utils/sha256.c tools/etude/posix/process.c tools/etude/json.c
 * host-deps: tools/etude/compare.c tools/etude/baseline.c
 * host-deps: tools/etude/compare_load.c tools/etude/observe_import.c
 * host-deps: tools/etude/history.c tools/etude/history_json.c
 * host-deps: tools/etude/history_collect.c
 * host-deps: tests/cases/host/host_script_inventory.c
 * host-inputs: tests/host/compat/collection_scheduling.c
 * host-inputs: tests/host/compat/collection_cost.c
 * host-inputs: tests/host/compat/load_history_host.h
 * host-inputs: tests/cases/host.host.json tests/cases/host/host.list
 * host-inputs: tests/host/compat/host_scripts_order.c
 * host-cflags: -I utils -I tools/etude -I tools/etude/include
 * host-cflags: -I tools/etude/posix -Wall -Wextra -Werror
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
 * and a changed artifact are refused before execution. A case whose hosts
 * leave out this host is left out of the selection, and a manifest whose
 * hosts are empty, unknown or repeated is refused. A run of one
 * architecture leaves out a case of the other unless an identity names it.
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
/* What the next collection inherits: a port block alone, or one inside a
 * claim of the site, and a temporary directory of the fixture's own. */
static enum { NO_CLAIM, PORTS_ALONE, PORTS_CLAIMED } claim;
static const char *temporary;
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
    CHECK(setenv("ETUDE_SITE", fix_path("site"), 1) == 0,
          "nested collection uses its private authority");
    unsetenv("ETUDE_ADMITTED");
    if (claim != NO_CLAIM)
        setenv("RACH2_PORT_OFFSET", "0", 1);
    if (claim == PORTS_CLAIMED)
        setenv("ETUDE_ADMITTED", fix_path("site"), 1);
    if (temporary)
        setenv("TMPDIR", temporary, 1);
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

/* Keep every complete stream, including early refusals before a plan exists. */
static void save_run(char *const *argv, int status)
{
    static unsigned sequence;
    char name[64], path[PATH_MAX];
    int ok;
    FILE *index;

    fix_format(name, sizeof(name), "output.%u", ++sequence);
    fix_format(path, sizeof(path), "%s/%s", fix_dir(), name);
    copy(fix_path("output"), path);
    fix_format(path, sizeof(path), "%s/runs.log", fix_dir());
    index = fopen(path, "ab");
    CHECK(index != NULL, "the complete collection stream index opens");
    if (!index)
        return;
    ok = fprintf(index, "%s wait-status=%d exit=%d\n", name, status,
                 WIFEXITED(status) ? WEXITSTATUS(status) : -1) >= 0;
    for (size_t i = 0; argv[i]; i++)
        ok &= fprintf(index, "argv[%lu]=%s\n", (unsigned long)i, argv[i]) >= 0;
    if (fclose(index))
        ok = 0;
    CHECK(ok, "the complete collection stream index is retained");
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
    save_run(argv, status);
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

/* Never fall back to an installed authority when exercising private cases. */
static void fixture_policy(unsigned cpu)
{
    char text[PATH_MAX * 4 + 256];
#ifdef __APPLE__
    const char *endpoint = "macos";
#else
    const char *endpoint = "linux";
#endif
    fix_format(text, sizeof(text),
               "etude-site 2\nendpoint %s\nlauncher %s\nledger %s\nlocks %s\netude %s\n"
               "capacity %u,536870912,1073741824\nwait 60\nseconds 60\n",
               endpoint, etude, fix_path("build/tests/results/ledger"), tree, etude, cpu);
    put("site", text);
}

static void fixture(void)
{
    static const char *const dirs[] = {"tests",
                                       "tests/cases",
                                       "tools",
                                       "tools/etude",
                                       "tools/etude/schemas",
                                       "build",
                                       "build/tests",
                                       "build/tests/host",
                                       "build/tests/results",
                                       "build/tests/results/ledger"};
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
    put("build/tests/results/ledger/claims", "etude-ledger 1\n");
    fixture_policy(4000);
}

#include "compat/collection_scheduling.c"
#include "compat/collection_cost.c"
#include "compat/host_scripts_order.c"
#include "compat/host_scripts_dispatch.c"

#ifdef __APPLE__
#define HOSTS_HERE      "macos"
#define HOSTS_ELSEWHERE "[\"linux\",\"windows\"]"
#else
#define HOSTS_HERE      "linux"
#define HOSTS_ELSEWHERE "[\"macos\",\"windows\"]"
#endif

/* Write the suite `hosts` of three copies of the host template's case, each
 * named for its `requirements.hosts`: `here`, `elsewhere` and `anywhere`,
 * which has none, or of one case of the given `hosts` alone. */
static void hosts_manifest(const char *hosts)
{
    static const char *const ids[] = {"here", "elsewhere", "anywhere"};
    const char *lists[] = {"[\"windows\",\"" HOSTS_HERE "\"]", HOSTS_ELSEWHERE, NULL};
    char text[16384], error[256], id[64];
    FILE *f = fopen("tools/etude/fixtures/verdict/cases/fixture.host.json", "rb");
    struct json *doc = NULL;
    size_t n = f ? fread(text, 1, sizeof(text), f) : 0;
    if (f)
        fclose(f);
    CHECK(n && !json_parse(text, n, 1, &doc, error, sizeof(error)), "hosts case template parses");
    const struct json *cases = json_member(doc, "cases");
    struct json *one = cases && cases->count == 1 ? cases->item[0] : NULL;
    struct jbuf output = {0};
    jbuf_puts(&output, "{\"schema_version\":1,\"summary\":\"Cases of some host systems\","
                       "\"family_id\":\"fixture.hosts\",\"cases\":[");
    for (size_t i = 0; one && i < (hosts ? 1 : 3); i++) {
        struct jbuf c = {0};
        const char *list = hosts ? hosts : lists[i];
        snprintf(id, sizeof(id), "fixture.hosts.%s", ids[i]);
        scheduling_string(one, "case_id", id);
        json_canonical(one, &c);
        char *requirements = c.data ? strstr(c.data, "\"requirements\":{") : NULL;
        CHECK(requirements != NULL, "hosts case template has requirements");
        if (requirements) {
            requirements += strlen("\"requirements\":{");
            jbuf_puts(&output, i ? "," : "");
            jbuf_put(&output, c.data, (size_t)(requirements - c.data));
            if (list) {
                jbuf_puts(&output, "\"hosts\":");
                jbuf_puts(&output, list);
                jbuf_puts(&output, ",");
            }
            jbuf_puts(&output, requirements);
        }
        jbuf_free(&c);
    }
    jbuf_puts(&output, "]}\n");
    put("tests/cases/fixture.hosts.json", output.data);
    jbuf_free(&output);
    json_free(doc);
}

/* A case whose hosts leave out this host is neither selected nor refused,
 * and is said once to be left out; one that lists this host and one with no
 * hosts are selected. A wrong `hosts` refuses the manifest. */
static void host_selection(char *out, size_t room)
{
    char *options[] = {"-N", "hosts", NULL};
    static const char *const wrong[][2] = {{"[]", "fewer than 1"},
                                           {"[\"solaris\"]", "is not one of the values"},
                                           {"[\"" HOSTS_HERE "\",\"" HOSTS_HERE "\"]", "equal"}};
    put("tests/registry", "default|-|core\nhost|-|core\nagents|-|full|||x86_64\nhosts|-|full\n");
    hosts_manifest(NULL);
    CHECK(run(options, NULL, NULL, NULL, out, room) == 0 &&
              strstr(out, "hosts none fixture.hosts.here\n") &&
              strstr(out, "hosts none fixture.hosts.anywhere\n") &&
              !strstr(out, "hosts none fixture.hosts.elsewhere\n"),
          "the selection keeps the cases of this host and of every host and leaves out another");
    CHECK(occurrences(out, "etude: case fixture.hosts.elsewhere: not run on " HOSTS_HERE "\n") == 1,
          "the selection says once that it leaves out the case of another host");
    CHECK(run(options, "fixture.hosts.elsewhere", NULL, NULL, out, room) == 2 &&
              strstr(out, "a chosen case does not run on this host"),
          "an identity of a case of another host is refused");
    for (size_t i = 0; i < sizeof(wrong) / sizeof(wrong[0]); i++) {
        hosts_manifest(wrong[i][0]);
        CHECK(run(options, NULL, NULL, NULL, out, room) == 2 &&
                  strstr(out, "/requirements/hosts") && strstr(out, wrong[i][1]),
              "an empty, unknown or repeated host refuses the manifest");
    }
    CHECK(unlink(fix_path("tests/cases/fixture.hosts.json")) == 0, "hosts fixture is removed");
    put("tests/registry", "default|-|core\nhost|-|core\nagents|-|full|||x86_64\n");
}

/* A suite run on one architecture leaves out its case of the other
 * architecture, and an identity of that case is refused. */
static void architecture_selection(char *out, size_t room)
{
    char *options[] = {"-N", "-a", "x86_64", "arches", NULL};
    static const char *const ids[] = {"fixture.arches.both", "fixture.arches.i686"};
    char text[16384], error[256];
    FILE *f = fopen("tools/etude/fixtures/verdict/cases/fixture.default.json", "rb");
    struct json *doc = NULL;
    size_t n = f ? fread(text, 1, sizeof(text), f) : 0;
    if (f)
        fclose(f);
    CHECK(n && !json_parse(text, n, 1, &doc, error, sizeof(error)),
          "architectures case template parses");
    const struct json *cases = json_member(doc, "cases");
    struct json *one = cases && cases->count == 1 ? cases->item[0] : NULL;
    struct json *arches = (struct json *)json_member(one, "architectures");
    CHECK(arches && arches->type == JSON_ARRAY && arches->count == 2,
          "architectures case template runs on both");
    struct jbuf output = {0};
    jbuf_puts(&output, "{\"schema_version\":1,\"summary\":\"Cases of one or both "
                       "architectures\",\"family_id\":\"fixture.arches\",\"cases\":[");
    for (size_t i = 0; arches && arches->count == 2 && i < 2; i++) {
        scheduling_string(one, "case_id", ids[i]);
        if (i) {
            json_free(arches->item[1]);
            arches->count = 1;
            jbuf_puts(&output, ",");
        }
        json_canonical(one, &output);
    }
    jbuf_puts(&output, "]}\n");
    put("tests/cases/fixture.arches.json", output.data);
    jbuf_free(&output);
    json_free(doc);
    put("tests/registry", "default|-|core\nhost|-|core\nagents|-|full|||x86_64\narches|-|full\n");
    CHECK(run(options, NULL, NULL, NULL, out, room) == 0 &&
              strstr(out, "arches x86_64 fixture.arches.both\n") &&
              !strstr(out, "fixture.arches.i686"),
          "a run of one architecture leaves out a case of the other");
    CHECK(run(options, "fixture.arches.i686", NULL, NULL, out, room) == 2 &&
              strstr(out, "a chosen case does not run on the selected architecture"),
          "an identity of a case of the other architecture is refused");
    CHECK(unlink(fix_path("tests/cases/fixture.arches.json")) == 0,
          "architectures fixture is removed");
    put("tests/registry", "default|-|core\nhost|-|core\nagents|-|full|||x86_64\n");
}

/**
 * Check what the run entry does beyond a selection: a port block inside and
 * outside a claim of the site, the inventory, and a preparation that fails.
 */
static void run_entry(char *out, size_t size)
{
    char *native[] = {"-A", "tcg", "-C", "2", "host", NULL};
    char *inventory[] = {"inventory", NULL};
    char *prepared[] = {"-B", "-A", "tcg", "host", NULL};
    claim = PORTS_ALONE;
    CHECK(run(native, NULL, NULL, NULL, out, size) == 2 &&
              strstr(out, "port overrides bypass native admission"),
          "a port block from outside a claim of the site is refused");
    claim = PORTS_CLAIMED;
    CHECK(run(native, NULL, NULL, NULL, out, size) == 0 &&
              strstr(out, "run: result: all 1 suites passed"),
          "a run inside a claim of the site takes that claim's port block");
    claim = NO_CLAIM;
    CHECK(run(inventory, NULL, NULL, NULL, out, size) == 0 &&
              strstr(out, "* `agents` (full tier; needs nothing beyond the build; no machine; "
                          "x86_64 only): (no sentence in suites.mk names this suite)\n"),
          "the inventory gives each suite's tier, tools, memory and architectures");
    CHECK(mkdir(fix_path("tmp"), 0700) == 0, "the preparation's temporary directory is made");
    temporary = fix_path("tmp");
    CHECK(mkdir(fix_path("bin"), 0700) == 0 && symlink(self, fix_path("bin/make")) == 0,
          "the stand-in make is made");
    static char saved[8192], first[8192 + PATH_MAX];
    const char *path = getenv("PATH");
    CHECK(snprintf(saved, sizeof(saved), "%s", path ? path : "/usr/bin:/bin") < (int)sizeof(saved),
          "the search path is saved");
    snprintf(first, sizeof(first), "%s:%s", fix_path("bin"), saved);
    setenv("PATH", first, 1);
    CHECK(run(prepared, NULL, NULL, NULL, out, size) == 1 &&
              strstr(out, "run: result: refused: preparation failed"),
          "a preparation that fails ends the run before any case");
    setenv("PATH", saved, 1);
    temporary = NULL;
    FILE *f = fopen(fix_path("hold.owner"), "rb");
    size_t got = f ? fread(out, 1, size - 1, f) : 0;
    if (f)
        fclose(f);
    out[got] = 0;
    CHECK(strstr(out, "\nargs run ") && !strstr(out, "\nheld script\n"),
          "the run holds the tree with a run's hold, which a later run takes over");
}

/**
 * Stand in for make in a preparation, the tree being the working directory:
 * copy the owner record of the tree's run lock to `hold.owner`, and fail.
 */
static int hold_record_make(void)
{
    char bytes[4096];
    size_t n = 0;
    FILE *in = fopen("build/.run.lock/owner", "rb"), *out = fopen("hold.owner", "wb");
    if (in)
        n = fread(bytes, 1, sizeof(bytes), in);
    if (out) {
        fwrite(bytes, 1, n, out);
        fclose(out);
    }
    if (in)
        fclose(in);
    return 2;
}

static int collection_main(int argc, char **argv)
{
    static char out[131072];
    char *both[] = {"-N", "-a", "both", "core", NULL};
    char *single[] = {"-N", "default", "host", NULL};
    char *duplicate[] = {"-N", "host", "host", NULL};
    char *unknown[] = {"-N", "absent", NULL};
    char *native[] = {"-A", "tcg", "-C", "2", "host", NULL};
    char *other[] = {"-A", "tcg", "-C", "3", "host", NULL};
    if (argc == 1 && getenv("ETUDE_CASE")) {
        int role = scheduling_role(getenv("ETUDE_CASE"));
        if (role >= 0)
            return role;
    }
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
    host_scripts_order();
    host_scripts_dispatch();
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
    host_selection(out, sizeof(out));
    architecture_selection(out, sizeof(out));
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
    run_entry(out, sizeof(out));
    repetitions(out, sizeof(out));
    put("build/tests/host/sha256_hosttest", "#!/bin/sh\nexit 0\n");
    make_record();
    CHECK(run(native, NULL, NULL, NULL, out, sizeof(out)) == 3 &&
              strstr(out, "requires a compiled program"),
          "portable execution rejects scripts even on POSIX");
    put("build/tests/host/sha256_hosttest", "changed\n");
    CHECK(run(native, NULL, NULL, NULL, out, sizeof(out)) == 3 && strstr(out, "changed"),
          "prepared artifacts are checked by digest before launch");
    copy(self, fix_path("build/tests/host/sha256_hosttest"));
    make_record();
    scheduling(out, sizeof(out));
    if (failures)
        return 1;
    fix_passed();
    puts("etude_collection_hosttest: pass");
    return 0;
}

int main(int argc, char **argv)
{
    const char *base = strrchr(argv[0], '/');
    if (!strcmp(base ? base + 1 : argv[0], "make"))
        return hold_record_make();
    if (argc == 2 && !strcmp(argv[1], "--scripts-only")) {
        host_scripts_order();
        host_scripts_dispatch();
        if (!failures)
            fix_passed();
        return failures ? 1 : 0;
    }
    return collection_main(argc, argv);
}
