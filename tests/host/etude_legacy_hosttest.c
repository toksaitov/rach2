/**
 * Check etude's preparation adapter (tools/etude/posix/legacy.c) on fixture
 * trees, each a registry and a Makefile whose recipes append their goals to
 * the file `calls`, with the tree's run lock held by this program, which is
 * also the recipe of a preparation graph's node with `--legacy`.
 *
 * host-deps: tools/etude/posix/legacy.c tools/etude/posix/store.c
 * host-deps: tools/etude/posix/prepare.c tools/etude/capture.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-deps: tools/etude/posix/process.c utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude/posix
 * host-cflags: -Wall -Wextra -Werror
 * host-seconds: 180
 *
 * A suite alone has the recovery key minted, the components and then its own
 * artifacts built, its target's recipe never run, and its files recorded with
 * their digests, once each though make names one both from the tree's root and
 * from the file system's; a second preparation mints no key. A set has
 * `prepare-all` and then the artifacts outside it built, and an e1000 suite its
 * plain twin's. The architecture and the kernel reach every make. A suite
 * unknown, named twice or of another architecture, a tool missing, a set whose
 * preparation lacks a tool, an architecture or a kernel name refused and a tree
 * not held are each refused with nothing built. A failed step stops the
 * preparation and keeps its log; an artifact missing fails it; a step past its
 * bound and one under a stop signal are ended, and nothing of theirs runs
 * after. The record is checked by status and by digest, and a preparation graph
 * publishes it as a fresh node's product.
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
#include "etude/artifact.h"
#include "etude/legacy.h"
#include "sha256.h"

static int fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                          \
    do {                                                                                          \
        if (!(cond)) {                                                                            \
            printf("etude-legacy-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), #cond); \
            fails++;                                                                              \
        }                                                                                         \
    } while (0)

/* The room for a path: a fixture's are under a $TMPDIR of at most 100
 * characters (fixdir_host.h). */
#define PATH_LEN 1024

/* The bound of a wait for a recipe to start, in milliseconds. */
#define START_BOUND 60000

/* This program, by an absolute path. */
static char self[PATH_LEN];

/* The suites of the fixture trees. */
static const char registry[] = "# The suites of the fixture tree.\n"
                               "alpha|-|core\n"
                               "beta|-|full||256|x86_64\n"
                               "gamma|-|full\n"
                               "alpha-e1000|-|full\n"
                               "delta|-|full\n"
                               "hang|-|full\n"
                               "wait|-|full\n"
                               "needy|sh,etude-legacy-no-such-tool|full\n"
                               "sbinny|fixturetool+sh/etude-legacy-no-such-tool|full\n";

/* The fixture's Makefile. A target's own variable names a path, and a
 * multi-line variable holds a rule, and neither may become an artifact; an
 * artifact is also named from the root of the file system. */
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
    "define CANNED\n"
    "run-alpha: build/images/canned.img\n"
    "\techo canned\n"
    "endef\n"
    ".PHONY: components prepare-all prepare-images prepare-products prepare-tools \\\n"
    "        recovery-key ubsan-kernel tools/guest run-alpha run-gamma run-beta \\\n"
    "        run-needy run-sbinny run-delta run-hang run-wait\n"
    "components: $(ARCH_BUILD)/rach2/kernel\n"
    "\t@$(note)\n"
    "\t@test ! -f fail-components\n"
    "$(ARCH_BUILD)/rach2/kernel:\n"
    "\t@mkdir -p $(@D) && echo kernel > $@ && $(note)\n"
    "recovery-key:\n"
    "\t@echo key > krffs/recovery_authorized_keys && $(note)\n"
    "ubsan-kernel:\n"
    "\t@mkdir -p $(ARCH_BUILD)/rach2-ubsan && echo ubsan > $(ARCH_BUILD)/rach2-ubsan/kernel\n"
    "\t@$(note)\n"
    "prepare-all: components\n"
    "\t$(MAKE) --no-print-directory prepare-images prepare-products\n"
    "prepare-images: $(IMAGES)/a.img\n"
    "prepare-products: $(IMAGES)/b.img\n"
    "prepare-tools:\n"
    "\t@if [ -f no-tools ]; then echo sh,etude-legacy-no-such-tool; else echo sh; fi\n"
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
    "\t@echo shadow > $@ && $(note)\n"
    "tools/guest:\n"
    "\t@$(note)\n"
    "run-alpha: EXTRA := $(IMAGES)/bogus.img\n"
    "$(CURDIR)/$(IMAGES)/a.img: $(IMAGES)/a.img\n"
    "run-alpha: $(IMAGES)/a.img $(CURDIR)/$(IMAGES)/a.img krffs/shadow tools/guest src/in.c\n"
    "\t@$(note)\n"
    "run-gamma: $(IMAGES)/g.img $(IMAGES)/a.img\n"
    "\t@$(note)\n"
    "run-beta: $(IMAGES)/a.img\n"
    "\t@$(note)\n"
    "run-delta: $(IMAGES)/d.img\n"
    "\t@$(note)\n"
    "run-hang: $(IMAGES)/h.img\n"
    "\t@$(note)\n"
    "run-wait: $(IMAGES)/w.img\n"
    "\t@$(note)\n"
    "run-needy run-sbinny:\n"
    "\t@$(note)\n";

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
    static const char *const dirs[] = {"",      "tests",           "krffs", "krffs/manifests",
                                       "build", "build/.run.lock", "src",   "sbin",
                                       "tools", "tools/setup"};
    char path[PATH_LEN], owner[64];
    size_t i;
    int bad = 0;

    fix_format(tree, size, "%s/%s", fix_dir(), name);
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        bad |= mkdir(in(path, sizeof(path), tree, dirs[i]), 0755) != 0;
    snprintf(owner, sizeof(owner), "pid %ld\nheld test\n", (long)getpid());
    bad |= write_text(in(path, sizeof(path), tree, "tests/registry"), registry, 0644);
    bad |= write_text(in(path, sizeof(path), tree, "Makefile"), makefile, 0644);
    bad |= write_text(in(path, sizeof(path), tree, "krffs/manifests/rach2-recovery.manifest"), "",
                      0644);
    bad |= write_text(in(path, sizeof(path), tree, "krffs/rach2_demo_ed25519"), "key\n", 0644);
    bad |= write_text(in(path, sizeof(path), tree, "src/in.c"), "src\n", 0644);
    bad |= write_text(in(path, sizeof(path), tree, "sbin/fixturetool"), "#!/bin/sh\n", 0755);
    bad |= write_text(in(path, sizeof(path), tree, "tools/setup/e2fsprogs_path.sh"),
                      "PATH=\"$PATH:$(pwd)/sbin\"\nexport PATH\n", 0644);
    bad |= write_text(in(path, sizeof(path), tree, "build/.run.lock/owner"), owner, 0644);
    bad |= mkfifo(in(path, sizeof(path), tree, "hang.fifo"), 0644) != 0;
    bad |= mkfifo(in(path, sizeof(path), tree, "started.fifo"), 0644) != 0;
    if (bad) {
        printf("etude-legacy-test: fail: the fixture tree %s was not made\n", tree);
        exit(1);
    }
}

/* ---- Preparations. ---- */

/** Fill `o` for the suites `suites` of `tree` on i686, with the work
 * directory and the record named after `row` in the fixture directory. */
static void opts_for(struct etude_legacy_opts *o, const char *tree, const char *row,
                     char *const *suites)
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
    o->suites = suites;
    while (suites[o->nsuites] != NULL)
        o->nsuites++;
}

/** Run the preparation `o` and take what it printed into `r`. */
static void run(struct etude_legacy_opts *o, struct outcome *r)
{
    FILE *f = tmpfile();
    size_t n;

    r->out[0] = '\0';
    if (f == NULL) {
        printf("etude-legacy-test: fail: no file for the output: %s\n", strerror(errno));
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
        printf("etude-legacy-test: its output:\n%s", r->out);
}

/* ---- Rows. ---- */

/** One suite alone: the steps in order, the target unrun, the record. */
static void test_alone(void)
{
    char *suites[] = {"alpha", NULL};
    char tree[PATH_LEN], path[PATH_LEN], calls[4096], record[8192], files[1024], want[128], hex[65];
    struct etude_legacy_opts o;
    struct outcome r;
    struct stat st;
    int before = fails;

    make_tree("alone", tree, sizeof(tree));
    opts_for(&o, tree, "alone", suites);
    run(&o, &r);
    read_text(in(path, sizeof(path), tree, "calls"), calls, sizeof(calls));
    read_text(o.record, record, sizeof(record));
    record_files(record, files, sizeof(files));
    CHECK(r.status == 0, "alone: prepared");
    CHECK(has_line(r.out, "etude-legacy: result: prepared: 1 suites, 3 files"), "alone: result");
    CHECK(line_at(calls, "recovery-key i686 build/rach2/kernel C") == 0, "alone: key first");
    CHECK(line_at(calls, "components i686 build/rach2/kernel C") >
              line_at(calls, "build/rach2/kernel i686 build/rach2/kernel C"),
          "alone: components");
    CHECK(line_at(calls, "build/images/a.img i686 build/rach2/kernel C") >
              line_at(calls, "components i686 build/rach2/kernel C"),
          "alone: artifacts after the components");
    CHECK(has_line(calls, "krffs/shadow i686 build/rach2/kernel C"), "alone: a tree's target");
    CHECK(strstr(calls, "run-alpha") == NULL && strstr(calls, "tools/guest") == NULL,
          "alone: no phony target run");
    CHECK(strstr(calls, "b.img") == NULL && strstr(calls, "bogus") == NULL &&
              strstr(calls, "canned") == NULL,
          "alone: nothing else built");
    CHECK(strcmp(files, "build/images/a.img\nbuild/rach2/kernel\nkrffs/shadow\n") == 0,
          "alone: the record's files");
    sha_text("src\n", hex);
    fix_format(want, sizeof(want), "file %s 4 ", hex);
    CHECK(strstr(record, want) != NULL, "alone: a file's digest and size");
    CHECK(has_line(record, "suite alpha run-alpha") && has_line(record, "arch i686"),
          "alone: the record's head");
    CHECK(exists(in(path, sizeof(path), tree, "krffs/recovery_authorized_keys")),
          "alone: the key minted");
    CHECK(stat(in(path, sizeof(path), tree, "krffs/rach2_demo_ed25519"), &st) == 0 &&
              (st.st_mode & 0777) == 0600,
          "alone: the demo key's mode");
    CHECK(!exists(o.work), "alone: the work removed");
    show(&r, before);
}

/** The same suite again: no key minted, nothing rebuilt. */
static void test_again(void)
{
    char *suites[] = {"alpha", NULL};
    char tree[PATH_LEN], path[PATH_LEN], calls[4096];
    struct etude_legacy_opts o;
    struct outcome r;
    int before = fails;

    fix_format(tree, sizeof(tree), "%s/alone", fix_dir());
    unlink(in(path, sizeof(path), tree, "calls"));
    opts_for(&o, tree, "again", suites);
    run(&o, &r);
    read_text(in(path, sizeof(path), tree, "calls"), calls, sizeof(calls));
    CHECK(r.status == 0, "again: prepared");
    CHECK(strcmp(calls, "components i686 build/rach2/kernel C\n") == 0,
          "again: only the phony components");
    CHECK(strstr(r.out, "step: recovery-key") == NULL, "again: no key minted");
    show(&r, before);
}

/** A set: prepare-all, then the artifacts outside it; an e1000 twin. */
static void test_set(void)
{
    char *suites[] = {"alpha", "gamma", "alpha-e1000", NULL};
    char tree[PATH_LEN], path[PATH_LEN], calls[4096], record[8192], files[1024];
    struct etude_legacy_opts o;
    struct outcome r;
    int before = fails;

    make_tree("set", tree, sizeof(tree));
    opts_for(&o, tree, "set", suites);
    run(&o, &r);
    read_text(in(path, sizeof(path), tree, "calls"), calls, sizeof(calls));
    read_text(o.record, record, sizeof(record));
    record_files(record, files, sizeof(files));
    CHECK(r.status == 0, "set: prepared");
    CHECK(has_line(r.out, "etude-legacy: step: prepare-all"), "set: prepare-all");
    CHECK(line_at(calls, "build/images/g.img i686 build/rach2/kernel C") >
              line_at(calls, "build/images/b.img i686 build/rach2/kernel C"),
          "set: the artifacts outside prepare-all after it");
    CHECK(has_line(calls, "build/images/a.img i686 build/rach2/kernel C"), "set: prepare-all's");
    CHECK(strcmp(files, "build/images/a.img\nbuild/images/g.img\nbuild/rach2/kernel\n"
                        "krffs/shadow\n") == 0,
          "set: the record's files");
    CHECK(has_line(record, "suite alpha-e1000 run-alpha"), "set: the e1000 twin's target");
    show(&r, before);
}

/** x86_64 and a kernel of its own reach every make and the record. */
static void test_machine(void)
{
    char *suites[] = {"beta", NULL};
    char tree[PATH_LEN], path[PATH_LEN], calls[4096], record[8192], files[1024];
    const char *k = "build/x86_64/rach2-ubsan/kernel";
    struct etude_legacy_opts o;
    struct outcome r;
    int before = fails;

    make_tree("machine", tree, sizeof(tree));
    opts_for(&o, tree, "machine", suites);
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
          "machine: the artifacts");
    CHECK(strcmp(files, "build/x86_64/images/a.img\nbuild/x86_64/rach2-ubsan/kernel\n") == 0,
          "machine: the record's files");
    fix_format(path, sizeof(path), "kernel %s", k);
    CHECK(has_line(record, path) && has_line(record, "arch x86_64"), "machine: the record's head");
    show(&r, before);
}

/**
 * Prepare `suites` of tree `tree` as row `row`, with `arch` and `kernel`,
 * and require a refusal with `why` and nothing built or left.
 */
static void refused(const char *tree, const char *row, char *const *suites, const char *arch,
                    const char *kernel, const char *why)
{
    char path[PATH_LEN], line[256];
    struct etude_legacy_opts o;
    struct outcome r;
    int before = fails;

    opts_for(&o, tree, row, suites);
    o.arch = arch;
    o.kernel = kernel;
    run(&o, &r);
    fix_format(line, sizeof(line), "etude-legacy: result: refused: %s", why);
    CHECK(r.status == 2, row);
    CHECK(has_line(r.out, line), row);
    CHECK(!exists(in(path, sizeof(path), tree, "calls")), row);
    CHECK(!exists(o.work) && !exists(o.record), row);
    show(&r, before);
}

/** Each refusal builds nothing; a tool on the path that e2fsprogs_path.sh
 * adds is found. */
static void test_refused(void)
{
    char *unknown[] = {"alpha", "nosuch", NULL}, *twice[] = {"alpha", "gamma", "alpha", NULL};
    char *beta[] = {"beta", NULL}, *needy[] = {"needy", NULL}, *alpha[] = {"alpha", NULL};
    char *set[] = {"alpha", "gamma", NULL}, *sbinny[] = {"sbinny", NULL};
    char tree[PATH_LEN], path[PATH_LEN], held[32];
    struct etude_legacy_opts o;
    struct outcome r;
    int before;

    make_tree("refused", tree, sizeof(tree));
    refused(tree, "unknown", unknown, "i686", NULL, "unknown suite 'nosuch'");
    refused(tree, "twice", twice, "i686", NULL, "alpha is named twice");
    refused(tree, "arch", beta, "i686", NULL,
            "beta runs on x86_64 alone, and the architecture is i686");
    refused(tree, "needy", needy, "i686", NULL,
            "needy needs etude-legacy-no-such-tool, which this host does not have");
    refused(tree, "arm", alpha, "arm", NULL, "arm: the architectures are i686 and x86_64");
    refused(tree, "kernel", alpha, "i686", "../x",
            "../x: a kernel's name is letters, digits, '_' and '-'");
    write_text(in(path, sizeof(path), tree, "no-tools"), "", 0644);
    refused(tree, "tools", set, "i686", NULL,
            "a set prepares every suite's artifacts, and needs etude-legacy-no-such-tool, which "
            "this host does not have");
    unlink(path);
    snprintf(held, sizeof(held), "%ld", (long)getppid());
    setenv("RACH2_TREE_HELD", held, 1);
    refused(tree, "held", alpha, "i686", NULL,
            "the tree is not held: RACH2_TREE_HELD must name the running process that "
            "build/.run.lock/owner names");
    snprintf(held, sizeof(held), "%ld", (long)getpid());
    setenv("RACH2_TREE_HELD", held, 1);
    before = fails;
    opts_for(&o, tree, "sbinny", sbinny);
    run(&o, &r);
    CHECK(r.status == 0, "sbinny: a tool on the path e2fsprogs_path.sh adds");
    show(&r, before);
}

/** A failed step stops the preparation and keeps its log. */
static void test_failed(void)
{
    char *suites[] = {"alpha", NULL};
    char tree[PATH_LEN], path[PATH_LEN], calls[4096], log[4096], line[PATH_LEN + 64], *work;
    struct etude_legacy_opts o;
    struct outcome r;
    int before = fails;

    make_tree("failed", tree, sizeof(tree));
    write_text(in(path, sizeof(path), tree, "fail-components"), "", 0644);
    opts_for(&o, tree, "failed", suites);
    run(&o, &r);
    read_text(in(path, sizeof(path), tree, "calls"), calls, sizeof(calls));
    fix_format(path, sizeof(path), "%s/3-components.log", o.work);
    read_text(path, log, sizeof(log));
    CHECK(r.status == 1, "failed: failed");
    CHECK(has_line(r.out, "etude-legacy: result: failed: components: it exited 2"),
          "failed: the result names the step");
    work = realpath(o.work, NULL);
    fix_format(line, sizeof(line), "etude-legacy: evidence: %s", work != NULL ? work : o.work);
    free(work);
    CHECK(has_line(r.out, line) && strstr(log, "components") != NULL, "failed: its log kept");
    CHECK(strstr(r.out, "step: database") == NULL && strstr(calls, "a.img") == NULL,
          "failed: no step after it");
    CHECK(!exists(o.record), "failed: no record");
    show(&r, before);
}

/** An artifact that its rule does not make fails the preparation. */
static void test_missing(void)
{
    char *suites[] = {"delta", NULL};
    char tree[PATH_LEN];
    struct etude_legacy_opts o;
    struct outcome r;
    int before = fails;

    make_tree("missing", tree, sizeof(tree));
    opts_for(&o, tree, "missing", suites);
    run(&o, &r);
    CHECK(r.status == 1, "missing: failed");
    CHECK(has_line(r.out, "etude-legacy: missing: build/images/d.img: No such file or directory"),
          "missing: the artifact named");
    CHECK(has_line(r.out, "etude-legacy: result: failed: 1 of 2 artifacts are missing"),
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
    char *suites[] = {"hang", NULL};
    char tree[PATH_LEN], path[PATH_LEN];
    struct etude_legacy_opts o;
    struct outcome r;
    int before = fails;

    make_tree("bound", tree, sizeof(tree));
    opts_for(&o, tree, "bound", suites);
    o.seconds = 1;
    run(&o, &r);
    CHECK(r.status == 1, "bound: failed");
    CHECK(has_line(r.out, "etude-legacy: result: failed: artifacts: it ran past its bound of 1 s"),
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

/** A stop signal ends the step that runs and removes the work. */
static void test_stop(void)
{
    char *suites[] = {"wait", NULL};
    char tree[PATH_LEN], path[PATH_LEN], got[16];
    const char *out = fix_path("stop.out");
    struct etude_legacy_opts o;
    struct outcome r;
    struct pollfd pfd;
    int status = -1, fd, before = fails;
    pid_t pid;

    make_tree("stop", tree, sizeof(tree));
    opts_for(&o, tree, "stop", suites);
    o.seconds = 30;
    fd = open(in(path, sizeof(path), tree, "started.fifo"), O_RDWR);
    fflush(stdout);
    pid = start_child(&o, out);
    pfd.fd = fd;
    pfd.events = POLLIN;
    CHECK(fd >= 0 && pid > 0 && poll(&pfd, 1, START_BOUND) == 1 && read(fd, got, sizeof(got)) > 0,
          "stop: the step started");
    if (pid > 0) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
    }
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGTERM, "stop: stopped");
    CHECK(no_reader(in(path, sizeof(path), tree, "hang.fifo")), "stop: its recipe ended");
    CHECK(!exists(o.work) && !exists(o.record), "stop: the work removed");
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
              has_line(out, "etude-legacy: result: unchanged: 3 files"),
          "check: unchanged");
    overwrite(img, "SRC\n");
    CHECK(check(record, 0, out, sizeof(out)) == 0, "check: same size and time pass by status");
    CHECK(check(record, 1, out, sizeof(out)) == 1 &&
              has_line(out, "etude-legacy: changed: build/images/a.img"),
          "check: a changed content found by digest");
    fix_format(tmp, sizeof(tmp), "%s.new", img);
    write_text(tmp, "src\n", 0644);
    rename(tmp, img);
    CHECK(check(record, 0, out, sizeof(out)) == 1 &&
              has_line(out, "etude-legacy: result: changed: 1 of 3 files"),
          "check: a new file found by status");
    CHECK(check(record, 1, out, sizeof(out)) == 0, "check: the same content passes by digest");
    unlink(in(tmp, sizeof(tmp), tree, "krffs/shadow"));
    CHECK(check(record, 1, out, sizeof(out)) == 1 &&
              has_line(out, "etude-legacy: missing: krffs/shadow: No such file or directory"),
          "check: a missing file");
    fix_format(tmp, sizeof(tmp), "%s.bad", record);
    write_text(tmp, "etude-legacy 1\ntree /\nfile 00 1 2 3 x\n", 0644);
    CHECK(check(tmp, 0, out, sizeof(out)) == 2, "check: a malformed record");
    write_text(tmp, "etude-legacy 2\n", 0644);
    CHECK(check(tmp, 0, out, sizeof(out)) == 2, "check: another record");
}

/** Run as a graph's recipe: prepare ARGV[4...] of tree ARGV[2] into the
 * record ARGV[3]. */
static int recipe(int argc, char **argv)
{
    struct etude_legacy_opts o;

    memset(&o, 0, sizeof(o));
    o.tree = argv[2];
    o.arch = "i686";
    o.record = argv[3];
    o.seconds = 60;
    o.suites = argv + 4;
    o.nsuites = (size_t)(argc - 4);
    o.out = stdout;
    return etude_legacy_prepare(&o);
}

/** A preparation graph publishes the record as a fresh node's product. */
static void test_graph(void)
{
    char tree[PATH_LEN], dir[PATH_LEN], obj[PATH_LEN], text[8192];
    char *argv[] = {self, "--legacy", tree, "${WORK}/record", "alpha", NULL};
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
              strncmp(text, "etude-legacy 1\n", 15) == 0 && has_line(text, "suite alpha run-alpha"),
          "graph: the record published");
    if (res != NULL && res->state != ETUDE_PREP_BUILT)
        printf("etude-legacy-test: the recipe's errors: %s\n", res->why);
    etude_exec_free(ex);
    etude_prep_free(g);
    if (s != NULL)
        etude_store_close(s);
}

int main(int argc, char **argv)
{
    char held[32];

    if (argc >= 5 && strcmp(argv[1], "--legacy") == 0)
        return recipe(argc, argv);
    if (realpath(argv[0], self) == NULL) {
        printf("etude-legacy-test: fail: %s cannot be found\n", argv[0]);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    snprintf(held, sizeof(held), "%ld", (long)getpid());
    setenv("RACH2_TREE_HELD", held, 1);
    setenv("LC_ALL", "POSIX", 1);
    setenv("MAKEFLAGS", "n", 1);
    test_alone();
    test_again();
    test_set();
    test_machine();
    test_refused();
    test_failed();
    test_missing();
    test_bound();
    test_stop();
    test_check();
    test_graph();
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_legacy_hosttest: pass\n");
    return 0;
}
