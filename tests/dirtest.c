/**
 * dirtest: the getcwd and chdir calls at the root.
 *
 * getcwd reports the current directory; chdir to "/", "." and "..", whose
 * parent at the root is the root itself, all succeed and leave the working
 * directory at "/"; a name that does not exist fails, and a plain file
 * fails with ENOTDIR.
 *
 * Prints a line for each test and exits 0 when every one passed, 1
 * otherwise. Writes etude's report records (probe_report.h) for the case
 * ETUDE_CASE names, an assertion for each test: `getcwd`, `chdir-dot`,
 * `chdir-root`, `chdir-dotdot`, `getcwd-root`, `missing` and `enotdir`.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define STDOUT_FILENO 1

#define ENOTDIR 20

/** write(2). */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** exit(2); does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** chdir(2). */
static int sys_chdir(const char *path)
{
    return syscall1(SYS_CHDIR, (long)path);
}

/**
 * getcwd(2) into `buf`: the path's length with its NUL, or a negative errno.
 * That is the system call's return, as Linux has it, and not the C
 * library's pointer.
 */
static int sys_getcwd(char *buf, size_t size)
{
    return syscall2(SYS_GETCWD, (long)buf, (long)size);
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

/** Write the string `s` and a newline to standard output. */
static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

/** Write the string `s` to standard output. */
static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

/* The counts of the tests that passed and failed. */
static int passed, failed;

/** The test of the assertion `id`, printed as `label`: a chdir to `path`,
 * which must succeed; `why` is the failure's detail. */
static void chdir_row(const char *id, const char *label, const char *path, const char *why)
{
    report_part(id);
    print(label);
    if (sys_chdir(path) == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        report_miss(why);
        failed++;
    }
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * The probe, which `_start` calls with the initial stack: the getcwd and
 * chdir tests in order, then the count of each verdict, as one digit each.
 */
void probe_main(unsigned long *sp)
{
    char cwdbuf[64];
    int cwd;
    int result;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.dirtest"));
    puts("Directory syscall test starting...");
    puts("");

    /* Test 1: getcwd initial value */
    report_part("getcwd");
    print("Test 1: getcwd()... ");
    cwd = sys_getcwd(cwdbuf, sizeof(cwdbuf));
    if (cwd > 0 && cwdbuf[0] == '/') {
        print("OK (cwd=\"");
        print(cwdbuf);
        puts("\")");
        passed++;
    } else {
        puts("FAILED");
        report_miss("getcwd did not give an absolute path");
        failed++;
    }

    /* Test 2: chdir("."), which stays in the current directory. */
    chdir_row("chdir-dot", "Test 2: chdir(\".\")... ", ".", "chdir(\".\") failed");

    /* Test 3: chdir("/"). */
    chdir_row("chdir-root", "Test 3: chdir(\"/\")... ", "/", "chdir(\"/\") failed");

    /* Test 4: chdir(".."): the root's parent is the root. */
    chdir_row("chdir-dotdot", "Test 4: chdir(\"..\")... ", "..", "chdir(\"..\") failed");

    /* Test 5: getcwd after chdir operations */
    report_part("getcwd-root");
    print("Test 5: getcwd() after chdir... ");
    cwd = sys_getcwd(cwdbuf, sizeof(cwdbuf));
    if (cwd > 0 && cwdbuf[0] == '/' && cwdbuf[1] == '\0') {
        puts("OK (still \"/\")");
        passed++;
    } else {
        puts("FAILED");
        report_miss("getcwd after the chdirs did not give \"/\"");
        failed++;
    }

    /* Test 6: chdir to a name that does not exist fails. */
    report_part("missing");
    print("Test 6: chdir(\"nonexistent\")... ");
    result = sys_chdir("nonexistent");
    if (result < 0) {
        puts("OK (failed as expected)");
        passed++;
    } else {
        puts("FAILED (should have failed)");
        report_miss("chdir(\"nonexistent\") succeeded");
        failed++;
    }

    /* Test 7: chdir to hello.txt, a file, fails. */
    report_part("enotdir");
    print("Test 7: chdir(\"hello.txt\")... ");
    result = sys_chdir("hello.txt");
    if (result == -ENOTDIR) {
        puts("OK (ENOTDIR: a file, not a directory)");
        passed++;
    } else {
        puts("FAILED (not ENOTDIR)");
        report_miss("chdir(\"hello.txt\") did not fail with ENOTDIR");
        failed++;
    }
    report_finish();

    puts("");
    print("Results: ");
    {
        char num = '0' + passed;
        sys_write(STDOUT_FILENO, &num, 1);
    }
    print(" passed, ");
    {
        char num = '0' + failed;
        sys_write(STDOUT_FILENO, &num, 1);
    }
    puts(" failed");

    if (failed == 0) {
        puts("All directory syscalls passed!");
        sys_exit(0);
    } else {
        puts("Some tests failed!");
        sys_exit(1);
    }
}
