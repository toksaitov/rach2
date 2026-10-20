/**
 * Check etude's driver of mutation proofs (tools/etude/proof.c and
 * tools/etude/posix/proof.c) on toy proofs whose builds and tests are shell
 * commands over two small sources.
 *
 * host-deps: tools/etude/proof.c tools/etude/posix/proof.c
 * host-deps: tools/etude/json.c tools/etude/schema.c
 * host-deps: tools/etude/posix/store.c tools/etude/capture.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-deps: tools/etude/posix/process.c utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude -I tools/etude/posix
 * host-cflags: -Wall -Wextra -Werror
 *
 * A catalogue with a fault named twice, an edit of a file that is not a
 * source, an unknown placeholder, `only` without a failure prefix or a
 * missing member is refused. An edit applies only to a text found once, and
 * the texts check finds a text that is missing or found twice. A sound proof
 * passes with three distinct artifacts in its store, the restored ones equal
 * to the base's, and leaves the tree as it was, also when its build judges by
 * time stamps and gives its artifact a time ahead of the clock. A fault that
 * does not reach the artifacts, one the test misses, one that fails with
 * another status, without its line or with a line it does not name, and an
 * edit whose text is not found once each fail the proof. So do a faulty run
 * that leaves state the restored build takes in, a build that changes its
 * source, a test that prints a failure and passes, and a test that runs past
 * its bound. A shared proof needs the
 * tree's run lock, puts its faults into the tree and restores it, also when
 * a signal stops it while a faulty test runs.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "compat/fixdir_host.c"
#include "etude/proof.h"

static int fails;
/* Nonzero: prove() runs the base phase alone, as `--base` asks. */
static int base_only;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                         \
    do {                                                                                         \
        if (!(cond)) {                                                                           \
            printf("etude-proof-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), #cond); \
            fails++;                                                                             \
        }                                                                                        \
    } while (0)

/* The pieces of a catalogue's faults, as JSON text. */
#define EDIT(f, a, b) "{\"file\": \"" f "\", \"find\": \"" a "\", \"replace\": \"" b "\"}"
#define FAULT(n, e, f) \
    "{\"name\": \"" n "\", \"summary\": \"" n "\", \"edits\": [" e "], \"fails\": " f "}"
#define FAILS(st, l, only) "{\"status\": " #st ", \"lines\": [" l "], \"only\": " only "}"

/* The two faults of a sound proof: a bad value, and a bad value and count. */
#define F_VALUE                                                  \
    FAULT("value", EDIT("src/a.txt", "value=good", "value=bad"), \
          FAILS(1, "\"toy-test: fail: value\"", "true"))
#define F_BOTH                                                                                    \
    FAULT(                                                                                        \
        "both",                                                                                   \
        EDIT("src/a.txt", "value=good", "value=bad") "," EDIT("src/b.txt", "count=2", "count=3"), \
        FAILS(1, "\"toy-test: fail: value\", \"toy-test: fail: count\"", "true"))

/* The build: the sources without their comments. */
#define BUILD "cat ${SRC}/src/a.txt ${SRC}/src/b.txt | grep -v '^#' > ${WORK}/art"

/* The test: one failure line for each wrong value. */
#define TEST                                                                              \
    "f=0; grep -q '^value=good$' ${WORK}/art || { echo 'toy-test: fail: value'; f=1; }; " \
    "grep -q '^count=2$' ${WORK}/art || { echo 'toy-test: fail: count'; f=1; }; exit $f"

/* The schemas of this tree, which the tests run from the top of. */
#define SCHEMAS "tools/etude/schemas"

/* A catalogue, as a row writes it. */
struct cat {
    const char *tree;   /* "private" or "shared" */
    const char *build;  /* shell text */
    const char *test;   /* shell text */
    int test_bound;     /* seconds */
    const char *faults; /* the members of `faults`, as JSON text */
};

/** Write `text` to `path`, replacing it; end the test when it cannot. */
static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");

    if (f == NULL || fputs(text, f) < 0 || fclose(f) != 0) {
        printf("etude-proof-test: fail: %s cannot be written\n", path);
        exit(1);
    }
}

/** Give the text of the file at `path` in `buf` of `size`, or "". */
static const char *get(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    size_t n = 0;

    if (f != NULL) {
        n = fread(buf, 1, size - 1, f);
        fclose(f);
    }
    buf[n] = '\0';
    return buf;
}

/** Write `s` as a JSON string to `f`; the commands have no control bytes. */
static void put_string(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s != '\0'; s++) {
        if (*s == '"' || *s == '\\')
            fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

/** Write catalogue `c` to the file `path`. */
static void write_cat(const char *path, const struct cat *c)
{
    FILE *f = fopen(path, "w");

    if (f == NULL) {
        printf("etude-proof-test: fail: %s cannot be written\n", path);
        exit(1);
    }
    fprintf(f,
            "{\"schema_version\": 1, \"proof\": \"toy\", \"summary\": \"a toy\", "
            "\"tree\": \"%s\", \"sources\": [\"src/a.txt\", \"src/b.txt\"], "
            "\"artifacts\": [\"${WORK}/art\"], \"build\": {\"argv\": [\"/bin/sh\", \"-c\", ",
            c->tree);
    put_string(f, c->build);
    fputs("], \"bound_s\": 30}, \"test\": {\"argv\": [\"/bin/sh\", \"-c\", ", f);
    put_string(f, c->test);
    fprintf(f, "], \"bound_s\": %d, \"fail_prefix\": \"toy-test: fail: \"}, \"faults\": [%s]}\n",
            c->test_bound, c->faults);
    fclose(f);
}

/** Put the path of `name` in the fixture directory into `buf` of `size`. */
static void place(char *buf, size_t size, const char *name)
{
    fix_format(buf, size, "%s/%s", fix_dir(), name);
}

/** Make the tree of row `row` with its two sources, and put its path in
 * `tree`, of 1024 bytes. */
static void make_tree(const char *row, char *tree)
{
    char name[64], path[1100];

    fix_format(name, sizeof name, "tree-%s", row);
    place(tree, 1024, name);
    fix_format(path, sizeof path, "%s/src", tree);
    if ((mkdir(tree, 0700) != 0 && errno != EEXIST) ||
        (mkdir(path, 0700) != 0 && errno != EEXIST)) {
        printf("etude-proof-test: fail: %s cannot be made\n", path);
        exit(1);
    }
    fix_format(path, sizeof path, "%s/src/a.txt", tree);
    put(path, "value=good\n# note\n");
    fix_format(path, sizeof path, "%s/src/b.txt", tree);
    put(path, "count=2\n");
}

/** Say whether row `row`'s tree has its two sources as they were made. */
static int tree_intact(const char *tree)
{
    char path[1100], buf[256];

    fix_format(path, sizeof path, "%s/src/a.txt", tree);
    if (strcmp(get(path, buf, sizeof buf), "value=good\n# note\n") != 0)
        return 0;
    fix_format(path, sizeof path, "%s/src/b.txt", tree);
    return strcmp(get(path, buf, sizeof buf), "count=2\n") == 0;
}

/* What a run of a proof gave. */
struct outcome {
    int status;
    char log[16384];
    char work[1024];
};

/**
 * Run catalogue `c` of row `row` over `tree` with the faults `only` asks
 * for, keeping the work when `keep`, and give the outcome in `*out`.
 */
static void prove(const char *row, const char *tree, const struct cat *c, char *const *only,
                  int keep, struct outcome *out)
{
    struct etude_proof_opts o;
    struct etude_proof *p = NULL;
    char name[64], path[1024], cat[1024];
    FILE *log;

    fix_format(name, sizeof name, "cat-%s.json", row);
    place(cat, sizeof cat, name);
    write_cat(cat, c);
    fix_format(name, sizeof name, "log-%s", row);
    place(path, sizeof path, name);
    log = fopen(path, "w+");
    fix_format(name, sizeof name, "work-%s", row);
    place(out->work, sizeof out->work, name);
    out->status = -1;
    out->log[0] = '\0';
    if (log == NULL)
        return;
    out->status = etude_proof_read(cat, SCHEMAS, log, &p);
    if (out->status == 0) {
        memset(&o, 0, sizeof(o));
        o.tree = tree;
        o.work = out->work;
        o.only = only;
        o.keep = keep;
        o.base = base_only;
        o.log = log;
        out->status = etude_proof_run(p, &o);
    }
    etude_proof_free(p);
    fclose(log);
    get(path, out->log, sizeof out->log);
}

/** Say whether the directory or file `path` exists. */
static int exists(const char *path)
{
    struct stat st;

    return lstat(path, &st) == 0;
}

/** Print a row's log when its checks failed, so that the failure is read. */
static void show(const char *row, const struct outcome *out, int before)
{
    if (fails != before)
        printf("etude-proof-test: %s: log:\n%s", row, out->log);
}

/** Read catalogue text `json` and give the status, with the findings. */
static int read_text(const char *row, const char *json, char *log, size_t size)
{
    struct etude_proof *p = NULL;
    char name[64], cat[1024], path[1100];
    FILE *f;
    int status;

    fix_format(name, sizeof name, "read-%s.json", row);
    place(cat, sizeof cat, name);
    put(cat, json);
    fix_format(path, sizeof path, "%s.log", cat);
    f = fopen(path, "w");
    if (f == NULL)
        return -1;
    status = etude_proof_read(cat, SCHEMAS, f, &p);
    fclose(f);
    get(path, log, size);
    etude_proof_free(p);
    return status;
}

/* The start and the end of a catalogue around its faults. */
#define CAT_HEAD                                                                                  \
    "{\"schema_version\": 1, \"proof\": \"toy\", \"summary\": \"a toy\", \"tree\": \"private\", " \
    "\"sources\": [\"src/a.txt\", \"src/b.txt\"], \"artifacts\": [\"${WORK}/art\"], "             \
    "\"build\": {\"argv\": [\"/bin/sh\", \"-c\", \"exec ${CC:-cc} ${SRC}/x.c\"], \"bound_s\": "   \
    "5}, "
#define CAT_TEST(prefix)                                                  \
    "\"test\": {\"argv\": [\"${WORK}/art\"], \"bound_s\": 5" prefix "}, " \
    "\"faults\": ["
#define PREFIX ", \"fail_prefix\": \"toy-test: fail: \""

/** A catalogue is read, and refused for each rule it breaks. */
static void test_read(void)
{
    char log[4096];

    CHECK(read_text("good", CAT_HEAD CAT_TEST(PREFIX) F_VALUE "," F_BOTH "]}", log, sizeof log) ==
              0,
          "read: a sound catalogue, with a shell's ${CC:-cc}, is read");
    CHECK(read_text("twice", CAT_HEAD CAT_TEST(PREFIX) F_VALUE "," F_VALUE "]}", log, sizeof log) ==
                  1 &&
              strstr(log, "fault value is given twice") != NULL,
          "read: a fault named twice is refused");
    CHECK(read_text("other",
                    CAT_HEAD CAT_TEST(PREFIX) FAULT("other", EDIT("src/c.txt", "a", "b"),
                                                    FAILS(1, "\"x\"", "false")) "]}",
                    log, sizeof log) == 1 &&
              strstr(log, "fault other edits src/c.txt, which is not a source") != NULL,
          "read: an edit of a file that is not a source is refused");
    CHECK(read_text("place",
                    "{\"schema_version\": 1, \"proof\": \"toy\", \"summary\": \"a toy\", "
                    "\"tree\": \"private\", \"sources\": [\"src/a.txt\"], "
                    "\"artifacts\": [\"${WROK}/art\"], \"build\": {\"argv\": [\"/bin/true\"], "
                    "\"bound_s\": 5}, " CAT_TEST(PREFIX) F_VALUE "]}",
                    log, sizeof log) == 1 &&
              strstr(log, "an unknown placeholder: ${WROK}") != NULL,
          "read: an unknown placeholder is refused");
    CHECK(read_text("only", CAT_HEAD CAT_TEST("") F_VALUE "]}", log, sizeof log) == 1 &&
              strstr(log, "fault value is `only` and the test has no fail_prefix") != NULL,
          "read: `only` without a failure prefix is refused");
    CHECK(read_text("member", CAT_HEAD "\"faults\": [" F_VALUE "]}", log, sizeof log) == 1 &&
              strstr(log, "has no member \"test\"") != NULL,
          "read: a catalogue without its test is refused");
}

/** An edit applies to a text found once, and the texts check finds the rest. */
static void test_edits(void)
{
    struct etude_edit e = {"f", "bb", "X"};
    struct etude_proof *p = NULL;
    char *out = NULL, log[4096], tree[1024], cat[1024], name[1100];
    size_t len = 0;
    FILE *f;

    make_tree("texts", tree);
    place(cat, sizeof cat, "read-texts.json");

    CHECK(etude_edit_apply("aabbcc", 6, &e, &out, &len) == 1 && out != NULL && len == 5 &&
              strcmp(out, "aaXcc") == 0,
          "edits: a text found once is replaced");
    free(out);
    CHECK(etude_edit_apply("aacc", 4, &e, &out, &len) == 0 && out == NULL,
          "edits: a text not found is counted and not replaced");
    CHECK(etude_edit_apply("bbabb", 5, &e, &out, &len) == 2 && out == NULL,
          "edits: a text found twice is counted and not replaced");
    CHECK(etude_edit_apply("bbb", 3, &e, &out, &len) == 2 && out == NULL,
          "edits: overlapping texts are each counted");
    put(cat,
        CAT_HEAD CAT_TEST(PREFIX) F_VALUE "," FAULT(
            "chain",
            EDIT("src/a.txt", "value=good", "value=goodX") "," EDIT("src/a.txt", "goodX", "bad"),
            FAILS(1, "\"x\"", "false")) "]}");
    if (etude_proof_read(cat, SCHEMAS, stdout, &p) == 0) {
        CHECK(etude_proof_texts(p, tree, stdout) == 0,
              "texts: each edit finds its text in what the earlier ones left");
        etude_proof_free(p);
    }
    put(cat, CAT_HEAD CAT_TEST(PREFIX)
                 FAULT("e", EDIT("src/a.txt", "e", "E"), FAILS(1, "\"x\"", "false")) "," FAULT(
                     "gone", EDIT("src/b.txt", "count=9", "0"), FAILS(1, "\"x\"", "false")) "]}");
    fix_format(name, sizeof name, "%s.log", cat);
    f = fopen(name, "w");
    if (f != NULL && etude_proof_read(cat, SCHEMAS, f, &p) == 0) {
        CHECK(etude_proof_texts(p, tree, f) == 1, "texts: a text found twice or not at all fails");
        etude_proof_free(p);
    }
    if (f != NULL)
        fclose(f);
    get(name, log, sizeof log);
    CHECK(strstr(log, "fault e: edit 1 finds its text 2 times in src/a.txt") != NULL,
          "texts: a text found twice is named");
    CHECK(strstr(log, "fault gone: edit 1 finds its text 0 times in src/b.txt") != NULL,
          "texts: a text not found is named");
}

/** Count the objects of the store at `dir`. */
static int objects(const char *dir)
{
    char cmd[1200], buf[64];
    FILE *f;
    int n = -1;

    fix_format(cmd, sizeof cmd, "ls %s/objects | wc -l", dir);
    f = popen(cmd, "r");
    if (f != NULL && fgets(buf, sizeof buf, f) != NULL)
        n = atoi(buf);
    if (f != NULL)
        pclose(f);
    return n;
}

/** A sound proof passes, keeps its artifacts and leaves the tree as it was. */
static void test_sound(void)
{
    struct cat c = {"private", BUILD, TEST, 30, F_VALUE "," F_BOTH};
    char *one[] = {"both", NULL}, *none[] = {"none", NULL};
    char tree[1024], store[1100];
    struct outcome out;
    int before = fails;

    make_tree("sound", tree);
    prove("sound", tree, &c, NULL, 1, &out);
    fix_format(store, sizeof store, "%s/store", out.work);
    CHECK(out.status == 0, "sound: the proof passes");
    CHECK(strstr(out.log, "result: pass: toy: 2 of 2 faults found") != NULL,
          "sound: both faults are found");
    CHECK(objects(store) == 3, "sound: the base and the two faults made three distinct artifacts");
    CHECK(tree_intact(tree), "sound: the tree is as it was");
    show("sound", &out, before);
    before = fails;
    prove("one", tree, &c, one, 0, &out);
    CHECK(out.status == 0 && strstr(out.log, "result: pass: toy: 1 of 2 faults found") != NULL,
          "sound: a fault asked for alone is proved alone");
    CHECK(!exists(out.work), "sound: the work of a pass is removed");
    show("one", &out, before);
    prove("none", tree, &c, none, 0, &out);
    CHECK(out.status == 2 && strstr(out.log, "no fault none") != NULL,
          "sound: a fault the proof lacks is refused");
    before = fails;
    c.build = "for s in a b; do [ ${WORK}/art -nt ${SRC}/src/$s.txt ] || n=1; done; "
              "[ -z \"$n\" ] || { " BUILD "; perl -e '$t = time + 30; utime $t, $t, @ARGV' "
              "${WORK}/art; }";
    prove("stamps", tree, &c, NULL, 0, &out);
    CHECK(out.status == 0, "sound: a build that judges by time stamps ahead of the clock makes "
                           "its artifact again");
    show("stamps", &out, before);
}

/** The base phase run alone passes or fails as the unmodified test does,
 * and makes no fault's artifact. */
static void test_base(void)
{
    struct cat c = {"private", BUILD, TEST, 30, F_VALUE "," F_BOTH};
    char tree[1024], store[1100];
    struct outcome out;
    int before = fails;

    base_only = 1;
    make_tree("base", tree);
    prove("base", tree, &c, NULL, 1, &out);
    fix_format(store, sizeof store, "%s/store", out.work);
    CHECK(out.status == 0 &&
              strstr(out.log, "result: pass: toy: the base passed; no fault was run") != NULL,
          "base: the unmodified sources are built and tested alone");
    CHECK(objects(store) == 1, "base: no fault's artifact is made");
    CHECK(tree_intact(tree), "base: the tree is as it was");
    show("base", &out, before);
    before = fails;
    c.test = "echo 'toy-test: fail: quietly'; exit 1";
    make_tree("base-fails", tree);
    prove("base-fails", tree, &c, NULL, 0, &out);
    CHECK(out.status == 1 && strstr(out.log, "base: the test exited 1") != NULL,
          "base: a test that fails on the unmodified sources fails the run");
    show("base-fails", &out, before);
    base_only = 0;
}

/* A proof that must fail, and the words of its failure. */
struct refusal {
    const char *row;
    const char *build;
    const char *test;
    const char *faults;
    const char *why;
};

/** Each fault or phase that does not prove what it claims fails the proof. */
static void test_refused(void)
{
    static const struct refusal rows[] = {
        {"unreached", BUILD, TEST,
         FAULT("note", EDIT("src/a.txt", "# note", "# other"),
               FAILS(1, "\"toy-test: fail: value\"", "false")),
         "fault-note: the artifacts are the base's, so the fault did not reach them"},
        {"missed", BUILD, TEST,
         FAULT("extra", EDIT("src/b.txt", "count=2", "count=2\\nextra=1"),
               FAILS(1, "\"toy-test: fail: value\"", "false")),
         "fault-extra: the test passed with the fault"},
        {"status", BUILD, TEST,
         FAULT("value", EDIT("src/a.txt", "value=good", "value=bad"),
               FAILS(3, "\"toy-test: fail: value\"", "false")),
         "fault-value: the test exited 1, not 3"},
        {"line", BUILD, TEST,
         FAULT("value", EDIT("src/a.txt", "value=good", "value=bad"),
               FAILS(1, "\"toy-test: fail: count\"", "false")),
         "fault-value: the test's output has no line with: toy-test: fail: count"},
        {"only", BUILD, TEST,
         FAULT("both",
               EDIT("src/a.txt", "value=good", "value=bad") "," EDIT("src/b.txt", "count=2",
                                                                     "count=3"),
               FAILS(1, "\"toy-test: fail: value\"", "true")),
         "fault-both: the test failed a check the fault does not name: toy-test: fail: count"},
        {"found", BUILD, TEST,
         FAULT("e", EDIT("src/a.txt", "e", "E"), FAILS(1, "\"toy-test: fail: value\"", "false")),
         "fault-e: edit 1 finds its text 2 times in src/a.txt"},
        {"leftover", BUILD "; [ ! -e ${WORK}/left ] || cat ${WORK}/left >> ${WORK}/art",
         "grep -q bad ${SRC}/src/a.txt && echo stale > ${WORK}/left; " TEST, F_VALUE,
         "restored-value: the restored ${WORK}/art differs from the base's"},
        {"rewrite", BUILD "; echo more >> ${SRC}/src/b.txt", TEST, F_VALUE,
         "base: src/b.txt changed while the build and the test ran"},
        {"printed", BUILD, "echo 'toy-test: fail: quietly'", F_VALUE,
         "base: the test passed and printed a failure: toy-test: fail: quietly"},
    };
    struct cat c = {"private", NULL, NULL, 30, NULL};
    struct outcome out;
    char tree[1024];
    size_t i;
    int before;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        before = fails;
        make_tree(rows[i].row, tree);
        c.build = rows[i].build;
        c.test = rows[i].test;
        c.faults = rows[i].faults;
        prove(rows[i].row, tree, &c, NULL, 0, &out);
        CHECK(out.status == 1, rows[i].row);
        CHECK(strstr(out.log, rows[i].why) != NULL, rows[i].row);
        CHECK(exists(out.work) && strstr(out.log, "etude-proof: evidence: ") != NULL, rows[i].row);
        CHECK(tree_intact(tree), rows[i].row);
        show(rows[i].row, &out, before);
    }
}

/** A test that runs past its bound is ended, and fails the proof. */
static void test_bound(void)
{
    struct cat c = {"private", BUILD, NULL, 1, F_VALUE};
    char tree[1024], fifo[1024], test[1200];
    struct outcome out;
    int before = fails;

    make_tree("bound", tree);
    place(fifo, sizeof fifo, "bound-fifo");
    if (mkfifo(fifo, 0600) != 0) {
        CHECK(0, "bound: the FIFO is made");
        return;
    }
    fix_format(test, sizeof test, "exec cat %s", fifo);
    c.test = test;
    prove("bound", tree, &c, NULL, 0, &out);
    CHECK(out.status == 1 && strstr(out.log, "base: the test ran past its bound") != NULL,
          "bound: a test that hangs is ended at its bound");
    show("bound", &out, before);
}

/** Hold the run lock of `tree` for this process, as runlock_exec.sh does. */
static void hold(const char *tree)
{
    char path[1100], text[64];

    fix_format(path, sizeof path, "%s/build", tree);
    mkdir(path, 0700);
    fix_format(path, sizeof path, "%s/build/.run.lock", tree);
    mkdir(path, 0700);
    fix_format(path, sizeof path, "%s/build/.run.lock/owner", tree);
    fix_format(text, sizeof text, "pid %ld\nheld fault script\n", (long)getpid());
    put(path, text);
    fix_format(text, sizeof text, "%ld", (long)getpid());
    setenv("RACH2_TREE_HELD", text, 1);
}

/** A shared proof needs the run lock, faults the tree and restores it. */
static void test_shared(void)
{
    struct cat c = {"shared", BUILD, "echo seen: $(head -n 1 ${TREE}/src/a.txt); " TEST, 30,
                    F_VALUE};
    char tree[1024], path[1200], buf[256];
    struct outcome out;
    int before = fails;

    make_tree("shared", tree);
    unsetenv("RACH2_TREE_HELD");
    prove("unheld", tree, &c, NULL, 0, &out);
    CHECK(out.status == 2 && strstr(out.log, "the tree's run lock is not held") != NULL,
          "shared: a proof whose tree is not held is refused");
    show("unheld", &out, before);
    before = fails;
    hold(tree);
    prove("shared", tree, &c, NULL, 1, &out);
    CHECK(out.status == 0, "shared: the proof passes");
    fix_format(path, sizeof path, "%s/fault-value.test.out", out.work);
    CHECK(strstr(get(path, buf, sizeof buf), "seen: value=bad") != NULL,
          "shared: the fault is in the tree's own source");
    CHECK(tree_intact(tree), "shared: the tree is restored");
    show("shared", &out, before);
}

/* The signal that stopped the proof in the child, or 0. */
static volatile sig_atomic_t stopped;

/** Note the signal that stops the proof. */
static void on_term(int sig)
{
    stopped = sig;
}

/** A signal while a faulty test runs restores the tree and removes the work. */
static void test_stopped(void)
{
    struct cat c = {"shared", BUILD, NULL, 60, F_VALUE};
    struct etude_proof_opts o;
    struct etude_proof *p = NULL;
    struct sigaction sa;
    char tree[1024], ready[1024], held[1024], cat[1024], work[1024], test[2400], buf[16];
    int fd, status = -1;
    pid_t child;

    make_tree("stopped", tree);
    hold(tree);
    place(ready, sizeof ready, "stop-ready");
    place(held, sizeof held, "stop-hold");
    place(cat, sizeof cat, "cat-stopped.json");
    place(work, sizeof work, "work-stopped");
    if (mkfifo(ready, 0600) != 0 || mkfifo(held, 0600) != 0) {
        CHECK(0, "stopped: the FIFOs are made");
        return;
    }
    fix_format(test, sizeof test,
               "if grep -q bad ${SRC}/src/a.txt; then echo ready > %s; exec cat %s; fi; " TEST,
               ready, held);
    c.test = test;
    write_cat(cat, &c);
    if (etude_proof_read(cat, SCHEMAS, stdout, &p) != 0) {
        CHECK(0, "stopped: the catalogue is read");
        return;
    }
    fflush(stdout);
    child = fork();
    if (child == 0) {
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_term;
        sigaction(SIGTERM, &sa, NULL);
        memset(&o, 0, sizeof(o));
        o.tree = tree;
        o.work = work;
        o.log = stdout;
        o.stop = &stopped;
        _exit(etude_proof_run(p, &o));
    }
    etude_proof_free(p);
    fd = child > 0 ? open(ready, O_RDONLY) : -1;
    CHECK(fd >= 0 && read(fd, buf, sizeof buf) > 0, "stopped: the faulty test runs");
    if (fd >= 0)
        close(fd);
    if (child > 0) {
        kill(child, SIGTERM);
        waitpid(child, &status, 0);
    }
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == ETUDE_PROOF_STOPPED,
          "stopped: the proof says it was stopped");
    CHECK(tree_intact(tree), "stopped: the tree is restored");
    CHECK(!exists(work), "stopped: the work is removed");
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    test_read();
    test_edits();
    test_sound();
    test_base();
    test_refused();
    test_bound();
    test_shared();
    test_stopped();
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_proof_hosttest: pass\n");
    return 0;
}
