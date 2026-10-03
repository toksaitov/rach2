/**
 * Exercises dup2 for I/O redirection: create a file, dup2 it onto stdout,
 * write through stdout, then read the file back and check the content.
 * Also covers the identity case dup2(fd, fd) and failure on invalid fds.
 *
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* Syscall numbers (32-bit Linux / Rach2 compatible) */

/* Open flags */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

static int sys_read(int fd, void *buf, size_t count)
{
    return syscall3(SYS_READ, (long)fd, (long)buf, (long)count);
}

static int sys_open(const char *path, int flags)
{
    return syscall2(SYS_OPEN, (long)path, (long)flags);
}

static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

static int sys_creat(const char *path, unsigned int mode)
{
    return syscall2(SYS_CREAT, (long)path, (long)mode);
}

static int sys_unlink(const char *path)
{
    return syscall1(SYS_UNLINK, (long)path);
}

static int sys_dup2(int oldfd, int newfd)
{
    return syscall2(SYS_DUP2, (long)oldfd, (long)newfd);
}

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

static size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

static int strcmp(const char *s1, const char *s2)
{
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static int strncmp(const char *s1, const char *s2, size_t n)
{
    while (n > 0 && *s1 && *s1 == *s2) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0)
        return 0;
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

/* Output goes to stderr, which the dup2 tests below leave untouched. */
#define puts_stderr(s)                          \
    do {                                        \
        sys_write(STDERR_FILENO, s, strlen(s)); \
        sys_write(STDERR_FILENO, "\n", 1);      \
    } while (0)

#define print_stderr(s) sys_write(STDERR_FILENO, s, strlen(s))

static void print_num_stderr(int n)
{
    char buf[12];
    int i = 0;

    if (n < 0) {
        sys_write(STDERR_FILENO, "-", 1);
        n = -n;
    }

    if (n == 0) {
        buf[i++] = '0';
    } else {
        while (n > 0) {
            buf[i++] = '0' + (n % 10);
            n /= 10;
        }
    }

    while (i > 0) {
        sys_write(STDERR_FILENO, &buf[--i], 1);
    }
}

static const char *test_file = "dup2test.tmp";
static const char *test_content = "Hello from dup2!";

/**
 * ELF entry point. Runs the dup2 tests in order: the identity dup2(fd, fd),
 * redirecting stdout onto a file and reading the bytes back, and the failure
 * cases for an invalid oldfd and an invalid newfd. Exits 0 on all-pass.
 */
PROBE_START void _start(void)
{
    char buf[128];
    int fd, fd2, n, result;
    int passed = 0;
    int failed = 0;
    int saved_stdout = -1;

    puts_stderr("Dup2 syscall test starting...");
    puts_stderr("");

    /* Test 1: dup2(fd, fd) returns fd */
    print_stderr("Test 1: dup2(1, 1) should return 1... ");
    result = sys_dup2(STDOUT_FILENO, STDOUT_FILENO);
    if (result == STDOUT_FILENO) {
        puts_stderr("OK");
        passed++;
    } else {
        print_stderr("FAILED (returned ");
        print_num_stderr(result);
        puts_stderr(")");
        failed++;
    }

    /* Test 2: Create file and redirect stdout to it */
    puts_stderr("");
    print_stderr("Test 2: redirect stdout to file...\n");

    /* Create a temporary file */
    print_stderr("  Creating ");
    print_stderr(test_file);
    print_stderr("... ");
    fd = sys_creat(test_file, 0644);
    if (fd < 0) {
        puts_stderr("FAILED");
        failed++;
        goto cleanup;
    }
    print_stderr("OK (fd=");
    print_num_stderr(fd);
    puts_stderr(")");

    /* Redirect stdout to the file, then read it back to confirm the write
     * landed there; stdout is not restored afterward. */
    print_stderr("  dup2(");
    print_num_stderr(fd);
    print_stderr(", 1)... ");
    result = sys_dup2(fd, STDOUT_FILENO);
    if (result != STDOUT_FILENO) {
        print_stderr("FAILED (returned ");
        print_num_stderr(result);
        puts_stderr(")");
        failed++;
        sys_close(fd);
        goto cleanup;
    }
    puts_stderr("OK");

    /* Write via stdout - should go to file */
    print_stderr("  Writing via stdout... ");
    n = sys_write(STDOUT_FILENO, test_content, strlen(test_content));
    if (n == (int)strlen(test_content)) {
        puts_stderr("OK");
    } else {
        print_stderr("FAILED (wrote ");
        print_num_stderr(n);
        puts_stderr(" bytes)");
        failed++;
    }

    /* Close the original fd (stdout still points to file) */
    sys_close(fd);

    /* Now restore stdout to console by creating a console fd */
    /* In Rach2, fd 2 (stderr) is still console, so dup2 stderr to stdout */
    print_stderr("  Restoring stdout from stderr... ");
    result = sys_dup2(STDERR_FILENO, STDOUT_FILENO);
    if (result == STDOUT_FILENO) {
        puts_stderr("OK");
    } else {
        puts_stderr("FAILED");
        /* Continue anyway - we can still use stderr */
    }

    /* Test 3: Read back the file and verify content */
    puts_stderr("");
    print_stderr("Test 3: verify file content...\n");

    print_stderr("  Opening ");
    print_stderr(test_file);
    print_stderr("... ");
    fd = sys_open(test_file, O_RDONLY);
    if (fd < 0) {
        puts_stderr("FAILED");
        failed++;
        goto cleanup;
    }
    print_stderr("OK (fd=");
    print_num_stderr(fd);
    puts_stderr(")");

    print_stderr("  Reading... ");
    n = sys_read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        puts_stderr("FAILED");
        sys_close(fd);
        failed++;
        goto cleanup;
    }
    buf[n] = '\0';
    print_stderr("OK (");
    print_num_stderr(n);
    puts_stderr(" bytes)");

    print_stderr("  Content: \"");
    print_stderr(buf);
    puts_stderr("\"");

    print_stderr("  Comparing... ");
    if (strncmp(buf, test_content, strlen(test_content)) == 0) {
        puts_stderr("OK (content matches!)");
        passed++;
    } else {
        puts_stderr("FAILED (content mismatch)");
        failed++;
    }
    sys_close(fd);

    /* Test 4: dup2 with invalid oldfd should fail */
    puts_stderr("");
    print_stderr("Test 4: dup2(99, 5) with invalid oldfd... ");
    result = sys_dup2(99, 5);
    if (result < 0) {
        puts_stderr("OK (failed as expected)");
        passed++;
    } else {
        print_stderr("FAILED (should have failed, got ");
        print_num_stderr(result);
        puts_stderr(")");
        failed++;
    }

    /* Test 5: dup2 onto a number past the limit of 1024 should fail */
    print_stderr("Test 5: dup2(1, 2000) with invalid newfd... ");
    result = sys_dup2(STDOUT_FILENO, 2000);
    if (result < 0) {
        puts_stderr("OK (failed as expected)");
        passed++;
    } else {
        print_stderr("FAILED (should have failed, got ");
        print_num_stderr(result);
        puts_stderr(")");
        failed++;
    }

cleanup:
    /* Clean up test file */
    puts_stderr("");
    print_stderr("Cleanup: unlinking ");
    print_stderr(test_file);
    print_stderr("... ");
    if (sys_unlink(test_file) == 0) {
        puts_stderr("OK");
    } else {
        puts_stderr("(file may not exist)");
    }

    puts_stderr("");
    print_stderr("Results: ");
    print_num_stderr(passed);
    print_stderr(" passed, ");
    print_num_stderr(failed);
    puts_stderr(" failed");

    if (failed == 0) {
        puts_stderr("All dup2 syscalls passed!");
        sys_exit(0);
    } else {
        puts_stderr("Some tests failed!");
        sys_exit(1);
    }
}
