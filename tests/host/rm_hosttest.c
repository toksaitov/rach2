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
 * Inject EACCES for one unlink; use host files and directories otherwise.
 *
 * This models a link in a non-writable parent without depending on the
 * test process's uid. It does not test kernel permission enforcement.
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

/** Remove known fixtures independently of the utility under test. */
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

/** Stop on a fixture error so it cannot count as a utility failure. */
static void require(int ok, const char *message)
{
    if (!ok) {
        fprintf(stderr, "rm-hosttest: fixture: %s\n", message);
        exit(2);
    }
}

static void check(int ok, const char *message)
{
    ++checks;
    if (!ok) {
        fprintf(stderr, "rm-hosttest: fail: %s\n", message);
        ++failures;
    }
}

static void create_file(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);

    require(fd >= 0, "create file");
    require(write(fd, "keep\n", 5) == 5, "write file");
    require(close(fd) == 0, "close file");
}

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

static int missing(const char *path)
{
    struct stat st;

    return lstat(path, &st) == -1 && errno == ENOENT;
}

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
 * Check denied symlink unlinks at the operand and inside a directory tree.
 *
 * The target must retain its bytes, and the link must remain. Also check
 * successful link and file unlinks, a dangling link, missing input, and
 * recursive directory removal through the production command entry point.
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
