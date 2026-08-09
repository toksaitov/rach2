/**
 * Exercises the `unlink` syscall: create, verify, and delete files, then
 * confirm that deleting a missing file, a directory, and the root all fail with
 * the errno POSIX names for each.
 *
 * The exact errno is the point of the negative cases, not the sign. The file
 * system reports every refusal as a bare -1, which userspace decodes as EPERM,
 * so a test that accepted any negative return could not tell "no such file"
 * from "is a directory" from a permission denial.
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
#define SYS_MKDIR  39
#define SYS_RMDIR  40

/* The errno values the negative cases name. */
#define ENOENT  2
#define EISDIR 21
#define ENOTDIR 20

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

static int sys_mkdir(const char *path, int mode)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_MKDIR), "b"(path), "c"(mode)
        : "memory"
    );
    return result;
}

static int sys_rmdir(const char *path)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_RMDIR), "b"(path)
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

    /* Test 5: Unlink nonexistent file: -ENOENT, not a blanket failure */
    print("Test 5: unlink(\"nonexistent.tmp\") == -ENOENT... ");
    result = sys_unlink("nonexistent.tmp");
    if (result == -ENOENT) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED (wrong errno)");
        failed++;
    }

    /* Test 6: Unlink "/": -EISDIR. The root has no type bits in its mode, so it
     * is refused by identity rather than by KRFFS_IS_DIR. */
    print("Test 6: unlink(\"/\") == -EISDIR... ");
    result = sys_unlink("/");
    if (result == -EISDIR) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED (wrong errno)");
        failed++;
    }

    /* Test 6b: Unlink an ordinary directory: -EISDIR, which is what tells a
     * caller to use rmdir. Its own fixture, created and destroyed here. */
    print("Test 6b: unlink(a directory) == -EISDIR... ");
    if (sys_mkdir("rmtest.dir", 0755) == 0) {
        result = sys_unlink("rmtest.dir");
        if (result == -EISDIR) {
            puts("OK");
            passed++;
        } else {
            puts("FAILED (wrong errno)");
            failed++;
        }
        sys_rmdir("rmtest.dir");
    } else {
        puts("FAILED (mkdir fixture failed)");
        failed++;
    }

    /* Test 6c: A plain file in the middle of the path is -ENOTDIR, not
     * -ENOENT. The resolver reports the two separately and unlink must not
     * flatten them. */
    print("Test 6c: unlink(\"rmtest.plain/child\") == -ENOTDIR... ");
    fd = sys_creat("rmtest.plain", S_IRUSR | S_IWUSR);
    if (fd >= 0) {
        sys_close(fd);
        result = sys_unlink("rmtest.plain/child");
        if (result == -ENOTDIR) {
            puts("OK");
            passed++;
        } else {
            puts("FAILED (wrong errno)");
            failed++;
        }
        sys_unlink("rmtest.plain");
    } else {
        puts("FAILED (creat fixture failed)");
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
