/**
 * Exercises the `unlink` syscall: create, verify, and delete files, then
 * confirm that deleting a missing file and the root directory both fail.
 *
 * Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

/* Syscall numbers (32-bit Linux / Rach2 compatible) */
#define SYS_EXIT   1
#define SYS_WRITE  4
#define SYS_OPEN   5
#define SYS_CLOSE  6
#define SYS_CREAT  8
#define SYS_UNLINK 10

/* Open flags */
#define O_RDONLY 0

/* File mode bits */
#define S_IRUSR 0400
#define S_IWUSR 0200

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

static void sys_exit(int status)
{
    __asm__ __volatile__ (
        "int $0x80"
        :
        : "a"(SYS_EXIT), "b"(status)
    );
    __builtin_unreachable();
}

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

static void print_num(int n)
{
    if (n >= 10) {
        char tens = '0' + (n / 10);
        sys_write(STDOUT_FILENO, &tens, 1);
    }
    char ones = '0' + (n % 10);
    sys_write(STDOUT_FILENO, &ones, 1);
}

/**
 * ELF entry point. Creates a file, confirms it exists, unlinks it, and
 * confirms it is gone, then checks that unlinking a missing path and the
 * root directory both fail.
 */
void _start(void)
{
    int fd;
    int result;
    int passed = 0;
    int failed = 0;

    puts("Unlink syscall test starting...");
    puts("");

    /* Test 1: Create a test file */
    print("Test 1: creat(\"rmtest.tmp\")... ");
    fd = sys_creat("rmtest.tmp", S_IRUSR | S_IWUSR);
    if (fd >= 0) {
        puts("OK");
        sys_close(fd);
        passed++;
    } else {
        puts("FAILED (can't create test file)");
        failed++;
        /* Can't continue without test file */
        goto results;
    }

    /* Test 2: Verify file exists */
    print("Test 2: open(\"rmtest.tmp\") to verify... ");
    fd = sys_open("rmtest.tmp", O_RDONLY);
    if (fd >= 0) {
        puts("OK (file exists)");
        sys_close(fd);
        passed++;
    } else {
        puts("FAILED (file not found)");
        failed++;
    }

    /* Test 3: Unlink the file */
    print("Test 3: unlink(\"rmtest.tmp\")... ");
    result = sys_unlink("rmtest.tmp");
    if (result == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 4: Verify file is gone */
    print("Test 4: open(\"rmtest.tmp\") should fail... ");
    fd = sys_open("rmtest.tmp", O_RDONLY);
    if (fd < 0) {
        puts("OK (file gone)");
        passed++;
    } else {
        puts("FAILED (file still exists)");
        sys_close(fd);
        failed++;
    }

    /* Test 5: Unlink nonexistent file (should fail) */
    print("Test 5: unlink(\"nonexistent.tmp\")... ");
    result = sys_unlink("nonexistent.tmp");
    if (result < 0) {
        puts("OK (failed as expected)");
        passed++;
    } else {
        puts("FAILED (should have failed)");
        failed++;
    }

    /* Test 6: Unlink "/" (should fail - it's a directory) */
    print("Test 6: unlink(\"/\")... ");
    result = sys_unlink("/");
    if (result < 0) {
        puts("OK (failed as expected - directory)");
        passed++;
    } else {
        puts("FAILED (should have failed)");
        failed++;
    }

    /* Test 7: Create and delete another file */
    print("Test 7: create, then unlink \"rmtest2.tmp\"... ");
    fd = sys_creat("rmtest2.tmp", S_IRUSR | S_IWUSR);
    if (fd >= 0) {
        sys_close(fd);
        result = sys_unlink("rmtest2.tmp");
        if (result == 0) {
            puts("OK");
            passed++;
        } else {
            puts("FAILED (unlink failed)");
            failed++;
        }
    } else {
        puts("FAILED (create failed)");
        failed++;
    }

results:
    puts("");
    print("Results: ");
    print_num(passed);
    print(" passed, ");
    print_num(failed);
    puts(" failed");

    if (failed == 0) {
        puts("All unlink syscalls passed!");
        sys_exit(0);
    } else {
        puts("Some tests failed!");
        sys_exit(1);
    }
}
