/**
 * Check `etude prepare producer`, tools/etude/routine_producer.c, through the
 * built etude on small committed trees, with this program as the producer's
 * installed etude. It fakes the collection preparation, the export and the
 * ledger's status, and passes every other command to the real etude.
 *
 * host-deps: utils/sha256.c
 * host-cflags: -I utils -Wall -Wextra -Werror
 * host-inputs: build/tools/etude/etude
 * host-tools: git
 *
 * A production stages the candidate bundle's commit, prepares once into a
 * fresh output of its run, with the clean declaration of its purpose and no
 * inherited phase selection, under the tree's run lock, which it releases;
 * the same run again is refused, and a second run has an output of its own.
 * A purpose whose clean declaration differs, a changed commit argument, a
 * clean value that is not a boolean and a transport admission for another
 * mode are refused before any output. Transport cleanup passes only when the
 * ledger has no claim or request of the producer's run. The actor passes only
 * while its digest matches, and the site projection is made once. On macOS a
 * jobs snapshot without a Linux boot proof is refused.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "compat/fixdir_host.c"
#include "sha256.h"

static int failures, cases;
static char self[4096], etude[4096];

#define CHECK(c, row)                                                       \
    do {                                                                    \
        if (!(c)) {                                                         \
            fprintf(stderr, "producer: %d: %s: %s\n", __LINE__, (row), #c); \
            failures++;                                                     \
        }                                                                   \
    } while (0)

/** One fixture: a committed repository, a workspace and the producer. */
struct state {
    char base[448], work[512], commit[41], producer[16384];
};

/** One operation and the status `etude prepare producer` must end with. */
struct op {
    const char *action, *run, *purpose, *clean, *mode, *argument, *status;
    int expected;
};

static void put(const char *file, const char *text)
{
    FILE *f = fopen(file, "wb");
    CHECK(f && fputs(text, f) >= 0 && !fclose(f), file);
}

/** Read FILE whole into BUF; return 0, or -1 when it is absent or too long. */
static int get(const char *file, char *buf, size_t size)
{
    FILE *f = fopen(file, "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, size - 1, f);
    int full = n == size - 1;
    fclose(f);
    buf[n] = 0;
    return full ? -1 : 0;
}

static int exists(const char *file)
{
    struct stat st;
    return !lstat(file, &st);
}

/** Run git with WORDS in TREE; with OUT, keep its output's first line. */
static int git(const char *tree, const char *const words[], char *out, size_t size)
{
    char *argv[16] = {"git", "-C", (char *)tree};
    size_t n = 3;
    int pipes[2];
    for (; *words && n < 15; words++)
        argv[n++] = (char *)*words;
    argv[n] = NULL;
    if (pipe(pipes))
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipes[1], 1);
        close(pipes[0]);
        close(pipes[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipes[1]);
    char line[256];
    ssize_t got, total = 0;
    while ((got = read(pipes[0], line + total, sizeof line - 1 - (size_t)total)) > 0)
        total += got;
    close(pipes[0]);
    line[total] = 0;
    int status;
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status))
        return -1;
    if (out) {
        line[strcspn(line, "\n")] = 0;
        snprintf(out, size, "%s", line);
    }
    return 0;
}

static void bundle(struct state *s, const char *work)
{
    char repository[4096], file[4096];
    snprintf(repository, sizeof repository, "%s/repository", s->base);
    snprintf(file, sizeof file, "%s/candidate.bundle", work);
    const char *create[] = {"bundle", "create", "-q", file, "HEAD", NULL};
    CHECK(!git(repository, create, NULL, 0), "the candidate bundle");
    snprintf(file, sizeof file, "%s/nonce", work);
    put(file, "\n");
}

/**
 * Write config.json in WORK: the route with this producer, its actor
 * described when ACTOR is 1, with a wrong digest when it is 2, and EXTRA
 * members of the route after it.
 */
static void configure(struct state *s, const char *work, int actor, const char *extra)
{
    char described[4608] = "", file[4096], config[20480];
    if (actor) {
        struct sha256 h;
        unsigned char digest[32], block[65536];
        char hex[65];
        size_t n, size = 0;
        FILE *f = fopen(self, "rb");
        sha256_init(&h);
        while (f && (n = fread(block, 1, sizeof block, f)) > 0) {
            sha256_update(&h, block, n);
            size += n;
        }
        CHECK(f && !fclose(f), "the actor's digest");
        sha256_final(&h, digest);
        sha256_hex(digest, hex);
        if (actor == 2)
            memset(hex, '0', 64);
        snprintf(described, sizeof described,
                 ",\"actor\":{\"path\":\"%s\",\"size_bytes\":%lu,\"sha256\":\"%s\"}", self,
                 (unsigned long)size, hex);
    }
    snprintf(s->producer, sizeof s->producer,
             "{\"site\":\"%s/site\",\"etude\":\"%s\",\"repository\":\"%s/repository\","
             "\"tree_root\":\"%s/trees\",\"output_root\":\"%s/outputs\","
             "\"account\":\"fixture\",\"accelerator\":\"fixture\","
             "\"preparation_resources\":\"1,1,1\","
             "\"policy\":{\"ledger\":\"%s/ledger\",\"capacity\":\"1,1,1\"}%s}",
             s->base, self, s->base, s->base, s->base, s->base, described);
    snprintf(config, sizeof config,
             "{\"format\":\"etude-routine-route\",\"schema_version\":1,"
             "\"mode\":\"macos-import\",\"producer\":%s%s}",
             s->producer, extra ? extra : "");
    snprintf(file, sizeof file, "%s/config.json", work);
    put(file, config);
}

/** Make a fresh fixture in a directory of its own. */
static void fixture(struct state *s)
{
    char path[4096];
    const char *init[] = {"init", "-q", NULL}, *add[] = {"add", ".", NULL};
    const char *save[] = {
        "-c",  "user.name=Etude fixture", "-c", "user.email=fixture@example.invalid", "commit",
        "-qm", "Producer fixture",        NULL};
    const char *head[] = {"rev-parse", "HEAD", NULL};
    memset(s, 0, sizeof *s);
    snprintf(s->base, sizeof s->base, "%s/case-%d", fix_dir(), ++cases);
    snprintf(s->work, sizeof s->work, "%s/workspace", s->base);
    snprintf(path, sizeof path, "%s/repository", s->base);
    CHECK(!mkdir(s->base, 0700) && !mkdir(s->work, 0700) && !mkdir(path, 0700), "the fixture");
    snprintf(path, sizeof path, "%s/repository/.gitignore", s->base);
    put(path, "build/\n");
    snprintf(path, sizeof path, "%s/repository/source", s->base);
    put(path, "small source fixture\n");
    snprintf(path, sizeof path, "%s/repository", s->base);
    CHECK(!git(path, init, NULL, 0) && !git(path, add, NULL, 0) && !git(path, save, NULL, 0) &&
              !git(path, head, s->commit, sizeof s->commit) && strlen(s->commit) == 40,
          "the committed repository");
    bundle(s, s->work);
    snprintf(path, sizeof path, "%s/site", s->base);
    put(path, "etude-site 2\nendpoint linux\nledger /fixture\ncapacity 1,1,1\n");
    snprintf(path, sizeof path, "%s/ledger", s->base);
    CHECK(!mkdir(path, 0700), "the ledger");
    snprintf(path, sizeof path, "%s/ledger/claims", s->base);
    put(path, "etude-ledger 1\n");
    configure(s, s->work, 0, NULL);
}

/** Run `etude prepare producer` for O in WORK and check its status. */
static void operate(struct state *s, const char *work, const struct op *o)
{
    const char *action = o->action ? o->action : "produce", *run = o->run ? o->run : "sample-01";
    const char *purpose = o->purpose ? o->purpose : "phase_first_run";
    const char *argument = o->argument ? o->argument : s->commit;
    char operation[24576], file[4096], log[4096], site[4096];
    snprintf(operation, sizeof operation,
             "{\"action\":\"%s\",\"producer\":%s,\"mode\":\"%s\",\"run\":\"%s\","
             "\"commit\":\"%s\",\"purpose\":\"%s\",\"clean\":%s,"
             "\"args\":[\"%s\",\"--config\",\"${WORKSPACE}/config.json\",\"--commit\",\"%s\","
             "\"--run\",\"%s\",\"--purpose\",\"%s\",\"--arches\",\"both\",\"--\",\"all\"],"
             "\"transport_admitted\":true,\"transport_result\":\"%s/transport.result\"}",
             action, s->producer, o->mode ? o->mode : "macos-import", run, s->commit, purpose,
             o->clean ? o->clean : "true", action, argument, run, purpose, s->base);
    snprintf(file, sizeof file, "%s/operation.json", work);
    put(file, operation);
    snprintf(log, sizeof log, "%s/producer.log", s->base);
    snprintf(site, sizeof site, "%s/site", s->base);
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        char *argv[] = {etude, "prepare", "producer", file, NULL};
        if (fd < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0 || chdir(work) ||
            setenv("ETUDE_ADMITTED", site, 1) ||
            setenv("ETUDE_PHASE_SELECTION_EXPORT", "/unrelated", 1) ||
            setenv("ETUDE_PREPARATION_CLEAN", "stale", 1) ||
            (o->status ? setenv("ETUDE_PRODUCER_TEST_STATUS", o->status, 1)
                       : unsetenv("ETUDE_PRODUCER_TEST_STATUS")))
            _exit(126);
        execv(argv[0], argv);
        _exit(127);
    }
    int status, ended = pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status);
    int value = ended ? WEXITSTATUS(status) : -1;
    CHECK(value == o->expected, action);
    if (value != o->expected) {
        static char text[65536];
        if (!get(log, text, sizeof text))
            fprintf(stderr, "producer: %s %s ended %d, not %d:\n%s", action, run, value,
                    o->expected, text);
    }
}

/** Check the record the fake collection preparation left for RUN. */
static void prepared(struct state *s, const char *run, const char *clean)
{
    char file[4096], text[4096], expected[4096];
    snprintf(file, sizeof file, "%s/outputs/%s/actual", s->base, run);
    snprintf(expected, sizeof expected,
             "clean %s\nrun %s:producer\ncommit %s\nselected -\nfirst prepare collection\n"
             "last all\n",
             clean, run, s->commit);
    CHECK(!get(file, text, sizeof text) && !strcmp(text, expected), run);
}

static int locked(struct state *s)
{
    char file[4096];
    snprintf(file, sizeof file, "%s/repository/build/.run.lock", s->base);
    return exists(file);
}

static int outputs(struct state *s)
{
    char file[4096];
    snprintf(file, sizeof file, "%s/outputs", s->base);
    return exists(file);
}

static void fresh_producer(void)
{
    struct state s;
    struct op produce = {0}, again = {0};
    char file[4096];
    fixture(&s);
    operate(&s, s.work, &produce);
    prepared(&s, "sample-01", "1");
    snprintf(file, sizeof file, "%s/produced.json", s.work);
    CHECK(exists(file), "a production writes produced.json");
    again.expected = 1;
    operate(&s, s.work, &again);
    CHECK(!locked(&s), "the run lock is released");
}

static void distinct_outputs(void)
{
    struct state s;
    struct op first = {0}, second = {0}, repeated = {0};
    char later[512];
    fixture(&s);
    operate(&s, s.work, &first);
    snprintf(later, sizeof later, "%s/workspace-02", s.base);
    CHECK(!mkdir(later, 0700), "the second workspace");
    bundle(&s, later);
    configure(&s, later, 0, NULL);
    second.run = "sample-02";
    operate(&s, later, &second);
    prepared(&s, "sample-01", "1");
    prepared(&s, "sample-02", "1");
    repeated.run = "sample-02";
    repeated.expected = 1;
    operate(&s, later, &repeated);
    CHECK(!locked(&s), "the run lock is released");
}

static void clean_purposes(void)
{
    const char *purposes[] = {"phase_first_run", "phase_first_run", "release", "ci"};
    const char *cleans[] = {"true", "false", "true", "false"};
    for (size_t i = 0; i < 4; i++) {
        struct state s;
        struct op o = {0};
        fixture(&s);
        o.purpose = purposes[i];
        o.clean = cleans[i];
        operate(&s, s.work, &o);
        prepared(&s, "sample-01", cleans[i][0] == 't' ? "1" : "-");
    }
}

static void refused_conditions(void)
{
    const char *purposes[] = {"ci", "release", "unknown"};
    const char *cleans[] = {"true", "false", "false"};
    struct state s;
    fixture(&s);
    for (size_t i = 0; i < 3; i++) {
        struct op o = {0};
        o.purpose = purposes[i];
        o.clean = cleans[i];
        o.expected = 1;
        operate(&s, s.work, &o);
    }
    struct op argument = {0}, clean = {0};
    argument.argument = "0000000000000000000000000000000000000000";
    argument.expected = 1;
    operate(&s, s.work, &argument);
    clean.clean = "\"true\"";
    clean.expected = 1;
    operate(&s, s.work, &clean);
    CHECK(!outputs(&s), "a refused operation makes no output");
}

static void transport_mode(void)
{
    struct state s;
    struct op o = {0};
    fixture(&s);
    o.mode = "windows-import";
    o.expected = 1;
    operate(&s, s.work, &o);
    CHECK(!outputs(&s), "a transport admission of another mode makes no output");
}

#ifdef __APPLE__
static void boot_required(void)
{
    struct state s;
    struct op o = {0};
    char file[4096];
    fixture(&s);
    configure(&s, s.work, 0,
              ",\"accepted_jobs\":{\"repository\":\"fixture/project\",\"listeners\":{}}");
    o.action = "facts";
    o.expected = 1;
    operate(&s, s.work, &o);
    snprintf(file, sizeof file, "%s/jobs-before.json", s.work);
    CHECK(!exists(file), "no job snapshot without a boot proof");
    DIR *d = opendir(s.work);
    struct dirent *e;
    while (d && (e = readdir(d)))
        CHECK(strncmp(e->d_name, "jobs-before.api.", 16), e->d_name);
    if (d)
        closedir(d);
}
#endif

static void transport_cleanup(void)
{
    const char *statuses[] = {"claim sample-01:producer:prepare",
                              "request sample-01:producer:prepare:child", "etude status: empty"};
    for (size_t i = 0; i < 3; i++) {
        struct state s;
        struct op o = {0};
        char file[4096], text[512], expected[512];
        fixture(&s);
        snprintf(file, sizeof file, "%s/transport.result", s.base);
        put(file, "exited 0\n");
        o.action = "transport-cleanup";
        o.status = statuses[i];
        o.expected = i < 2;
        operate(&s, s.work, &o);
        snprintf(file, sizeof file, "%s/producer.admit.result", s.work);
        CHECK(exists(file) == (i == 2), statuses[i]);
        snprintf(file, sizeof file, "%s/transport.status", s.work);
        snprintf(expected, sizeof expected, "%s\n", statuses[i]);
        CHECK(!get(file, text, sizeof text) && !strcmp(text, expected), statuses[i]);
    }
}

static void installed_actor(void)
{
    for (int changed = 0; changed < 2; changed++) {
        struct state s;
        struct op o = {0};
        fixture(&s);
        configure(&s, s.work, 1 + changed, NULL);
        o.expected = changed;
        operate(&s, s.work, &o);
    }
}

static void projection(void)
{
    struct state s;
    struct op o = {0}, again = {0};
    char file[4096], text[4096];
    fixture(&s);
    snprintf(file, sizeof file, "%s/site", s.base);
    put(file, "etude-site 2\nendpoint blocked\nledger /fixture\ncapacity 1,1,1\n");
    o.action = "projection";
    operate(&s, s.work, &o);
    snprintf(file, sizeof file, "%s/producer.site", s.work);
    CHECK(!get(file, text, sizeof text) &&
              !strcmp(text, "etude-site 2\nendpoint linux\nledger /fixture\ncapacity 1,1,1\n"),
          "the projection names the Linux endpoint");
    again.action = "projection";
    again.expected = 1;
    operate(&s, s.work, &again);
}

static const char *option(int argc, char **argv, const char *name)
{
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], name))
            return argv[i + 1];
    return NULL;
}

/**
 * The producer's installed etude: fake `prepare collection`, `prepare
 * export` and `status`, and run the real etude for every other command.
 */
static int actor(int argc, char **argv)
{
    const char *real = getenv("ETUDE_PRODUCER_TEST_ETUDE");
    if (argc >= 3 && !strcmp(argv[1], "prepare") && !strcmp(argv[2], "collection")) {
        const char *record = option(argc, argv, "-p");
        char out[1024], file[1100];
        if (!record || strlen(record) >= sizeof out)
            return 2;
        strcpy(out, record);
        *strrchr(out, '/') = 0;
        snprintf(file, sizeof file, "%s/prepare.record", out);
        put(file, "fixture record\n");
        snprintf(file, sizeof file, "%s/preparation.receipt.json", out);
        put(file, "{}\n");
        const char *clean = getenv("ETUDE_PREPARATION_CLEAN");
        const char *run = getenv("ETUDE_PREPARATION_RUN_ID");
        const char *commit = getenv("ETUDE_PREPARATION_COMMIT");
        const char *selected = getenv("ETUDE_PHASE_SELECTION_EXPORT");
        snprintf(file, sizeof file, "%s/actual", out);
        FILE *f = fopen(file, "wb");
        if (!f)
            return 2;
        fprintf(f, "clean %s\nrun %s\ncommit %s\nselected %s\nfirst %s %s\nlast %s\n",
                clean ? clean : "-", run ? run : "-", commit ? commit : "-",
                selected ? selected : "-", argv[1], argv[2], argv[argc - 1]);
        return fclose(f) ? 2 : failures;
    }
    if (argc >= 3 && !strcmp(argv[1], "prepare") && !strcmp(argv[2], "export")) {
        const char *output = option(argc, argv, "--output");
        if (!output)
            return 2;
        put(output, "{}\n");
        return failures;
    }
    if (!strcmp(argv[1], "status")) {
        const char *status = getenv("ETUDE_PRODUCER_TEST_STATUS");
        puts(status ? status : "etude status: empty");
        return 0;
    }
    argv[0] = (char *)real;
    execv(real, argv);
    perror(real);
    return 127;
}

int main(int argc, char **argv)
{
    if (argc > 1 && getenv("ETUDE_PRODUCER_TEST_ETUDE"))
        return actor(argc, argv);
    if (!realpath(argv[0], self) || !realpath("build/tools/etude/etude", etude)) {
        fprintf(stderr, "producer: this program or build/tools/etude/etude cannot be found\n");
        return 1;
    }
    if (setenv("ETUDE_PRODUCER_TEST_ETUDE", etude, 1))
        return 1;
    fresh_producer();
    distinct_outputs();
    clean_purposes();
    refused_conditions();
    transport_mode();
#ifdef __APPLE__
    boot_required();
#endif
    transport_cleanup();
    installed_actor();
    projection();
    if (failures) {
        fprintf(stderr, "producer fixture evidence: %s\n", fix_dir());
        return 1;
    }
    fix_passed();
    puts("etude_producer_hosttest: pass");
    return 0;
}
