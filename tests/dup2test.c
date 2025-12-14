/*
 * dup2test.c - File descriptor duplication test program for Rach2 OS
 *
 * Tests sys_dup2 syscall for I/O redirection by:
 * 1. Creating a file
 * 2. Duplicating file fd to stdout (fd 1)
 * 3. Writing via stdout (should go to file)
 * 4. Reading file back to verify content
 *
 * Compile with:
 *   i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 -o dup2test dup2test.c
 */

typedef unsigned int size_t;

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* Syscall numbers (32-bit Linux / Rach2 compatible) */
#define SYS_EXIT  1
#define SYS_READ  3
#define SYS_WRITE 4
#define SYS_OPEN  5
#define SYS_CLOSE 6
#define SYS_CREAT 8
#define SYS_UNLINK 10
#define SYS_DUP2  63

/* Open flags */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

/*
 * sys_write - Write to a file descriptor
 */
static int sys_write(int fd, const void *buf, size_t count)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count)
        : "memory"
    );
    return result;
}

/*
 * sys_read - Read from a file descriptor
 */
static int sys_read(int fd, void *buf, size_t count)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_READ), "b"(fd), "c"(buf), "d"(count)
        : "memory"
    );
    return result;
}

/*
 * sys_open - Open a file
 */
static int sys_open(const char *path, int flags)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_OPEN), "b"(path), "c"(flags)
        : "memory"
    );
    return result;
}

/*
 * sys_close - Close a file descriptor
 */
static int sys_close(int fd)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_CLOSE), "b"(fd)
    );
    return result;
}

/*
 * sys_creat - Create a file
 */
static int sys_creat(const char *path, unsigned int mode)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_CREAT), "b"(path), "c"(mode)
        : "memory"
    );
    return result;
}

/*
 * sys_unlink - Delete a file
 */
static int sys_unlink(const char *path)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_UNLINK), "b"(path)
        : "memory"
    );
    return result;
}

/*
 * sys_dup2 - Duplicate file descriptor
 */
static int sys_dup2(int oldfd, int newfd)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_DUP2), "b"(oldfd), "c"(newfd)
        : "memory"
    );
    return result;
}

/*
 * sys_exit - Terminate the process
 */
static void sys_exit(int status)
{
    __asm__ __volatile__ (
        "int $0x80"
        :
        : "a"(SYS_EXIT), "b"(status)
    );
    __builtin_unreachable();
}

/*
 * strlen - Calculate string length
 */
static size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

/*
 * strcmp - Compare two strings
 * Returns 0 if equal
 */
static int strcmp(const char *s1, const char *s2)
{
    while (*s1 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

/*
 * strncmp - Compare first n characters
 */
static int strncmp(const char *s1, const char *s2, size_t n)
{
    while (n > 0 && *s1 && *s1 == *s2) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

/*
 * Macros for output to stderr (fd 2 is not affected by our dup2 tests)
 */
#define puts_stderr(s) do { \
    sys_write(STDERR_FILENO, s, strlen(s)); \
    sys_write(STDERR_FILENO, "\n", 1); \
} while(0)

#define print_stderr(s) sys_write(STDERR_FILENO, s, strlen(s))

/*
 * print_num_stderr - Print a decimal number to stderr
 */
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

/*
 * _start - ELF entry point
 *
 * Tests dup2 for I/O redirection:
 * 1. dup2(fd, fd) should return fd unchanged
 * 2. Create file, dup2 to stdout, write via stdout, verify in file
 * 3. dup2 to stdin, read via stdin from file
 * 4. Test error cases (invalid fd)
 */
void _start(void)
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

    /* Save current stdout by dup2 to a high fd */
    /* Note: In Rach2, we can't easily save/restore stdout, so we'll just
     * verify the write went to the file by reading it back */

    /* Redirect stdout to our file */
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

    /* Test 5: dup2 with invalid newfd should fail */
    print_stderr("Test 5: dup2(1, 999) with invalid newfd... ");
    result = sys_dup2(STDOUT_FILENO, 999);
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
