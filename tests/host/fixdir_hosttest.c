/**
 * Check the fixture directory of the host tests (compat/fixdir_host.c).
 *
 * host-deps:
 *
 * The helper is included whole. $TMPDIR is taken only when it is an absolute
 * path of at most 100 characters, each one a shell passes through unchanged,
 * since the tests put the fixture paths in their commands unquoted: a
 * semicolon, a space, a redirection, an ampersand, a wildcard, a quote or a
 * dollar sign sends the directory to /tmp, and so do a relative path and one
 * of 101 characters. A child given such a $TMPDIR makes its directory in
 * /tmp, and one given a plain $TMPDIR makes it there. A text `fix_format`
 * cannot fit ends the process with status 1, where snprintf would cut it
 * short. A child whose exit comes without `fix_passed`, as an exit on a
 * failed check does, keeps its directory with the files in it; one that said
 * it passed has it removed, files and all. Each child must exit with the
 * status it was given. This process makes no fixture directory of its own,
 * so the children's are their own.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "compat/fixdir_host.c"

static int failures;

#define CHECK(condition, detail)                             \
    do {                                                     \
        if (!(condition)) {                                  \
            printf("fixdir-hosttest: failed: %s\n", detail); \
            failures++;                                      \
        }                                                    \
    } while (0)

/** The $TMPDIR values the helper takes and the ones it sends to /tmp. */
static void plain_bases(void)
{
    static const char *refused[] = {"/tmp/a;b", "/tmp/a b", "/tmp/a>b",   "/tmp/a&b",
                                    "/tmp/*",   "/tmp/a'b", "/tmp/$HOME", "/tmp/a|b",
                                    "tmp/rel",  "",         "/tmp/a\nb"};
    char longest[FIX_TMP_MAX + 2];
    size_t i;

    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        CHECK(!fix_plain(refused[i]), "a $TMPDIR a shell would change, or relative, is not taken");
    }
    CHECK(!fix_plain(NULL), "no $TMPDIR is not taken");
    CHECK(fix_plain("/var/folders/32/q1rc49295sg4rbt0fb9z0psh0000gn/T/") &&
              fix_plain("/home/user_1/tmp-2.x+y"),
          "a $TMPDIR of letters, digits and / . _ + - is taken");
    memset(longest, 'a', sizeof(longest));
    longest[0] = '/';
    longest[FIX_TMP_MAX] = '\0';
    CHECK(fix_plain(longest), "a $TMPDIR of 100 characters is taken");
    longest[FIX_TMP_MAX] = 'a';
    longest[FIX_TMP_MAX + 1] = '\0';
    CHECK(!fix_plain(longest), "a $TMPDIR of 101 characters is not taken");
}

/** A text that fits is formatted whole; one that does not ends the child
 * that formats it, with status 1. */
static void format_fits(void)
{
    char buf[8];
    pid_t pid;
    int status = 0;

    fix_format(buf, sizeof(buf), "%s-%d", "ab", 42);
    CHECK(strcmp(buf, "ab-42") == 0, "a text that fits is formatted whole");
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        fix_format(buf, sizeof(buf), "%s", "twelve bytes");
        _exit(0);
    }
    CHECK(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
              WEXITSTATUS(status) == 1,
          "a text that does not fit ends the test with status 1");
}

/**
 * Run a child that, with $TMPDIR set to `tmp` when that is not NULL, makes
 * its fixture directory, writes `sub/kept` in it, tells the directory through
 * a pipe into `dir`, calls `fix_passed` when `pass`, and exits with `code`.
 * Returns whether the directory is there after the exit, with the file and
 * its text, or -1 when the child exited otherwise or the file is not whole.
 * A directory left is removed.
 */
static int run_child(const char *tmp, int pass, int code, char *dir, size_t size)
{
    char cmd[FIX_PATH_MAX + 16], text[8] = {0};
    struct stat st;
    int ends[2], status = 0, there;
    ssize_t n;
    pid_t pid;
    FILE *f;

    if (pipe(ends) != 0) {
        return -1;
    }
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        const char *made;

        if (tmp && setenv("TMPDIR", tmp, 1) != 0) {
            _exit(3);
        }
        made = fix_dir();
        f = mkdir(fix_path("sub"), 0700) == 0 ? fopen(fix_path("sub/kept"), "w") : NULL;
        if (!f || fputs("kept\n", f) < 0 || fclose(f) != 0 ||
            write(ends[1], made, strlen(made)) < 0) {
            _exit(3);
        }
        if (pass) {
            fix_passed();
        }
        exit(code);
    }
    close(ends[1]);
    n = read(ends[0], dir, size - 1);
    close(ends[0]);
    if (pid < 0 || n <= 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != code) {
        return -1;
    }
    dir[n] = '\0';
    there = stat(dir, &st) == 0;
    if (there) {
        snprintf(cmd, sizeof(cmd), "%s/sub/kept", dir);
        f = fopen(cmd, "r");
        if (!f || fread(text, 1, sizeof(text) - 1, f) != 5 || strcmp(text, "kept\n") != 0) {
            there = -1;
        }
        if (f) {
            fclose(f);
        }
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        if (system(cmd) != 0) {
            return -1;
        }
    }
    return there;
}

/** The fixture directory a child keeps after its exit with status `code`,
 * after a `fix_passed` first when `pass`, as `run_child` answers. */
static int left_after(int pass, int code)
{
    char dir[FIX_PATH_MAX];

    return run_child(NULL, pass, code, dir, sizeof(dir));
}

/** A child given a $TMPDIR with a semicolon, which exists, makes its
 * directory in /tmp, and one given a plain $TMPDIR makes it there. */
static void tmpdir_rows(void)
{
    char base[32], dir[FIX_PATH_MAX];

    strcpy(base, "/tmp/fixdir;XXXXXX");
    if (!mkdtemp(base)) {
        CHECK(0, "a directory with a semicolon in /tmp");
        return;
    }
    CHECK(run_child(base, 1, 0, dir, sizeof(dir)) == 0 &&
              strncmp(dir, "/tmp/rach2_fixtures.", 20) == 0,
          "a child given a $TMPDIR with a semicolon makes its directory in /tmp");
    rmdir(base);
    strcpy(base, "/tmp/fixdir_plain.XXXXXX");
    if (!mkdtemp(base)) {
        CHECK(0, "a plain directory in /tmp");
        return;
    }
    CHECK(run_child(base, 1, 0, dir, sizeof(dir)) == 0 && strncmp(dir, base, strlen(base)) == 0 &&
              dir[strlen(base)] == '/',
          "a child given a plain $TMPDIR makes its directory there");
    rmdir(base);
}

int main(void)
{
    plain_bases();
    tmpdir_rows();
    format_fits();
    CHECK(left_after(0, 1) == 1,
          "a test that exits failed keeps its fixture directory, files and all");
    CHECK(left_after(0, 0) == 1, "a test that exits without saying it passed keeps it");
    CHECK(left_after(1, 0) == 0,
          "a test that said it passed has its directory removed, files and all");
    printf("fixdir-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
