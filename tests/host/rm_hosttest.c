/**
 * Host test of `rm`, utils/rm.c, run through its entry point on host files
 * with an `unlink` that fails for one path.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *denied;
static int failures;
static int checks;
static int saved_cwd = -1;
static char fixture[] = "/tmp/rach2-rm-hosttest.XXXXXX";

/**
 * unlink(2), but -1 with EACCES for the path `denied`.
 *
 * The failure stands for a link in a directory the caller cannot write,
 * whatever the test's uid; the kernel's permission checks are not tested.
 */
static int denied_unlink(const char *path)
{
    if (denied && strcmp(path, denied) == 0) {
        errno = EACCES;
        return -1;
    }
    return unlink(path);
}

#define unlink denied_unlink
#define main   rm_main
#include "rm.c"
#undef main
#undef unlink

/** Remove every fixture the rows make, without `rm`, and the fixture
 * directory; registered with atexit. */
static void cleanup(void)
{
    unlink("locked/link");
    unlink("tree/denied");
    unlink("tree/sub/file");
    unlink("tree/broken");
    unlink("link");
    unlink("plain");
    unlink("target/keep");
    rmdir("tree/sub");
    rmdir("tree");
    rmdir("locked");
    rmdir("target");
    if (saved_cwd >= 0) {
        fchdir(saved_cwd);
        close(saved_cwd);
    }
    rmdir(fixture);
}

/** Exit 2 with `message` when `ok` is 0, so that a fault in a fixture is
 * not counted as a fault of `rm`. */
static void require(int ok, const char *message)
{
    if (!ok) {
        fprintf(stderr, "rm-hosttest: fixture: %s\n", message);
        exit(2);
    }
}

/** Count a check, and a failure with `message` when `ok` is 0. */
static void check(int ok, const char *message)
{
    ++checks;
    if (!ok) {
        fprintf(stderr, "rm-hosttest: fail: %s\n", message);
        ++failures;
    }
}

/** Make the file `path` with the bytes "keep\n". */
static void create_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);

    require(fd >= 0, "create file");
    require(write(fd, "keep\n", 5) == 5, "write file");
    require(close(fd) == 0, "close file");
}

/** Whether target/keep still has its bytes. */
static int target_intact(void)
{
    char bytes[6];
    int fd = open("target/keep", O_RDONLY);
    ssize_t n;

    if (fd < 0) {
        return 0;
    }
    n = read(fd, bytes, sizeof(bytes));
    close(fd);
    return n == 5 && memcmp(bytes, "keep\n", 5) == 0;
}

/** Whether `path` does not exist, as a link or anything else. */
static int missing(const char *path)
{
    struct stat st;

    return lstat(path, &st) == -1 && errno == ENOENT;
}

/** Run `rm`, with -r when `recursive`, on `path`; its exit status. */
static int run_rm(const char *path, int recursive)
{
    char *argv[4];
    int argc = 1;

    argv[0] = "rm";
    if (recursive) {
        argv[argc++] = "-r";
    }
    argv[argc++] = (char *)path;
    argv[argc] = NULL;
    return rm_main(argc, argv);
}

/**
 * Run every row in a new directory under /tmp; 0 when each passes, 1
 * otherwise, and 2 when a fixture cannot be made.
 *
 * A symbolic link whose unlink is denied, as an operand and inside a tree
 * removed with -r, fails the command, stays a link and leaves its target's
 * bytes alone. The command removes a link, a file and a tree with a
 * dangling link in it, needs -r for a directory, and fails on a missing
 * operand.
 */
int main(void)
{
    struct stat st;

    saved_cwd = open(".", O_RDONLY);
    require(saved_cwd >= 0, "save working directory");
    require(mkdtemp(fixture) != NULL, "create temporary directory");
    require(chdir(fixture) == 0, "enter temporary directory");
    require(atexit(cleanup) == 0, "register cleanup");
    require(mkdir("target", 0700) == 0, "create target");
    create_file("target/keep");
    require(mkdir("locked", 0700) == 0, "create link parent");
    require(symlink("../target", "locked/link") == 0, "create denied link");
    denied = "locked/link";
    check(run_rm(denied, 1) == 1, "denied operand returns failure");
    check(target_intact(), "denied operand preserves target contents");
    check(lstat(denied, &st) == 0 && S_ISLNK(st.st_mode), "denied operand remains a link");
    unlink("target/keep");
    create_file("target/keep");

    require(mkdir("tree", 0700) == 0, "create tree");
    require(symlink("../target", "tree/denied") == 0, "create denied child");
    denied = "tree/denied";
    check(run_rm("tree", 1) == 1, "denied child returns failure");
    check(target_intact(), "denied child preserves target contents");
    check(lstat(denied, &st) == 0 && S_ISLNK(st.st_mode), "denied child remains a link");
    unlink("target/keep");
    create_file("target/keep");
    denied = NULL;
    require(unlink("tree/denied") == 0, "remove denied child");

    require(symlink("target", "link") == 0, "create removable link");
    check(run_rm("link", 1) == 0 && missing("link"), "remove link operand");
    check(target_intact(), "removed link preserves target contents");
    create_file("plain");
    check(run_rm("plain", 0) == 0 && missing("plain"), "remove plain file");
    require(mkdir("tree/sub", 0700) == 0, "create subtree");
    create_file("tree/sub/file");
    require(symlink("absent", "tree/broken") == 0, "create dangling link");
    check(run_rm("tree", 0) == 1, "directory requires recursion");
    check(run_rm("tree", 1) == 0 && missing("tree"), "remove tree and dangling link");
    check(run_rm("absent", 1) == 1, "missing operand returns failure");
    if (failures) {
        fprintf(stderr, "rm-hosttest: result: fail: %d of %d checks\n", failures, checks);
        return 1;
    }
    printf("rm-hosttest: result: pass: %d checks\n", checks);
    return 0;
}
