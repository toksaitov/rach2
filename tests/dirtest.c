/**
 * Exercises the getcwd and chdir syscalls at the root. getcwd reports the
 * current directory; chdir to "/", ".", and ".." (whose parent at the root is
 * the root itself) all succeed and leave the working directory at "/"; a
 * nonexistent name fails, and a plain file fails because it is not a directory.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

/* Syscall numbers (32-bit Linux / Rach2 compatible) */

static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

static int sys_chdir(const char *path)
{
    return syscall1(SYS_CHDIR, (long)path);
}

/**
 * Returns the path length including the NUL on success (the path is written
 * into `buf`), or a negative errno on error. This is the Linux getcwd(2) ABI.
 */
static int sys_getcwd(char *buf, size_t size)
{
    return syscall2(SYS_GETCWD, (long)buf, (long)size);
}

static size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

/**
 * ELF entry point. Runs the getcwd and chdir cases, tallies passes and
 * failures, prints the count, and exits 0 only if all pass.
 */
PROBE_START void _start(void)
{
    char cwdbuf[64];
    int cwd;
    int result;
    int passed = 0;
    int failed = 0;

    puts("Directory syscall test starting...");
    puts("");

    /* Test 1: getcwd initial value */
    print("Test 1: getcwd()... ");
    cwd = sys_getcwd(cwdbuf, sizeof(cwdbuf));
    if (cwd > 0 && cwdbuf[0] == '/') {
        print("OK (cwd=\"");
        print(cwdbuf);
        puts("\")");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 2: chdir(".") - stay in current directory */
    print("Test 2: chdir(\".\")... ");
    result = sys_chdir(".");
    if (result == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 3: chdir("/") - go to root */
    print("Test 3: chdir(\"/\")... ");
    result = sys_chdir("/");
    if (result == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 4: chdir("..") - parent of root is root */
    print("Test 4: chdir(\"..\")... ");
    result = sys_chdir("..");
    if (result == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 5: getcwd after chdir operations */
    print("Test 5: getcwd() after chdir... ");
    cwd = sys_getcwd(cwdbuf, sizeof(cwdbuf));
    if (cwd > 0 && cwdbuf[0] == '/' && cwdbuf[1] == '\0') {
        puts("OK (still \"/\")");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 6: chdir("nonexistent") - should fail (ENOENT) */
    print("Test 6: chdir(\"nonexistent\")... ");
    result = sys_chdir("nonexistent");
    if (result < 0) {
        puts("OK (failed as expected)");
        passed++;
    } else {
        puts("FAILED (should have failed)");
        failed++;
    }

    /* Test 7: chdir("hello.txt") - should fail (ENOTDIR - it's a file) */
    print("Test 7: chdir(\"hello.txt\")... ");
    result = sys_chdir("hello.txt");
    if (result < 0) {
        puts("OK (failed as expected - file, not dir)");
        passed++;
    } else {
        puts("FAILED (should have failed)");
        failed++;
    }

    puts("");
    print("Results: ");
    /* Print passed count */
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
