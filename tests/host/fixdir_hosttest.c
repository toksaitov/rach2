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
 * of 101 characters. A child whose exit comes without `fix_passed`, as an
 * exit on a failed check does, keeps its directory; one that said it passed
 * has it removed. This process makes no fixture directory of its own, so
 * the children's are their own.
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

/**
 * The fixture directory a child makes and tells through a pipe, whether it
 * is there after the child's exit, with status `code`, and after a
 * `fix_passed` first when `pass`; a directory left is removed.
 */
static int left_after(int pass, int code)
{
    char dir[FIX_PATH_MAX], cmd[FIX_PATH_MAX + 16];
    struct stat st;
    int ends[2], status = 0, there;
    ssize_t n;
    pid_t pid;

    if (pipe(ends) != 0) {
        return -1;
    }
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        const char *made = fix_dir();

        if (write(ends[1], made, strlen(made)) < 0) {
            _exit(2);
        }
        if (pass) {
            fix_passed();
        }
        exit(code);
    }
    close(ends[1]);
    n = read(ends[0], dir, sizeof(dir) - 1);
    close(ends[0]);
    if (pid < 0 || n <= 0 || waitpid(pid, &status, 0) != pid) {
        return -1;
    }
    dir[n] = '\0';
    there = stat(dir, &st) == 0;
    if (there) {
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
        if (system(cmd) != 0) {
            return -1;
        }
    }
    return there;
}

int main(void)
{
    plain_bases();
    CHECK(left_after(0, 1) == 1, "a test that exits failed keeps its fixture directory");
    CHECK(left_after(0, 0) == 1, "a test that exits without saying it passed keeps it");
    CHECK(left_after(1, 0) == 0, "a test that said it passed has its directory removed");
    printf("fixdir-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
