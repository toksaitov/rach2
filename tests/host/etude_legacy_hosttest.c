/**
 * Check etude's preparation adapter (tools/etude/posix/legacy.c) on fixture
 * trees, each a Makefile whose recipes append their goals to the file
 * `calls`, with the tree's run lock held by this program, which is also the
 * recipe of a preparation graph's node with `--legacy`.
 *
 * host-deps: tools/etude/routine_charge.c
 * host-deps: tools/etude/routine_proof.c
 * host-deps: tools/etude/observe_import.c
 * host-deps: tools/etude/boot_identity.c
 * host-deps: tools/etude/measurement_context.c tools/etude/json.c
 * host-deps: tools/etude/history.c tools/etude/history_json.c
 * host-deps: tools/etude/history_collect.c
 * host-deps: tools/etude/preparation_record.c
 * host-deps: tools/etude/posix/legacy.c tools/etude/posix/store.c
 * host-deps: tools/etude/posix/prepare.c tools/etude/capture.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-deps: tools/etude/posix/process.c utils/sha256.c
 * host-deps: tests/host/compat/command_host.c
 * host-cflags: -I tools/etude/include -I tools/etude/posix
 * host-cflags: -Wall -Wextra -Werror
 * host-inputs: tests/host/preparation_import_test.py tests/native/prepare.py
 * host-inputs: tools/etude/host_prepare.c tools/etude/host_prepare.h
 * host-inputs: tools/etude/host_prepare_io.c tools/etude/host_prepare_build.c
 * host-inputs: tools/etude/host_prepare_tools.c
 * host-inputs: tools/etude/host_prepare_style.c
 * host-inputs: tools/etude/host_prepare_scripts.c mk/host-check.mk
 * host-inputs: mk/host-fixtures.mk build/tools/etude/etude
 * host-inputs: tests/host/fixtures/extract.py
 * host-inputs: tools/etude/preparation_import.c
 * host-inputs: tools/etude/preparation_import_data.c
 * host-inputs: tools/etude/preparation_import_transfer.c
 * host-inputs: tools/etude/preparation_import.h
 * host-inputs: tools/etude/collection_tools.c tools/etude/collection_tools.h
 * host-tools: python3
 * host-first: it runs for minutes
 *
 * Goals have the recovery key minted, the components and then the goals
 * built, and the goals' files and the kernel's recorded with their digests, a
 * directory among the goals accepted and not recorded; a second preparation
 * mints no key. The architecture and the kernel reach every make, over an
 * `ARCH` in the environment. A request of no goal, a goal outside the tree, an
 * architecture or a kernel name refused and a tree not held are each refused
 * with nothing built. A failed step stops the preparation and keeps its log; a
 * goal missing fails it; a step past its bound and one under a stop signal are
 * ended, and nothing of theirs runs after. The record is checked by status and
 * by digest, and a preparation graph publishes it as a fresh node's product.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "compat/fixdir_host.c"
#include "compat/command_host.h"
#include "etude/artifact.h"
#include "etude/legacy.h"
#include "sha256.h"
#include "../../tools/etude/json.h"
#include "../../tools/etude/measurement_context.h"

extern char **environ;

static int fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("etude-prepare-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), #cond); \
            fails++;                                                                               \
        }                                                                                          \
    } while (0)

/* The room for a path: a fixture's are under a $TMPDIR of at most 100
 * characters (fixdir_host.h). */
#define PATH_LEN 1024

/* This program, by an absolute path. */
static char self[PATH_LEN];

/* The fixture's Makefile. */
static const char makefile[] =
    "ARCH ?= i686\n"
    "ifeq ($(ARCH),x86_64)\n"
    "ARCH_BUILD := build/x86_64\n"
    "else\n"
    "ARCH_BUILD := build\n"
    "endif\n"
    "BOOT_KERNEL ?= $(ARCH_BUILD)/rach2/kernel\n"
    "IMAGES := $(ARCH_BUILD)/images\n"
    "note = echo \"$@ $(ARCH) $(BOOT_KERNEL) $$LC_ALL\" >> calls\n"
    ".PHONY: components recovery-key ubsan-kernel\n"
    "components: $(ARCH_BUILD)/rach2/kernel\n"
    "\t@$(note)\n"
    "\t@test ! -f fail-components\n"
    "\t@test -z "
    "\"$$ETUDE_PREPARATION_RUN_ID$$ETUDE_PREPARATION_RECEIPT$$ETUDE_PREPARATION_COMMIT$$ETUDE_"
    "PREPARATION_GRANT\"\n"
    "$(ARCH_BUILD)/rach2/kernel:\n"
    "\t@mkdir -p $(@D) && echo kernel > $@ && $(note)\n"
    "recovery-key:\n"
    "\t@echo key > krffs/recovery_authorized_keys && $(note)\n"
    "ubsan-kernel:\n"
    "\t@mkdir -p $(ARCH_BUILD)/rach2-ubsan && echo ubsan > $(ARCH_BUILD)/rach2-ubsan/kernel\n"
    "\t@$(note)\n"
    "$(IMAGES):\n"
    "\t@mkdir -p $@\n"
    "$(IMAGES)/%.img: src/in.c | $(IMAGES)\n"
    "\t@cp src/in.c $@ && $(note)\n"
    "$(IMAGES)/d.img:\n"
    "\t@$(note)\n"
    "$(IMAGES)/h.img:\n"
    "\t@$(note); read x < hang.fifo\n"
    "$(IMAGES)/w.img:\n"
    "\t@$(note); echo started > started.fifo; read x < hang.fifo\n"
    "krffs/shadow:\n"
    "\t@echo shadow > $@ && $(note)\n";

/* What one preparation gave. */
struct outcome {
    int status;
    char out[16384]; /* what it printed */
};

/* ---- Files. ---- */

/** Give the path of `name` in the fixture tree `tree` in `buf`. */
static const char *in(char *buf, size_t size, const char *tree, const char *name)
{
    fix_format(buf, size, "%s/%s", tree, name);
    return buf;
}

/** Make file `path` hold `text` with mode `mode`; 0 or -1. */
static int write_text(const char *path, const char *text, mode_t mode)
{
    FILE *f = fopen(path, "w");
    int r;

    if (f == NULL)
        return -1;
    r = fputs(text, f) < 0 ? -1 : 0;
    if (fclose(f) != 0)
        r = -1;
    return r == 0 ? chmod(path, mode) : -1;
}

/** Read up to `max - 1` bytes of `path` into `buf`, ended by NUL; the
 * length, or -1, with `buf` then empty. */
static long read_text(const char *path, char *buf, size_t max)
{
    FILE *f = fopen(path, "r");
    size_t n;

    buf[0] = '\0';
    if (f == NULL)
        return -1;
    n = fread(buf, 1, max - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}

/** Say whether `path` exists. */
static int exists(const char *path)
{
    struct stat st;

    return lstat(path, &st) == 0;
}

/** Say whether `text` has the whole line `line`. */
static int has_line(const char *text, const char *line)
{
    size_t n = strlen(line);
    const char *at;

    for (at = text; (at = strstr(at, line)) != NULL; at++) {
        if ((at == text || at[-1] == '\n') && (at[n] == '\n' || at[n] == '\0'))
            return 1;
    }
    return 0;
}

/** Give the offset of the line `line` in `text`, or -1 when it has none. */
static long line_at(const char *text, const char *line)
{
    size_t n = strlen(line);
    const char *at;

    for (at = text; (at = strstr(at, line)) != NULL; at++) {
        if ((at == text || at[-1] == '\n') && (at[n] == '\n' || at[n] == '\0'))
            return (long)(at - text);
    }
    return -1;
}

/** Give the SHA-256 of `text` in hex. */
static void sha_text(const char *text, char hex[65])
{
    unsigned char sum[32];
    struct sha256 h;

    sha256_init(&h);
    sha256_update(&h, (const unsigned char *)text, strlen(text));
    sha256_final(&h, sum);
    sha256_hex(sum, hex);
}

/** Write into `buf` the paths of a record's `file` lines, one a line. */
static void record_files(const char *record, char *buf, size_t max)
{
    const char *line, *path;
    size_t len = 0, n;
    int field;

    buf[0] = '\0';
    for (line = record; *line != '\0';
         line += strcspn(line, "\n") + (line[strcspn(line, "\n")] != 0)) {
        if (strncmp(line, "file ", 5) != 0)
            continue;
        for (path = line, field = 0; field < 5; field++)
            path += strcspn(path, " ") + 1;
        n = strcspn(path, "\n");
        if (len + n + 2 > max)
            return;
        memcpy(buf + len, path, n);
        len += n;
        buf[len++] = '\n';
        buf[len] = '\0';
    }
}

/**
 * Make the fixture tree `name` in the fixture directory, with its run lock
 * held by this process, and give its path in `tree`.
 */
static void make_tree(const char *name, char *tree, size_t size)
{
    static const char *const dirs[] = {"",      "krffs",           "krffs/manifests",
                                       "build", "build/.run.lock", "src"};
    char path[PATH_LEN], owner[64];
    size_t i;
    int bad = 0;

    fix_format(tree, size, "%s/%s", fix_dir(), name);
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        bad |= mkdir(in(path, sizeof(path), tree, dirs[i]), 0755) != 0;
    snprintf(owner, sizeof(owner), "pid %ld\nheld test\n", (long)getpid());
    bad |= write_text(in(path, sizeof(path), tree, "Makefile"), makefile, 0644);
    bad |= write_text(in(path, sizeof(path), tree, "krffs/manifests/rach2-recovery.manifest"), "",
                      0644);
    bad |= write_text(in(path, sizeof(path), tree, "krffs/rach2_demo_ed25519"), "key\n", 0644);
    bad |= write_text(in(path, sizeof(path), tree, "src/in.c"), "src\n", 0644);
    bad |= write_text(in(path, sizeof(path), tree, "build/.run.lock/owner"), owner, 0644);
    bad |= mkfifo(in(path, sizeof(path), tree, "hang.fifo"), 0644) != 0;
    bad |= mkfifo(in(path, sizeof(path), tree, "started.fifo"), 0644) != 0;
    if (bad) {
        printf("etude-prepare-test: fail: the fixture tree %s was not made\n", tree);
        exit(1);
    }
}

/* ---- Preparations. ---- */

/** Fill `o` for the goals `goals` of `tree` on i686, with the work
 * directory and the record named after `row` in the fixture directory. */
static void opts_for(struct etude_legacy_opts *o, const char *tree, const char *row,
                     char *const *goals)
{
    static char work[PATH_LEN], record[PATH_LEN];

    memset(o, 0, sizeof(*o));
    fix_format(work, sizeof(work), "%s/%s.work", fix_dir(), row);
    fix_format(record, sizeof(record), "%s/%s.record", fix_dir(), row);
    o->tree = tree;
    o->arch = "i686";
    o->work = work;
    o->record = record;
    o->seconds = 60;
    o->goals = goals;
    while (goals[o->ngoals] != NULL)
        o->ngoals++;
}

/** Run the preparation `o` and take what it printed into `r`. */
static void run(struct etude_legacy_opts *o, struct outcome *r)
{
    FILE *f = tmpfile();
    size_t n;

    r->out[0] = '\0';
    if (f == NULL) {
        printf("etude-prepare-test: fail: no file for the output: %s\n", strerror(errno));
        exit(1);
    }
    o->out = f;
    r->status = etude_legacy_prepare(o);
    rewind(f);
    n = fread(r->out, 1, sizeof(r->out) - 1, f);
    r->out[n] = '\0';
    fclose(f);
}

/** Print what a preparation printed when its row failed. */
static void show(const struct outcome *r, int before)
{
    if (fails != before)
        printf("etude-prepare-test: its output:\n%s", r->out);
}

/** Give this invocation a fresh receipt, or clear the optional inputs. */
static void receipt_env(const char *path)
{
    if (path) {
        setenv("ETUDE_PREPARATION_RUN_ID", "fixture-preparation-1", 1);
        setenv("ETUDE_PREPARATION_RECEIPT", path, 1);
        setenv("ETUDE_PREPARATION_COMMIT", "0123456789abcdef0123456789abcdef01234567", 1);
    } else {
        unsetenv("ETUDE_PREPARATION_RUN_ID");
        unsetenv("ETUDE_PREPARATION_RECEIPT");
        unsetenv("ETUDE_PREPARATION_COMMIT");
        unsetenv("ETUDE_PREPARATION_GRANT");
    }
}

/** The receipt binds the completed record and one nonempty clock interval. */
static void check_receipt(const char *path, const char *record,
                          const struct etude_preparation_grant *grant)
{
    char text[2048], error[160], hex[65];
    struct json *doc = NULL;
    const struct json *start, *end;
    long size = read_text(path, text, sizeof(text));
    CHECK(size > 0 &&
              !json_parse(text, (size_t)(size > 0 ? size : 0), 1, &doc, error, sizeof(error)),
          "receipt: valid JSON");
    if (!doc)
        return;
    sha_text(record, hex);
    CHECK(json_text(doc, "record_sha256") && !strcmp(json_text(doc, "record_sha256"), hex),
          "receipt: actual artifact record digest");
    CHECK(json_text(doc, "run_id") && !strcmp(json_text(doc, "run_id"), "fixture-preparation-1") &&
              json_text(doc, "outcome") && !strcmp(json_text(doc, "outcome"), "pass") &&
              json_text(doc, "cleanup") && !strcmp(json_text(doc, "cleanup"), "confirmed"),
          "receipt: the completed invocation");
    start = json_member(json_member(doc, "start"), "monotonic_us");
    end = json_member(json_member(doc, "end"), "monotonic_us");
    CHECK(start && end && start->type == JSON_INTEGER && end->type == JSON_INTEGER &&
              start->integer < end->integer,
          "receipt: elapsed preparation includes the build");
    CHECK(start && start->type == JSON_INTEGER &&
              (uint64_t)start->integer == grant->start.monotonic_us,
          "receipt: interval begins at the first admission grant");
    const struct json *conditioning = json_member(doc, "conditioning");
    CHECK(json_text(conditioning, "inhouse") &&
              !strcmp(json_text(conditioning, "inhouse"), "retained") &&
              json_text(conditioning, "backend") &&
              !strcmp(json_text(conditioning, "backend"), "ordinary") &&
              json_member(conditioning, "provenance") &&
              json_member(conditioning, "provenance")->type == JSON_NULL,
          "receipt: ordinary retained build is not labelled clean");
    json_free(doc);
}

/* ---- Rows. ---- */

/** Seed products and evidence together for a clean preparation. */
static void clean_products(const char *tree)
{
    static const char *const dirs[] = {
        "tests",        "build/tests",  "build/tests/host-suite", "build/tests/host-suite/current",
        "build/vendor", "build/x86_64", "build/x86_64/tests"};
    static const char *const files[] = {"build/tests/probe",
                                        "build/tests/probe.d",
                                        "build/x86_64/tests/probe",
                                        "build/x86_64/tests/probe.d",
                                        "build/tests/host-suite/stale",
                                        "build/tests/host-suite/current/keep",
                                        "build/vendor/keep"};
    char path[PATH_LEN];
    size_t i;
    for (i = 0; i < sizeof(dirs) / sizeof(*dirs); i++)
        CHECK(!mkdir(in(path, sizeof(path), tree, dirs[i]), 0755), "clean: product directory");
    for (i = 0; i < sizeof(files) / sizeof(*files); i++)
        CHECK(!write_text(in(path, sizeof(path), tree, files[i]), "retained\n", 0644),
              "clean: product file");
    CHECK(!write_text(in(path, sizeof(path), tree, "tests/Makefile"), "PROGS = probe \\\n other\n",
                      0644),
          "clean: literal probe list");
    CHECK(!symlink("../src", in(path, sizeof(path), tree, "build/tools")),
          "clean: linked product directory");
}

/** Rebuild products without removing caches or evidence. */
static void test_clean(void)
{
    char *goals[] = {"build/images/a.img", "build/x86_64/images/a.img", NULL};
    static const char *const removed[] = {
        "build/tests/probe",          "build/tests/probe.d",          "build/x86_64/tests/probe",
        "build/x86_64/tests/probe.d", "build/tests/host-suite/stale", "build/tools"};
    static const char *const retained[] = {"build/tests/host-suite/current/keep",
                                           "build/vendor/keep",
                                           "build/.run.lock/owner",
                                           "src/in.c",
                                           "build/images/a.img",
                                           "build/x86_64/images/a.img"};
    char tree[PATH_LEN], path[PATH_LEN], receipt[PATH_LEN], text[4096];
    struct etude_legacy_opts o;
    struct outcome r;
    size_t i;
    int before = fails;
    make_tree("clean", tree, sizeof(tree));
    clean_products(tree);
    opts_for(&o, tree, "clean", goals);
    o.arch = "i686,x86_64";
    fix_format(receipt, sizeof(receipt), "%s/build/tests/host-suite/current/receipt", tree);
    receipt_env(receipt);
    setenv("ETUDE_PREPARATION_CLEAN", "1", 1);
    run(&o, &r);
    unsetenv("ETUDE_PREPARATION_CLEAN");
    receipt_env(NULL);
    CHECK(!r.status, "clean: normal preparation completed");
    for (i = 0; i < sizeof(removed) / sizeof(*removed); i++)
        CHECK(!exists(in(path, sizeof(path), tree, removed[i])), "clean: obsolete product removed");
    for (i = 0; i < sizeof(retained) / sizeof(*retained); i++)
        CHECK(exists(in(path, sizeof(path), tree, retained[i])),
              "clean: retained input or new product");
    CHECK(read_text(receipt, text, sizeof(text)) > 0 && strstr(text, "\"inhouse\":\"clean\""),
          "clean: actual removal recorded in the receipt");
    show(&r, before);
}

/** Refuse an evaluated probe declaration before deleting any product. */
static void test_clean_refused(void)
{
    char *goals[] = {"build/images/a.img", NULL};
    char tree[PATH_LEN], path[PATH_LEN], receipt[PATH_LEN];
    struct etude_legacy_opts o;
    struct outcome r;
    int before = fails;
    make_tree("clean-refused", tree, sizeof(tree));
    clean_products(tree);
    CHECK(
        !write_text(in(path, sizeof(path), tree, "tests/Makefile"), "PROGS = $(computed)\n", 0644),
        "clean: nonliteral probe declaration");
    opts_for(&o, tree, "clean-refused", goals);
    fix_format(receipt, sizeof(receipt), "%s/clean-refused.receipt", fix_dir());
    receipt_env(receipt);
    setenv("ETUDE_PREPARATION_CLEAN", "1", 1);
    run(&o, &r);
    unsetenv("ETUDE_PREPARATION_CLEAN");
    receipt_env(NULL);
    CHECK(r.status == 2 && !exists(receipt), "clean: no receipt for refused declaration");
    CHECK(exists(in(path, sizeof(path), tree, "build/tests/probe")),
          "clean: product remains after declaration refusal");
    CHECK(!exists(in(path, sizeof(path), tree, "calls")), "clean: no build after refusal");
    show(&r, before);
}

/** Goals: the steps in order, the goals built, the record. */
static void test_alone(void)
{
    char *goals[] = {"build/images/a.img", "krffs/shadow", "build/images", NULL};
    char tree[PATH_LEN], path[PATH_LEN], receipt[PATH_LEN];
    char calls[4096], record[8192], files[1024], want[128], hex[65];
    struct etude_legacy_opts o;
    struct outcome r;
    struct stat st;
    int before = fails;

    make_tree("alone", tree, sizeof(tree));
    opts_for(&o, tree, "alone", goals);
    fix_format(receipt, sizeof(receipt), "%s/alone.receipt", fix_dir());
    struct etude_preparation_grant grant;
    CHECK(!etude_history_now(&grant.start) && !etude_context_clock(&grant.clock),
          "receipt: first grant is read");
    CHECK(!etude_context_grant_write(fix_path("alone.grant"), "fixture-preparation-1:prepare",
                                     &grant),
          "receipt: admission grant writes");
    receipt_env(receipt);
    setenv("ETUDE_PREPARATION_GRANT", fix_path("alone.grant"), 1);
    run(&o, &r);
    receipt_env(NULL);
    read_text(in(path, sizeof(path), tree, "calls"), calls, sizeof(calls));
    read_text(o.record, record, sizeof(record));
    record_files(record, files, sizeof(files));
    CHECK(r.status == 0, "alone: prepared");
    CHECK(has_line(r.out, "etude prepare: result: prepared: 3 goals, 3 files"), "alone: result");
    CHECK(line_at(calls, "recovery-key i686 build/rach2/kernel C") == 0, "alone: key first");
    CHECK(line_at(calls, "components i686 build/rach2/kernel C") >
              line_at(calls, "build/rach2/kernel i686 build/rach2/kernel C"),
          "alone: components");
    CHECK(line_at(calls, "build/images/a.img i686 build/rach2/kernel C") >
              line_at(calls, "components i686 build/rach2/kernel C"),
          "alone: artifacts after the components");
    CHECK(has_line(calls, "krffs/shadow i686 build/rach2/kernel C"), "alone: a tree's target");
    CHECK(strstr(calls, "g.img") == NULL, "alone: nothing else built");
    CHECK(strcmp(files, "build/images/a.img\nbuild/rach2/kernel\nkrffs/shadow\n") == 0,
          "alone: the record's files");
    sha_text("src\n", hex);
    fix_format(want, sizeof(want), "file %s 4 ", hex);
    CHECK(strstr(record, want) != NULL, "alone: a file's digest and size");
    CHECK(has_line(record, "etude-prepare 1") && has_line(record, "arch i686") &&
              has_line(record, "kernel build/rach2/kernel"),
          "alone: the record's head");
    CHECK(exists(in(path, sizeof(path), tree, "krffs/recovery_authorized_keys")),
          "alone: the key minted");
    CHECK(stat(in(path, sizeof(path), tree, "krffs/rach2_demo_ed25519"), &st) == 0 &&
              (st.st_mode & 0777) == 0600,
          "alone: the demo key's mode");
    CHECK(!exists(o.work), "alone: the work removed");
    show(&r, before);
    check_receipt(receipt, record, &grant);
    receipt_env(receipt);
    run(&o, &r);
    receipt_env(NULL);
    CHECK(r.status == 2 && strstr(r.out, "receipt must be a new file"),
          "receipt: duplicate refused before preparing");
    check_receipt(receipt, record, &grant);
}

/** The same goals again: no key minted, nothing rebuilt. */
static void test_again(void)
{
    char *goals[] = {"build/images/a.img", "krffs/shadow", "build/images", NULL};
    char tree[PATH_LEN], path[PATH_LEN], calls[4096];
    struct etude_legacy_opts o;
    struct outcome r;
    int before = fails;

    fix_format(tree, sizeof(tree), "%s/alone", fix_dir());
    unlink(in(path, sizeof(path), tree, "calls"));
    opts_for(&o, tree, "again", goals);
    run(&o, &r);
    read_text(in(path, sizeof(path), tree, "calls"), calls, sizeof(calls));
    CHECK(r.status == 0, "again: prepared");
    CHECK(strcmp(calls, "components i686 build/rach2/kernel C\n") == 0,
          "again: only the phony components");
    CHECK(strstr(r.out, "step: recovery-key") == NULL, "again: no key minted");
    show(&r, before);
}

/** x86_64 and a kernel of its own reach every make and the record. */
static void test_machine(void)
{
    char *goals[] = {"build/x86_64/images/a.img", NULL};
    char tree[PATH_LEN], path[PATH_LEN], calls[4096], record[8192], files[1024];
    const char *k = "build/x86_64/rach2-ubsan/kernel";
    struct etude_legacy_opts o;
    struct outcome r;
    int before = fails;

    make_tree("machine", tree, sizeof(tree));
    opts_for(&o, tree, "machine", goals);
    o.arch = "x86_64";
    o.kernel = "ubsan";
    run(&o, &r);
    read_text(in(path, sizeof(path), tree, "calls"), calls, sizeof(calls));
    read_text(o.record, record, sizeof(record));
    record_files(record, files, sizeof(files));
    CHECK(r.status == 0, "machine: prepared");
    CHECK(line_at(calls, "ubsan-kernel x86_64 build/x86_64/rach2-ubsan/kernel C") >
              line_at(calls, "components x86_64 build/x86_64/rach2-ubsan/kernel C"),
          "machine: the kernel after the components");
    CHECK(has_line(calls, "build/x86_64/images/a.img x86_64 build/x86_64/rach2-ubsan/kernel C"),
          "machine: the goal");
    CHECK(strcmp(files, "build/x86_64/images/a.img\nbuild/x86_64/rach2-ubsan/kernel\n") == 0,
          "machine: the record's files");
    fix_format(path, sizeof(path), "kernel %s", k);
    CHECK(has_line(record, path) && has_line(record, "arch x86_64"), "machine: the record's head");
    show(&r, before);
}

/** A fixed x86_64 helper is prepared beside the selected i686 artifacts. */
static void test_helper_architecture(void)
{
    char *goals[] = {"build/images/a.img", "build/x86_64/images/a.img", NULL};
    char tree[PATH_LEN], path[PATH_LEN], calls[4096], record[8192];
    struct etude_legacy_opts o;
    struct outcome r;
    int before = fails;

    make_tree("helper-architecture", tree, sizeof(tree));
    opts_for(&o, tree, "helper-architecture", goals);
    run(&o, &r);
    CHECK(r.status == 0, "helper architecture: prepared");
    read_text(in(path, sizeof(path), tree, "calls"), calls, sizeof(calls));
    CHECK(has_line(calls, "build/images/a.img i686 build/rach2/kernel C"),
          "helper architecture: selected artifact uses i686");
    CHECK(has_line(calls, "build/x86_64/images/a.img x86_64 build/x86_64/rach2/kernel C"),
          "helper architecture: fixed helper artifact uses x86_64");
    CHECK(line_at(calls, "components x86_64 build/x86_64/rach2/kernel C") >
              line_at(calls, "build/images/a.img i686 build/rach2/kernel C"),
          "helper architecture: helper components follow selected artifacts");
    read_text(o.record, record, sizeof(record));
    CHECK(has_line(record, "arch i686") && has_line(record, "arch x86_64") &&
              strstr(record, " build/images/a.img\n") &&
              strstr(record, " build/x86_64/images/a.img\n"),
          "helper architecture: record includes both architectures and artifacts");
    show(&r, before);
}

/**
 * Prepare `goals` of tree `tree` as row `row`, with `arch` and `kernel`,
 * and require a refusal with `why` and nothing built or left.
 */
static void refused(const char *tree, const char *row, char *const *goals, const char *arch,
                    const char *kernel, const char *why)
{
    char path[PATH_LEN], line[256];
    struct etude_legacy_opts o;
    struct outcome r;
    int before = fails;

    opts_for(&o, tree, row, goals);
    o.arch = arch;
    o.kernel = kernel;
    run(&o, &r);
    fix_format(line, sizeof(line), "etude prepare: result: refused: %s", why);
    CHECK(r.status == 2, row);
    CHECK(has_line(r.out, line), row);
    CHECK(!exists(in(path, sizeof(path), tree, "calls")), row);
    CHECK(!exists(o.work) && !exists(o.record), row);
    show(&r, before);
}

/** Each refusal builds nothing. */
static void test_refused(void)
{
    char *none[] = {NULL}, *a[] = {"build/images/a.img", NULL};
    char *absolute[] = {"build/images/a.img", "/x", NULL}, *parent[] = {"build/../x", NULL};
    char tree[PATH_LEN], held[32];

    make_tree("refused", tree, sizeof(tree));
    refused(tree, "none", none, "i686", NULL, "no goal named");
    refused(tree, "absolute", absolute, "i686", NULL,
            "/x: a goal is a relative path inside the tree");
    refused(tree, "parent", parent, "i686", NULL,
            "build/../x: a goal is a relative path inside the tree");
    refused(tree, "arm", a, "arm", NULL, "arm: the architectures are i686 and x86_64");
    refused(tree, "kernel", a, "i686", "../x",
            "../x: a kernel's name is letters, digits, '_' and '-'");
    snprintf(held, sizeof(held), "%ld", (long)getppid());
    setenv("RACH2_TREE_HELD", held, 1);
    refused(tree, "held", a, "i686", NULL,
            "the tree is not held: RACH2_TREE_HELD must name the running process that "
            "build/.run.lock/owner names");
    snprintf(held, sizeof(held), "%ld", (long)getpid());
    setenv("RACH2_TREE_HELD", held, 1);
    setenv("ETUDE_PREPARATION_RUN_ID", "partial", 1);
    refused(tree, "receipt-partial", a, "i686", NULL,
            "a preparation receipt needs RUN_ID, RECEIPT and COMMIT variables "
            "and an artifact record");
    receipt_env(NULL);
    struct etude_preparation_grant grant;
    CHECK(!etude_history_now(&grant.start) && !etude_context_clock(&grant.clock) &&
              !etude_context_grant_write(fix_path("wrong.grant"), "another:prepare", &grant),
          "refused: unrelated grant is prepared");
    receipt_env(fix_path("wrong-grant.receipt"));
    setenv("ETUDE_PREPARATION_GRANT", fix_path("wrong.grant"), 1);
    refused(tree, "grant-wrong", a, "i686", NULL,
            "the preparation grant does not match this run and clock");
    receipt_env(NULL);
    setenv("ETUDE_PREPARATION_GRANT", fix_path("wrong.grant"), 1);
    refused(tree, "grant-unpaired", a, "i686", NULL,
            "a preparation receipt needs RUN_ID, RECEIPT and COMMIT variables "
            "and an artifact record");
    receipt_env(NULL);
}

/** A failed step stops the preparation and keeps its log. */
static void test_failed(void)
{
    char *goals[] = {"build/images/a.img", NULL};
    char tree[PATH_LEN], path[PATH_LEN], calls[4096], log[4096], line[PATH_LEN + 64], *work;
    struct etude_legacy_opts o;
    struct outcome r;
    int before = fails;
    const char *receipt = fix_path("failed.receipt");

    make_tree("failed", tree, sizeof(tree));
    write_text(in(path, sizeof(path), tree, "fail-components"), "", 0644);
    opts_for(&o, tree, "failed", goals);
    receipt_env(receipt);
    run(&o, &r);
    receipt_env(NULL);
    read_text(in(path, sizeof(path), tree, "calls"), calls, sizeof(calls));
    fix_format(path, sizeof(path), "%s/3-components.log", o.work);
    read_text(path, log, sizeof(log));
    CHECK(r.status == 1, "failed: failed");
    CHECK(has_line(r.out, "etude prepare: result: failed: components: it exited 2"),
          "failed: the result names the step");
    work = realpath(o.work, NULL);
    fix_format(line, sizeof(line), "etude prepare: evidence: %s", work != NULL ? work : o.work);
    free(work);
    CHECK(has_line(r.out, line) && strstr(log, "components") != NULL, "failed: its log kept");
    CHECK(strstr(r.out, "step: artifacts") == NULL && strstr(calls, "a.img") == NULL,
          "failed: no step after it");
    CHECK(!exists(o.record), "failed: no record");
    CHECK(!exists(receipt), "failed: no successful receipt");
    show(&r, before);
}

/** A goal that its rule does not make fails the preparation. */
static void test_missing(void)
{
    char *goals[] = {"build/images/d.img", NULL};
    char tree[PATH_LEN];
    struct etude_legacy_opts o;
    struct outcome r;
    int before = fails;

    make_tree("missing", tree, sizeof(tree));
    opts_for(&o, tree, "missing", goals);
    run(&o, &r);
    CHECK(r.status == 1, "missing: failed");
    CHECK(has_line(r.out, "etude prepare: missing: build/images/d.img: No such file or directory"),
          "missing: the goal named");
    CHECK(has_line(r.out, "etude prepare: result: failed: 1 of 2 artifacts are missing"),
          "missing: the result");
    CHECK(!exists(o.record), "missing: no record");
    show(&r, before);
}

/** Say whether nothing has the FIFO `path` open for reading. */
static int no_reader(const char *path)
{
    int fd = open(path, O_WRONLY | O_NONBLOCK);

    if (fd >= 0) {
        close(fd);
        return 0;
    }
    return errno == ENXIO;
}

/** A step past its bound is ended, with what it started. */
static void test_bound(void)
{
    char *goals[] = {"build/images/h.img", NULL};
    char tree[PATH_LEN], path[PATH_LEN];
    struct etude_legacy_opts o;
    struct outcome r;
    int before = fails;

    make_tree("bound", tree, sizeof(tree));
    opts_for(&o, tree, "bound", goals);
    o.seconds = 1;
    run(&o, &r);
    CHECK(r.status == 1, "bound: failed");
    CHECK(has_line(r.out, "etude prepare: result: failed: artifacts: it ran past its bound of 1 s"),
          "bound: the result");
    CHECK(no_reader(in(path, sizeof(path), tree, "hang.fifo")), "bound: its recipe ended");
    show(&r, before);
}

/* The stop signal the child of the stop row received. */
static volatile sig_atomic_t child_stop;

/** Note the stop signal in the child of the stop row. */
static void note_stop(int sig)
{
    child_stop = sig;
}

/** Run the preparation `o` in a child that a SIGTERM stops, with its output
 * in the file `out`; its pid. */
static pid_t start_child(struct etude_legacy_opts *o, const char *out)
{
    struct sigaction sa;
    pid_t pid = fork();
    int status;

    if (pid != 0)
        return pid;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = note_stop;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    o->stop = &child_stop;
    if ((o->out = fopen(out, "w")) == NULL)
        _exit(1);
    status = etude_legacy_prepare(o);
    fclose(o->out);
    _exit(status & 0xff);
}

/**
 * Wait for a byte on `fd` while the child `pid` runs, and read what came into
 * `buf`; 1, or 0 when the child ended first with nothing written.
 */
static int await_start(int fd, pid_t pid, char *buf, size_t size)
{
    struct pollfd pfd;
    siginfo_t si;

    pfd.fd = fd;
    pfd.events = POLLIN;
    for (;;) {
        /* interval: the child is looked at again every 50 ms */
        if (poll(&pfd, 1, 50) == 1)
            return read(fd, buf, size) > 0;
        memset(&si, 0, sizeof(si));
        if (waitid(P_PID, (id_t)pid, &si, WEXITED | WNOHANG | WNOWAIT) != 0 || si.si_pid != 0)
            return poll(&pfd, 1, 0) == 1 && read(fd, buf, size) > 0;
    }
}

/** A stop signal ends the step that runs and removes the work. */
static void test_stop(void)
{
    char *goals[] = {"build/images/w.img", NULL};
    char tree[PATH_LEN], path[PATH_LEN], got[16];
    const char *out = fix_path("stop.out");
    const char *receipt = fix_path("stop.receipt");
    struct etude_legacy_opts o;
    struct outcome r;
    int status = -1, fd, before = fails;
    pid_t pid;

    make_tree("stop", tree, sizeof(tree));
    opts_for(&o, tree, "stop", goals);
    o.seconds = 30;
    fd = open(in(path, sizeof(path), tree, "started.fifo"), O_RDWR);
    fflush(stdout);
    receipt_env(receipt);
    pid = start_child(&o, out);
    receipt_env(NULL);
    CHECK(fd >= 0 && pid > 0 && await_start(fd, pid, got, sizeof(got)), "stop: the step started");
    if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
    }
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGTERM, "stop: stopped");
    CHECK(no_reader(in(path, sizeof(path), tree, "hang.fifo")), "stop: its recipe ended");
    CHECK(!exists(o.work) && !exists(o.record), "stop: the work removed");
    CHECK(!exists(receipt), "stop: no successful receipt");
    if (fd >= 0)
        close(fd);
    read_text(out, r.out, sizeof(r.out));
    show(&r, before);
}

/** Give the status of `etude_legacy_check` on `record` and its output. */
static int check(const char *record, int digests, char *out, size_t max)
{
    FILE *f = tmpfile();
    size_t n;
    int status;

    if (f == NULL)
        return -1;
    status = etude_legacy_check(record, digests, f);
    rewind(f);
    n = fread(out, 1, max - 1, f);
    out[n] = '\0';
    fclose(f);
    return status;
}

/** Write `text` over the file `path` in place, keeping its times. */
static void overwrite(const char *path, const char *text)
{
    struct timespec times[2];
    struct stat st;
    int fd = open(path, O_WRONLY);

    if (fd < 0 || fstat(fd, &st) != 0 || write(fd, text, strlen(text)) != (ssize_t)strlen(text)) {
        CHECK(0, "check: a file overwritten");
    } else {
        times[0].tv_sec = st.st_atime;
        times[0].tv_nsec = UTIME_OMIT;
#if defined(__APPLE__)
        times[1] = st.st_mtimespec;
#else
        times[1] = st.st_mtim;
#endif
        CHECK(futimens(fd, times) == 0, "check: a file's time kept");
    }
    if (fd >= 0)
        close(fd);
}

/** The record by status and by digest, after the files change. */
static void test_check(void)
{
    char tree[PATH_LEN], record[PATH_LEN], img[PATH_LEN], tmp[PATH_LEN], out[4096];

    fix_format(tree, sizeof(tree), "%s/alone", fix_dir());
    fix_format(record, sizeof(record), "%s/alone.record", fix_dir());
    in(img, sizeof(img), tree, "build/images/a.img");
    CHECK(check(record, 0, out, sizeof(out)) == 0 && check(record, 1, out, sizeof(out)) == 0 &&
              has_line(out, "etude check: result: unchanged: 3 files"),
          "check: unchanged");
    overwrite(img, "SRC\n");
    CHECK(check(record, 0, out, sizeof(out)) == 0, "check: same size and time pass by status");
    CHECK(check(record, 1, out, sizeof(out)) == 1 &&
              has_line(out, "etude check: changed: build/images/a.img"),
          "check: a changed content found by digest");
    fix_format(tmp, sizeof(tmp), "%s.new", img);
    write_text(tmp, "src\n", 0644);
    rename(tmp, img);
    CHECK(check(record, 0, out, sizeof(out)) == 1 &&
              has_line(out, "etude check: result: changed: 1 of 3 files"),
          "check: a new file found by status");
    CHECK(check(record, 1, out, sizeof(out)) == 0, "check: the same content passes by digest");
    unlink(in(tmp, sizeof(tmp), tree, "krffs/shadow"));
    CHECK(check(record, 1, out, sizeof(out)) == 1 &&
              has_line(out, "etude check: missing: krffs/shadow: No such file or directory"),
          "check: a missing file");
    fix_format(tmp, sizeof(tmp), "%s.bad", record);
    write_text(tmp, "etude-prepare 1\ntree /\nfile 00 1 2 3 x\n", 0644);
    CHECK(check(tmp, 0, out, sizeof(out)) == 2, "check: a malformed record");
    write_text(tmp, "etude-prepare 2\n", 0644);
    CHECK(check(tmp, 0, out, sizeof(out)) == 2, "check: another record");
}

/** Run as a graph's recipe: prepare the goals ARGV[4...] of tree ARGV[2]
 * into the record ARGV[3]. */
static int recipe(int argc, char **argv)
{
    struct etude_legacy_opts o;

    memset(&o, 0, sizeof(o));
    o.tree = argv[2];
    o.arch = "i686";
    o.record = argv[3];
    o.seconds = 60;
    o.goals = argv + 4;
    o.ngoals = (size_t)(argc - 4);
    o.out = stdout;
    return etude_legacy_prepare(&o);
}

/** A preparation graph publishes the record as a fresh node's product. */
static void test_graph(void)
{
    char tree[PATH_LEN], dir[PATH_LEN], obj[PATH_LEN], text[8192];
    char *argv[] = {self, "--legacy", tree, "${WORK}/record", "build/images/a.img", NULL};
    struct etude_prep_node n;
    struct etude_prep_opts po;
    struct etude_prep *g = etude_prep_new();
    struct etude_store *s;
    struct etude_exec *ex = etude_exec_new();
    const struct etude_prep_result *res;

    make_tree("graph", tree, sizeof(tree));
    fix_format(dir, sizeof(dir), "%s/graph.store", fix_dir());
    s = etude_store_open(dir);
    memset(&n, 0, sizeof(n));
    n.name = "legacy";
    n.argv = argv;
    n.output = "record";
    n.fresh = 1;
    n.bound_us = 60000000u;
    memset(&po, 0, sizeof(po));
    po.scratch = fix_dir();
    po.jobs = 1;
    po.output_max = 65536;
    po.grace_us = 2000000u;
    CHECK(g != NULL && s != NULL && ex != NULL && etude_prep_add(g, &n) == 0 &&
              etude_prep_run(g, s, ex, &po) == 0,
          "graph: the node built");
    res = g != NULL ? etude_prep_result(g, "legacy") : NULL;
    CHECK(res != NULL && res->state == ETUDE_PREP_BUILT &&
              etude_store_path(s, res->digest, obj, sizeof(obj)) == 0 &&
              read_text(obj, text, sizeof(text)) > 0 &&
              strncmp(text, "etude-prepare 1\n", 16) == 0 && has_line(text, "arch i686") &&
              strstr(text, " build/images/a.img\n") != NULL,
          "graph: the record published");
    if (res != NULL && res->state != ETUDE_PREP_BUILT)
        printf("etude-prepare-test: the recipe's errors: %s\n", res->why);
    etude_exec_free(ex);
    etude_prep_free(g);
    if (s != NULL)
        etude_store_close(s);
}

/* Check the preparation importer in the GNU fixture. */
static void test_import_product(void)
{
    char *args[] = {(char *)host_tool_path("python3"), "-B",
                    "tests/host/preparation_import_test.py", NULL};
    struct etude_exec *ex = etude_exec_new();
    struct etude_proc *proc = NULL;
    struct etude_exec_spec spec;
    const struct etude_exec_result *result;
    int started;

    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = "preparation-import";
    spec.argv = args;
    spec.envp = environ;
    spec.cwd = getenv("ETUDE_TREE");
    spec.output_max = 65536;
    spec.grace_us = 5000000;
    started = ex && !etude_exec_start(ex, &spec, &proc);
    if (!started)
        fprintf(stderr, "import product: %s: %s\n", args[0],
                ex ? strerror(errno) : "cannot create the native executor");
    CHECK(started, "import product: started");
    if (proc) {
        while (etude_exec_wait(ex, 1000) != proc)
            ;
        result = etude_proc_result(proc);
        CHECK(result->end == ETUDE_END_EXITED && !result->value &&
                  result->cleanup == ETUDE_CLEANUP_CONFIRMED && !result->output_error,
              "import product: identity, receipt, fixture chain and native build vectors");
        if (result->end != ETUDE_END_EXITED || result->value ||
            result->cleanup != ETUDE_CLEANUP_CONFIRMED || result->output_error)
            printf("import product: end=%d value=%d cleanup=%d output_error=%d\n%s%s", result->end,
                   result->value, result->cleanup, result->output_error,
                   result->out.data ? result->out.data : "",
                   result->err.data ? result->err.data : "");
        etude_proc_free(proc);
    }
    etude_exec_free(ex);
}

#include "compat/context_source_host.c"

int main(int argc, char **argv)
{
    char held[32];

    if (argc >= 5 && strcmp(argv[1], "--legacy") == 0)
        return recipe(argc, argv);
    if (realpath(argv[0], self) == NULL) {
        printf("etude-prepare-test: fail: %s cannot be found\n", argv[0]);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    snprintf(held, sizeof(held), "%ld", (long)getpid());
    setenv("RACH2_TREE_HELD", held, 1);
    setenv("LC_ALL", "POSIX", 1);
    setenv("MAKEFLAGS", "n", 1);
    setenv("ARCH", "x86_64", 1);
    if (argc == 2 && !strcmp(argv[1], "--source-only")) {
        test_source_capture();
        if (fails)
            return 1;
        fix_passed();
        puts("etude_legacy_hosttest: source pass");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--clean-only")) {
        test_clean();
        test_clean_refused();
        if (fails)
            return 1;
        fix_passed();
        puts("etude_legacy_hosttest: clean pass");
        return 0;
    }
    test_alone();
    if (argc == 2 && !strcmp(argv[1], "--receipt-only")) {
        test_refused();
        if (fails)
            return 1;
        fix_passed();
        puts("etude_legacy_hosttest: receipt pass");
        return 0;
    }
    test_again();
    test_clean();
    test_clean_refused();
    test_source_capture();
    test_machine();
    test_helper_architecture();
    test_refused();
    test_failed();
    test_missing();
    test_bound();
    test_stop();
    test_check();
    test_graph();
    test_import_product();
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_legacy_hosttest: pass\n");
    return 0;
}
