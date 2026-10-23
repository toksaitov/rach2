/**
 * Check etude-step: the key of a step and what changes it, a copy-mode step
 * stored by one copy and taken by another, a link-mode step built in the
 * store with the links of its uses, the build without the store, the key a
 * command is given, the steps that are not shared, a failed and an
 * unfinished build, ccache's names, two builds of one key at once, the
 * removal of entries nobody uses, and the refusals. Each row runs the
 * program's main in a child of the test, in copies and a store of its own.
 * host-deps: utils/sha256.c
 * host-cflags: -I utils -Wall -Wextra -Werror
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "compat/fixdir_host.c"

#define main step_main
#include "../../tools/etude/step/etude_step.c"
#undef main

static int fails;

#define CHECK(c, row)                                                    \
    do {                                                                 \
        if (!(c)) {                                                      \
            fprintf(stderr, "etude-step-test: fail: %s: %s\n", row, #c); \
            fails++;                                                     \
        }                                                                \
    } while (0)

/* What one run of the program gave. */
struct ran {
    int status;      /* as a shell gives it */
    char out[16384]; /* its standard output and error */
};

/* The store of the test, and its two copies of a tree. */
static char store[PATH_LEN], copy_a[PATH_LEN], copy_b[PATH_LEN];

/** Give the text of `path`, or "" when it cannot be read, in `buf`. */
static char *get(const char *path, char *buf, size_t size)
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

/** Write `text` to `path`, making its directories. */
static void put(const char *path, const char *text)
{
    FILE *f;

    make_parents(path);
    f = fopen(path, "w");
    if (f == NULL || fputs(text, f) == EOF || fclose(f) != 0) {
        fprintf(stderr, "etude-step-test: cannot write %s\n", path);
        exit(1);
    }
}

static int exists(const char *path)
{
    struct stat st;

    return lstat(path, &st) == 0;
}

static int is_link(const char *path)
{
    struct stat st;

    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

/** The text of the file `name` in the copy `copy`. */
static char *in_copy(const char *copy, const char *name, char *buf, size_t size)
{
    char p[1200];

    fix_format(p, sizeof p, "%s/%s", copy, name);
    return get(p, buf, size);
}

/** Start the program's main in a child, in `dir`, with the arguments
 * `argv` and the environment the caller set, its output into `out`. */
static pid_t start(const char *dir, char **argv, const char *out)
{
    pid_t pid = fork();

    if (pid == 0) {
        int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        int argc = 0;

        if (fd < 0 || chdir(dir) != 0)
            _exit(125);
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
        while (argv[argc])
            argc++;
        exit(step_main(argc, argv));
    }
    return pid;
}

/** Run the program in `dir` and wait for it. */
static void run(const char *dir, char **argv, struct ran *r)
{
    const char *out = fix_path("step.out");
    pid_t pid = start(dir, argv, out);

    while (waitpid(pid, &r->status, 0) < 0 && errno == EINTR)
        ;
    r->status = WIFEXITED(r->status) ? WEXITSTATUS(r->status) : 128 + WTERMSIG(r->status);
    get(out, r->out, sizeof r->out);
}

/** Make an empty copy of a tree, `name` in the fixture directory, with an
 * input file `src/in.txt` that says `text`. */
static void make_copy(const char *name, char *copy, size_t size, const char *text)
{
    char p[1200];

    fix_format(p, sizeof p, "%s/%s", fix_dir(), name);
    mkdir(p, 0700);
    if (realpath(p, copy) == NULL || strlen(copy) >= size) {
        fprintf(stderr, "etude-step-test: cannot make %s\n", p);
        exit(1);
    }
    fix_format(p, sizeof p, "%s/src/in.txt", copy);
    put(p, text);
}

/** The key that `-p` gives for the step of `argv` in `dir`, in `key`. */
static void key_of(const char *dir, char **argv, char key[65])
{
    struct ran r;
    char *k;

    run(dir, argv, &r);
    k = strstr(r.out, "\nkey ");
    key[0] = '\0';
    if (r.status == 0 && k != NULL)
        snprintf(key, 65, "%.64s", k + 5);
}

/** The key, its record and what changes them. */
static void test_key(void)
{
    char k1[65], k2[65], k3[65], k4[65], k5[65], k6[65], p[1200];
    char *one[] = {"etude-step", "-p",  "-n", "demo",       "-t", "st/demo.stamp", "-a", "i686",
                   "-i",         "src", "-i", "src/in.txt", "-o", "out",           NULL};
    char *order[] = {"etude-step", "-p",         "-n", "demo", "-t", "st/demo.stamp", "-a", "i686",
                     "-i",         "src/in.txt", "-i", "src",  "-o", "out",           NULL};
    char *arch[] = {"etude-step", "-p",  "-n", "demo", "-t", "st/demo.stamp", "-a", "x86_64",
                    "-i",         "src", "-o", "out",  NULL};
    char *outs[] = {"etude-step", "-p",  "-n", "demo", "-t", "st/demo.stamp", "-a", "i686",
                    "-i",         "src", "-o", "out",  "-o", "more",          NULL};
    char *dep[] = {"etude-step", "-p",  "-n", "demo",     "-t", "st/demo.stamp", "-a", "i686",
                   "-i",         "src", "-d", "up.stamp", "-o", "out",           NULL};
    struct ran r;

    key_of(copy_a, one, k1);
    key_of(copy_a, order, k2);
    key_of(copy_b, one, k3);
    CHECK(strlen(k1) == 64 && strcmp(k1, k2) == 0, "key: the order of the inputs does not count");
    CHECK(strcmp(k1, k3) != 0, "key: another input file gives another key");
    key_of(copy_a, arch, k4);
    CHECK(k4[0] && strcmp(k1, k4) != 0, "key: another architecture gives another key");
    key_of(copy_a, outs, k5);
    CHECK(k5[0] && strcmp(k1, k5) != 0, "key: another output gives another key");

    fix_format(p, sizeof p, "%s/up.stamp", copy_a);
    put(p, "etude-step 1 1111111111111111111111111111111111111111111111111111111111111111 here\n");
    key_of(copy_a, dep, k5);
    put(p, "etude-step 1 1111111111111111111111111111111111111111111111111111111111111111 store\n");
    key_of(copy_a, dep, k6);
    CHECK(k5[0] && strcmp(k5, k6) == 0,
          "key: a stamp counts by its key, not where its outputs came from");
    put(p, "etude-step 1 2222222222222222222222222222222222222222222222222222222222222222 here\n");
    key_of(copy_a, dep, k6);
    CHECK(k6[0] && strcmp(k5, k6) != 0, "key: a stamp with another key gives another key");
    run(copy_a, dep, &r);
    CHECK(strstr(r.out, "depends 2222222222222222222222222222222222222222222222222222222222222222 "
                        "up.stamp\n") != NULL &&
              strstr(r.out, "input ") != NULL && strstr(r.out, " src/in.txt\n") != NULL &&
              strstr(r.out, "mode copy\n") != NULL,
          "key: the record names the inputs and the stamps it uses");
    unlink(p);
}

/** A copy-mode step that copy a builds and stores, and copy b takes. */
static void test_copy(void)
{
    char buf[512], p[1200], want[1200];
    char *argv[] = {"etude-step",
                    "-s",
                    store,
                    "-n",
                    "demo",
                    "-t",
                    "st/demo.stamp",
                    "-a",
                    "i686",
                    "-i",
                    "src",
                    "-o",
                    "out",
                    "-o",
                    "flag",
                    "-x",
                    "*.o",
                    "--",
                    "sh",
                    "-c",
                    "mkdir -p out/sub && echo built >> ran && echo object > out/sub/a.o && "
                    "echo header > out/sub/a.h && echo yes > flag",
                    NULL};
    struct ran r;
    struct stat st;

    fix_format(p, sizeof p, "%s/src/in.txt", copy_a);
    put(p, "same\n");
    fix_format(p, sizeof p, "%s/src/in.txt", copy_b);
    put(p, "same\n");
    run(copy_a, argv, &r);
    CHECK(r.status == 0 && strstr(r.out, "result: built and stored") != NULL,
          "copy: the first build is stored");
    CHECK(strncmp(in_copy(copy_a, "st/demo.stamp", buf, sizeof buf), "etude-step 1 ", 13) == 0 &&
              strstr(buf, " here\n") != NULL,
          "copy: the stamp says the outputs were built here");
    run(copy_b, argv, &r);
    CHECK(r.status == 0 && strstr(r.out, "result: taken from the store") != NULL,
          "copy: the second copy takes the outputs");
    CHECK(strstr(in_copy(copy_b, "st/demo.stamp", buf, sizeof buf), " store\n") != NULL,
          "copy: the stamp says the outputs came from the store");
    CHECK(!exists(fix_path("copy-b/ran")), "copy: the second copy runs no command");
    CHECK(strcmp(in_copy(copy_b, "out/sub/a.h", buf, sizeof buf), "header\n") == 0 &&
              strcmp(in_copy(copy_b, "flag", buf, sizeof buf), "yes\n") == 0,
          "copy: the outputs are the first copy's");
    CHECK(!exists(fix_path("copy-b/out/sub/a.o")), "copy: a name of -x is not stored");
    fix_format(p, sizeof p, "%s/out/sub/a.h", copy_b);
    CHECK(stat(p, &st) == 0 && (st.st_mode & S_IWUSR) && !is_link(p),
          "copy: a taken output is a writable copy");
    fix_format(want, sizeof want, "%s/entries", store);
    CHECK(exists(want), "copy: the store has its entries");
    run(copy_b, argv, &r);
    CHECK(r.status == 0 && strstr(r.out, "taken from the store") != NULL,
          "copy: taking the outputs again replaces those in the copy");
}

/** The store's entry of the key the stamp `stamp` of `copy` gives, and the
 * path `rel` in its tree. */
static void entry_path(const char *copy, const char *stamp, const char *rel, char *out, size_t size)
{
    char buf[256], key[65];

    in_copy(copy, stamp, buf, sizeof buf);
    if (sscanf(buf, "etude-step 1 %64s", key) != 1)
        key[0] = '\0';
    fix_format(out, size, "%s/entries/%s/tree/%s", store, key, rel);
}

/** Link-mode steps: a toolchain and the ports that use it. */
static void test_link(void)
{
    char buf[PATH_LEN], p[1200], q[1200];
    char *tc[] = {"etude-step",
                  "-s",
                  store,
                  "-n",
                  "tc",
                  "-t",
                  "r/tc.stamp",
                  "-r",
                  "r",
                  "-i",
                  "src",
                  "-o",
                  "cross",
                  "--",
                  "sh",
                  "-c",
                  "mkdir -p \"$ETUDE_STEP_ROOT/cross\" \"$ETUDE_STEP_ROOT/work/x\" && "
                  "echo \"$ETUDE_STEP_ROOT\" > \"$ETUDE_STEP_ROOT/cross/where\" && "
                  "echo built >> ../ran-tc",
                  NULL};
    char *ports[] = {"etude-step",
                     "-s",
                     store,
                     "-n",
                     "ports",
                     "-t",
                     "r/ports.stamp",
                     "-r",
                     "r",
                     "-d",
                     "r/tc.stamp",
                     "-u",
                     "cross",
                     "-o",
                     "root",
                     "-o",
                     "work/keep",
                     "--",
                     "sh",
                     "-c",
                     "mkdir -p \"$ETUDE_STEP_ROOT/root\" \"$ETUDE_STEP_ROOT/work/keep\" "
                     "\"$ETUDE_STEP_ROOT/work/drop\" && "
                     "cat \"$ETUDE_STEP_ROOT/cross/where\" > \"$ETUDE_STEP_ROOT/root/w\"",
                     NULL};
    struct ran r;
    struct stat st;

    run(copy_a, tc, &r);
    CHECK(r.status == 0 && strstr(r.out, "result: built in the store") != NULL,
          "link: the first build is in the store");
    fix_format(p, sizeof p, "%s/r/cross", copy_a);
    entry_path(copy_a, "r/tc.stamp", "cross", q, sizeof q);
    CHECK(is_link(p) && realpath(p, buf) != NULL && strcmp(buf, q) == 0,
          "link: the copy's output is a link to the entry");
    entry_path(copy_a, "r/tc.stamp", "", q, sizeof q);
    q[strlen(q) - 1] = '\0';
    CHECK(strcmp(in_copy(copy_a, "r/cross/where", buf, sizeof buf), "") != 0 &&
              strncmp(buf, q, strlen(q)) == 0,
          "link: the command ran at the entry's own path");
    fix_format(p, sizeof p, "%s/work", q);
    CHECK(!exists(p), "link: what is not an output is removed from the entry");
    fix_format(p, sizeof p, "%s/cross/where", q);
    CHECK(stat(p, &st) == 0 && (st.st_mode & 0222) == 0, "link: the entry is read-only");

    run(copy_b, tc, &r);
    CHECK(r.status == 0 && strstr(r.out, "taken from the store") != NULL &&
              strcmp(get(fix_path("ran-tc"), buf, sizeof buf), "built\n") == 0,
          "link: the second copy takes the links and runs no command");
    CHECK(strcmp(in_copy(copy_b, "r/cross/where", buf, sizeof buf), "") != 0,
          "link: the second copy reads the entry");

    run(copy_a, ports, &r);
    CHECK(r.status == 0 && strstr(r.out, "built in the store") != NULL,
          "link: a step that uses an entry is built in the store");
    CHECK(strcmp(in_copy(copy_a, "r/root/w", buf, sizeof buf), "") != 0,
          "link: the use is linked into the step's tree");
    entry_path(copy_a, "r/ports.stamp", "work/drop", p, sizeof p);
    entry_path(copy_a, "r/ports.stamp", "work/keep", q, sizeof q);
    CHECK(!exists(p) && exists(q), "link: an output below a directory is kept and the rest goes");
    fix_format(p, sizeof p, "%s/r/work/keep", copy_a);
    CHECK(is_link(p), "link: an output below a directory is linked in the copy");
}

/** The steps that run in the copy: without the store, with a stamp of no
 * key, with a use built here, and an output that names the copy. */
static void test_here(void)
{
    char buf[1200], p[1200];
    char *off[] = {
        "etude-step",
        "-s",
        "",
        "-n",
        "tc",
        "-t",
        "r/tc.stamp",
        "-r",
        "r",
        "-i",
        "src",
        "-o",
        "cross",
        "--",
        "sh",
        "-c",
        "mkdir -p \"$ETUDE_STEP_ROOT/cross\" && echo here > \"$ETUDE_STEP_ROOT/cross/where\"",
        NULL};
    char *ports[] = {
        "etude-step",
        "-s",
        store,
        "-n",
        "ports",
        "-t",
        "r/ports.stamp",
        "-r",
        "r",
        "-d",
        "r/tc.stamp",
        "-u",
        "cross",
        "-o",
        "root",
        "--",
        "sh",
        "-c",
        "mkdir -p \"$ETUDE_STEP_ROOT/root\" && echo local > \"$ETUDE_STEP_ROOT/root/w\"",
        NULL};
    char *nokey[] = {"etude-step", "-s", store, "-n", "nokey", "-t", "st/nokey.stamp", "-d",
                     "old.stamp",  "-o", "nk",  "--", "sh",    "-c", "echo x > nk",    NULL};
    char *leak[] = {"etude-step",
                    "-s",
                    store,
                    "-n",
                    "leak",
                    "-t",
                    "st/leak.stamp",
                    "-i",
                    "src",
                    "-o",
                    "lk",
                    "--",
                    "sh",
                    "-c",
                    "mkdir lk && echo \"$PWD/vendor\" > lk/p",
                    NULL};
    struct ran r;

    run(copy_a, off, &r);
    fix_format(p, sizeof p, "%s/r/cross", copy_a);
    CHECK(r.status == 0 && strstr(r.out, "result: built here") != NULL && !is_link(p) &&
              strcmp(in_copy(copy_a, "r/cross/where", buf, sizeof buf), "here\n") == 0,
          "here: without the store the link to it is replaced by a build in the copy");
    CHECK(strstr(in_copy(copy_a, "r/tc.stamp", buf, sizeof buf), " here\n") != NULL,
          "here: the stamp says here");
    run(copy_a, ports, &r);
    CHECK(r.status == 0 &&
              strstr(r.out, "was built in this copy; built without the store") != NULL &&
              strcmp(in_copy(copy_a, "r/root/w", buf, sizeof buf), "local\n") == 0,
          "here: a step whose use was built in the copy is not shared");

    put(fix_path("copy-a/old.stamp"), "");
    run(copy_a, nokey, &r);
    CHECK(r.status == 0 && strstr(r.out, "stamp with no key; built without the store") != NULL &&
              strstr(r.out, "result: built here") != NULL,
          "here: a stamp of no key keeps the step out of the store");

    run(copy_a, leak, &r);
    CHECK(r.status == 0 && strstr(r.out, "has the path of this copy") != NULL &&
              strstr(r.out, "result: built here") != NULL,
          "here: an output with the copy's path is not stored");
    run(copy_b, leak, &r);
    CHECK(r.status == 0 && strstr(r.out, "taken from the store") == NULL,
          "here: nothing was stored for it");
}

/** The key a command has as ETUDE_STEP_KEY: the stamp's, and none when a
 * step it uses has no key. */
static void test_key_env(void)
{
    char buf[512], stamp[512];
    char *keyed[] = {"etude-step",
                     "-s",
                     "",
                     "-n",
                     "env",
                     "-t",
                     "st/env.stamp",
                     "-i",
                     "src",
                     "-o",
                     "ek",
                     "--",
                     "sh",
                     "-c",
                     "echo \"etude-step 1 $ETUDE_STEP_KEY here\" > ek",
                     NULL};
    char *unkeyed[] = {"etude-step",
                       "-s",
                       "",
                       "-n",
                       "unkeyed",
                       "-t",
                       "st/unkeyed.stamp",
                       "-d",
                       "none.stamp",
                       "-o",
                       "uk",
                       "--",
                       "sh",
                       "-c",
                       "echo \"${ETUDE_STEP_KEY-none}\" > uk",
                       NULL};
    struct ran r;

    run(copy_a, keyed, &r);
    in_copy(copy_a, "st/env.stamp", stamp, sizeof stamp);
    CHECK(r.status == 0 && stamp[0] && strcmp(in_copy(copy_a, "ek", buf, sizeof buf), stamp) == 0,
          "key env: the command has the key of its stamp as ETUDE_STEP_KEY");
    put(fix_path("copy-a/none.stamp"), "");
    run(copy_a, unkeyed, &r);
    CHECK(r.status == 0 && strcmp(in_copy(copy_a, "uk", buf, sizeof buf), "none\n") == 0,
          "key env: a step that uses a stamp of no key has no ETUDE_STEP_KEY");
}

/** A command that fails, one that makes no output, and a link-mode build
 * that is cut short and then built again. */
static void test_failures(void)
{
    char buf[256], p[1200];
    char *fail[] = {"etude-step", "-s", store, "-n", "fail", "-t", "st/fail.stamp",      "-i",
                    "src",        "-o", "f",   "--", "sh",   "-c", "echo x > f; exit 3", NULL};
    char *none[] = {"etude-step", "-s",  store, "-n",      "none", "-t",   "st/none.stamp",
                    "-i",         "src", "-o",  "missing", "--",   "true", NULL};
    char *lost[] = {
        "etude-step",           "-s", "", "-n", "lost", "-t", "st/lost.stamp", "-o", "x", "--",
        "/nonexistent/command", NULL};
    char *cut[] = {
        "etude-step",
        "-s",
        store,
        "-n",
        "cut",
        "-t",
        "r/cut.stamp",
        "-r",
        "r",
        "-i",
        "src",
        "-o",
        "c",
        "--",
        "sh",
        "-c",
        "mkdir -p \"$ETUDE_STEP_ROOT/c\"; test -f ../go && echo ok > \"$ETUDE_STEP_ROOT/c/f\"",
        NULL};
    struct ran r;

    run(copy_a, fail, &r);
    CHECK(r.status == 3 && !exists(fix_path("copy-a/st/fail.stamp")),
          "failures: the command's status is the program's, and no stamp is written");
    run(copy_b, fail, &r);
    CHECK(r.status == 3 && strstr(r.out, "taken") == NULL,
          "failures: a failed build is not stored");
    run(copy_a, none, &r);
    CHECK(r.status == 1 && strstr(r.out, "the step made no missing") != NULL &&
              !exists(fix_path("copy-a/st/none.stamp")),
          "failures: a missing output fails the step");
    run(copy_a, lost, &r);
    CHECK(r.status == 127, "failures: a command that cannot be run gives 127");

    run(copy_a, cut, &r);
    CHECK(r.status == 1 && strstr(r.out, "its tree stays in") != NULL,
          "failures: a failed link-mode build leaves its partial tree");
    put(fix_path("go"), "");
    run(copy_a, cut, &r);
    CHECK(r.status == 0 && strstr(r.out, "removing the partial entry") != NULL &&
              strstr(r.out, "built in the store") != NULL,
          "failures: the next build removes the partial tree and builds again");
    CHECK(strcmp(in_copy(copy_a, "r/c/f", buf, sizeof buf), "ok\n") == 0,
          "failures: the second build's output is the copy's");
    fix_format(p, sizeof p, "%s/tmp", store);
    CHECK(exists(p), "failures: the store has its tmp directory");
}

/** ccache's names: links to the ccache RACH2_CCACHE names, first on the
 * command's path, and the variables the command gets. */
static void test_ccache(void)
{
    char buf[4096], fake[1200], cmd[2400];
    char *argv[] = {"etude-step",
                    "-s",
                    store,
                    "-n",
                    "cc",
                    "-t",
                    "st/cc.stamp",
                    "-c",
                    "echo",
                    "-w",
                    "/opt/x/bin/i686-linux-gnu-gcc",
                    "-i",
                    "src",
                    "-o",
                    "seen",
                    "--",
                    "sh",
                    "-c",
                    cmd,
                    NULL};
    char *off[] = {"etude-step",
                   "-s",
                   "",
                   "-n",
                   "cc",
                   "-t",
                   "st/cc2.stamp",
                   "-o",
                   "seen2",
                   "--",
                   "sh",
                   "-c",
                   "env | grep -c '^ETUDE_STEP_CCACHE=' > seen2; :",
                   NULL};
    struct ran r;

    fix_format(fake, sizeof fake, "%s/fake-ccache", fix_dir());
    put(fake, "#!/bin/sh\necho \"ccache as $(basename \"$0\")\"\n");
    chmod(fake, 0755);
    fix_format(
        cmd, sizeof cmd,
        "{ cc; i686-linux-gnu-gcc; echo \"path ${PATH%%%%:*}\"; echo \"cc $ETUDE_STEP_CCACHE\"; "
        "echo \"base $CCACHE_BASEDIR\"; echo \"hash $CCACHE_NOHASHDIR\"; } > seen");
    setenv("RACH2_CCACHE", fake, 1);
    run(copy_a, argv, &r);
    in_copy(copy_a, "seen", buf, sizeof buf);
    CHECK(r.status == 0 && strstr(buf, "ccache as cc\n") != NULL &&
              strstr(buf, "ccache as i686-linux-gnu-gcc\n") != NULL,
          "ccache: the compilers' names run ccache");
    CHECK(strstr(buf, "path ") != NULL && strstr(buf, "/etude-step.") != NULL &&
              strstr(buf, "cc /") != NULL && strstr(buf, "hash true\n") != NULL &&
              strstr(buf, copy_a) != NULL,
          "ccache: the command has the names first on its path, and ccache's variables");
    setenv("ETUDE_STEP_CCACHE", "/inherited", 1);
    run(copy_a, off, &r);
    CHECK(r.status == 0 && strcmp(in_copy(copy_a, "seen2", buf, sizeof buf), "0\n") == 0,
          "ccache: without the store the command has no ccache");
    unsetenv("ETUDE_STEP_CCACHE");
    setenv("RACH2_CCACHE", "", 1);
}

/** Two builds of one key at once: the second waits for the first and takes
 * its outputs. */
static void test_together(void)
{
    char buf[4096], cmd[1200], started[1200], go[1200];
    char *argv[] = {"etude-step", "-s", store, "-n", "two", "-t", "st/two.stamp", "-i", "src", "-o",
                    "t",          "--", "sh",  "-c", cmd,   NULL};
    const char *first = fix_path("first.out"), *second = fix_path("second.out");
    pid_t a, b;
    int sa, sb, n;

    fix_format(started, sizeof started, "%s/started", fix_dir());
    fix_format(go, sizeof go, "%s/go-two", fix_dir());
    fix_format(cmd, sizeof cmd,
               "touch %s; while [ ! -f %s ]; do sleep 0.01; done; echo run >> %s/runs; echo t > t",
               started, go, fix_dir());
    a = start(copy_a, argv, first);
    for (n = 0; n < 3000 && !exists(started); n++)
        /* interval: a fixture's file is looked at again every 10 ms */
        usleep(10000);
    b = start(copy_b, argv, second);
    for (n = 0; n < 3000 && strstr(get(second, buf, sizeof buf), "waiting") == NULL; n++)
        /* interval: a fixture's file is looked at again every 10 ms */
        usleep(10000);
    CHECK(strstr(buf, "waiting for another build") != NULL, "together: the second build waits");
    put(go, "");
    while (waitpid(a, &sa, 0) < 0 && errno == EINTR)
        ;
    while (waitpid(b, &sb, 0) < 0 && errno == EINTR)
        ;
    CHECK(WIFEXITED(sa) && WEXITSTATUS(sa) == 0 && WIFEXITED(sb) && WEXITSTATUS(sb) == 0,
          "together: both succeed");
    CHECK(strstr(get(second, buf, sizeof buf), "taken from the store") != NULL,
          "together: the second takes the first's outputs");
    fix_format(cmd, sizeof cmd, "%s/runs", fix_dir());
    CHECK(strcmp(get(cmd, buf, sizeof buf), "run\n") == 0, "together: the command ran once");
}

/** Date each file of `dir` in the store `shelf` three days back. */
static void age(const char *shelf, const char *dir)
{
    char p[1200];
    DIR *d;
    struct dirent *e;
    struct timespec t[2];

    t[0].tv_sec = t[1].tv_sec = time(NULL) - 3 * 86400L;
    t[0].tv_nsec = t[1].tv_nsec = 0;
    fix_format(p, sizeof p, "%s/%s", shelf, dir);
    if ((d = opendir(p)) == NULL)
        return;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        fix_format(p, sizeof p, "%s/%s/%s", shelf, dir, e->d_name);
        utimensat(AT_FDCWD, p, t, AT_SYMLINK_NOFOLLOW);
    }
    closedir(d);
}

/** The count of links in the store's stamps/ for the key `key`. */
static int recorded(const char *shelf, const char *key)
{
    char p[1200];
    DIR *d;
    struct dirent *e;
    int n = 0;

    fix_format(p, sizeof p, "%s/stamps", shelf);
    if ((d = opendir(p)) == NULL)
        return 0;
    while ((e = readdir(d)) != NULL)
        if (strncmp(e->d_name, key, 64) == 0 && e->d_name[64] == '.')
            n++;
    closedir(d);
    return n;
}

/** The path `what`, such as `entries` or `locks`, of the key that the stamp
 * `stamp` of copy a gives, in `out`. */
static void key_path(const char *shelf, const char *stamp, const char *what, char *out, size_t size)
{
    char buf[256], key[65];

    in_copy(copy_a, stamp, buf, sizeof buf);
    if (sscanf(buf, "etude-step 1 %64s", key) != 1)
        key[0] = '\0';
    fix_format(out, size, "%s/%s/%s", shelf, what, key);
}

/* The store of the removal's rows, its entries, and the commands they run. */
static char shelf[PATH_LEN], tc_e[1200], ports_e[1200], demo_e[1200];
static char *ex_tc[] = {"etude-step",
                        "-s",
                        shelf,
                        "-n",
                        "tc",
                        "-t",
                        "e/tc.stamp",
                        "-r",
                        "e",
                        "-i",
                        "src",
                        "-o",
                        "cross",
                        "--",
                        "sh",
                        "-c",
                        "mkdir -p \"$ETUDE_STEP_ROOT/cross\"",
                        NULL};
static char *ex_ports[] = {"etude-step",
                           "-s",
                           shelf,
                           "-n",
                           "ports",
                           "-t",
                           "e/ports.stamp",
                           "-r",
                           "e",
                           "-d",
                           "e/tc.stamp",
                           "-u",
                           "cross",
                           "-o",
                           "root",
                           "--",
                           "sh",
                           "-c",
                           "mkdir -p \"$ETUDE_STEP_ROOT/root\"",
                           NULL};
static char *ex_demo[] = {"etude-step", "-s", shelf, "-n", "demo", "-t",          "e/demo.stamp",
                          "-o",         "dm", "--",  "sh", "-c",   "echo d > dm", NULL};
static char *ex_go[] = {"etude-step", "-s", shelf, "-e", "2", NULL};
static char *ex_list[] = {"etude-step", "-p", "-s", shelf, "-e", "2", NULL};
static char *ex_named[] = {"etude-step", "-s", shelf, "-e", "2", "-n", "x", NULL};
static char *ex_word[] = {"etude-step", "-s", shelf, "-e", "two", NULL};
static char *ex_nostore[] = {"etude-step", "-e", "2", NULL};

/** The removal of entries, what stays: what a recorded stamp names, what a
 * kept entry uses, and an entry used recently or whose lock is held, each
 * with what it uses. */
static void test_expire(void)
{
    char p[1200];
    struct ran r;
    int fd;

    fix_format(p, sizeof p, "%s/store-e", fix_dir());
    mkdir(p, 0700);
    if (realpath(p, shelf) == NULL) {
        CHECK(0, "expire: the store is made");
        return;
    }
    run(copy_a, ex_tc, &r);
    run(copy_a, ex_ports, &r);
    run(copy_a, ex_demo, &r);
    run(copy_b, ex_demo, &r);
    CHECK(r.status == 0 && strstr(r.out, "taken from the store") != NULL,
          "expire: the steps are built and taken");
    key_path(shelf, "e/tc.stamp", "entries", tc_e, sizeof tc_e);
    key_path(shelf, "e/ports.stamp", "entries", ports_e, sizeof ports_e);
    key_path(shelf, "e/demo.stamp", "entries", demo_e, sizeof demo_e);
    CHECK(recorded(shelf, demo_e + strlen(shelf) + 9) == 2,
          "expire: the stamp of each copy that has the key is recorded");
    age(shelf, "entries");
    age(shelf, "locks");
    run(copy_a, ex_go, &r);
    CHECK(r.status == 0 && strstr(r.out, "result: removed 0 entries") != NULL && exists(tc_e) &&
              exists(ports_e) && exists(demo_e),
          "expire: an entry that a recorded stamp names stays");

    unlink(fix_path("copy-a/e/tc.stamp"));
    run(copy_a, ex_go, &r);
    CHECK(r.status == 0 && strstr(r.out, "removed 0 entries, 0.0 GiB, and 1 links") != NULL &&
              exists(tc_e),
          "expire: an entry that a kept entry uses stays, and the gone stamp's link goes");

    unlink(fix_path("copy-a/e/ports.stamp"));
    fix_format(p, sizeof p, "%s/locks/%s", shelf, ports_e + strlen(shelf) + 9);
    utimensat(AT_FDCWD, p, NULL, 0);
    run(copy_a, ex_go, &r);
    CHECK(r.status == 0 && strstr(r.out, "removed 0 entries") != NULL && exists(ports_e) &&
              exists(tc_e),
          "expire: a recent use keeps the entry and what it uses");

    age(shelf, "locks");
    fd = open(p, O_RDWR);
    CHECK(fd >= 0 && flock(fd, LOCK_EX) == 0, "expire: the test holds a lock");
    run(copy_a, ex_go, &r);
    CHECK(r.status == 0 && strstr(r.out, "removed 0 entries") != NULL && exists(ports_e) &&
              exists(tc_e),
          "expire: a held lock keeps the entry and what it uses");
    if (fd >= 0)
        close(fd);
}

/** The removal of entries, what goes: -p removes nothing; an entry that no
 * stamp names goes with its lock file and is built again; and the refusals. */
static void test_expire_removal(void)
{
    char p[1200], buf[256];
    struct ran r;

    fix_format(p, sizeof p, "%s/locks/%s", shelf, ports_e + strlen(shelf) + 9);
    run(copy_a, ex_list, &r);
    CHECK(r.status == 0 && strstr(r.out, "would remove 2 entries") != NULL &&
              strstr(r.out, ", ports, ") != NULL && strstr(r.out, ", tc, ") != NULL &&
              exists(ports_e) && exists(tc_e),
          "expire: -p lists the entries that would go and removes nothing");
    run(copy_a, ex_go, &r);
    CHECK(r.status == 0 && strstr(r.out, "result: removed 2 entries") != NULL &&
              strstr(r.out, "unused for 3 days") != NULL && !exists(ports_e) && !exists(tc_e) &&
              !exists(p) && exists(demo_e),
          "expire: an entry nobody names or uses goes, with its lock file");

    unlink(fix_path("copy-b/e/demo.stamp"));
    put(fix_path("copy-a/e/demo.stamp"),
        "etude-step 1 1111111111111111111111111111111111111111111111111111111111111111 here\n");
    run(copy_a, ex_go, &r);
    CHECK(r.status == 0 && strstr(r.out, "result: removed 1 entries") != NULL && !exists(demo_e),
          "expire: a stamp that names another key no longer keeps the entry");

    run(copy_a, ex_tc, &r);
    CHECK(r.status == 0 && strstr(r.out, "built in the store") != NULL && exists(tc_e) &&
              strcmp(in_copy(copy_a, "e/tc.stamp", buf, sizeof buf), "") != 0,
          "expire: a removed entry is built again");

    run(copy_a, ex_named, &r);
    CHECK(r.status == 2, "expire: -e takes no option of a step");
    run(copy_a, ex_word, &r);
    CHECK(r.status == 2, "expire: DAYS is a number");
    run(copy_a, ex_nostore, &r);
    CHECK(r.status == 2, "expire: -e needs a store");
    remove_tree(shelf);
}

/** The usage errors. */
static void test_refusals(void)
{
    char *noname[] = {"etude-step", "-t", "s", "-o", "x", "--", "true", NULL};
    char *nested[] = {"etude-step", "-n", "x",   "-t", "s",    "-o",
                      "a",          "-o", "a/b", "--", "true", NULL};
    char *dots[] = {"etude-step", "-n", "x", "-t", "s", "-o", "../a", "--", "true", NULL};
    char *useless[] = {"etude-step", "-n", "x", "-t", "s",    "-u",
                       "c",          "-o", "a", "--", "true", NULL};
    char *nostore[] = {
        "etude-step", "-s", "/nonexistent/store", "-n", "x", "-t", "s", "-o", "a", "--",
        "true",       NULL};
    struct ran r;

    run(copy_a, noname, &r);
    CHECK(r.status == 2, "refusals: a step needs a name");
    run(copy_a, nested, &r);
    CHECK(r.status == 2, "refusals: an output may not be inside another");
    run(copy_a, dots, &r);
    CHECK(r.status == 2, "refusals: an output may not leave the copy");
    run(copy_a, useless, &r);
    CHECK(r.status == 2, "refusals: a use needs a root");
    run(copy_a, nostore, &r);
    CHECK(r.status == 1 && strstr(r.out, "is not a directory") != NULL,
          "refusals: a store that is not there");
}

int main(void)
{
    char p[1200];

    unsetenv("ETUDE_STEP_SHIMS");
    unsetenv("ETUDE_STEP_CCACHE");
    setenv("RACH2_CCACHE", "", 1);
    fix_format(p, sizeof p, "%s/store", fix_dir());
    mkdir(p, 0700);
    if (realpath(p, store) == NULL) {
        fprintf(stderr, "etude-step-test: cannot make %s\n", p);
        return 1;
    }
    make_copy("copy-a", copy_a, sizeof copy_a, "one\n");
    make_copy("copy-b", copy_b, sizeof copy_b, "two\n");
    test_key();
    test_copy();
    test_link();
    test_here();
    test_key_env();
    test_failures();
    test_ccache();
    test_together();
    test_expire();
    test_expire_removal();
    test_refusals();
    fix_format(p, sizeof p, "%s", store);
    remove_tree(p);
    if (fails != 0) {
        fprintf(stderr, "etude-step-test: %d checks failed\n", fails);
        return 1;
    }
    fix_passed();
    printf("etude-step-test: pass\n");
    return 0;
}
