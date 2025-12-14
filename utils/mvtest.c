/*
 * mvtest.c - Rename file syscall test program for Rach2 OS
 *
 * Tests sys_rename syscall by renaming files.
 *
 * Compile with:
 *   i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 -o mvtest mvtest.c
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
#define SYS_RENAME 38

/* Open flags */
#define O_RDONLY 0

/* File mode bits */
#define S_IRUSR 0400
#define S_IWUSR 0200

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
 * sys_creat - Create a new file
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
 * sys_rename - Rename a file
 * Returns: 0 on success, -1 on error
 */
static int sys_rename(const char *oldpath, const char *newpath)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_RENAME), "b"(oldpath), "c"(newpath)
        : "memory"
    );
    return result;
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
 * puts - Print string followed by newline
 */
static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

/*
 * print - Print string without newline
 */
static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

/*
 * print_num - Print a number (0-99)
 */
static void print_num(int n)
{
    if (n >= 10) {
        char tens = '0' + (n / 10);
        sys_write(STDOUT_FILENO, &tens, 1);
    }
    char ones = '0' + (n % 10);
    sys_write(STDOUT_FILENO, &ones, 1);
}

/*
 * _start - ELF entry point
 *
 * Tests rename syscall:
 * 1. Create a test file "mvtest_old.tmp"
 * 2. Rename it to "mvtest_new.tmp"
 * 3. Verify old name is gone
 * 4. Verify new name exists
 * 5. Try to rename nonexistent file (should fail)
 * 6. Clean up
 */
void _start(void)
{
    int fd;
    int result;
    int passed = 0;
    int failed = 0;

    puts("Rename syscall test starting...");
    puts("");

    /* Test 1: Create a test file */
    print("Test 1: creat(\"mvtest_old.tmp\")... ");
    fd = sys_creat("mvtest_old.tmp", S_IRUSR | S_IWUSR);
    if (fd >= 0) {
        puts("OK");
        sys_close(fd);
        passed++;
    } else {
        puts("FAILED (can't create test file)");
        failed++;
        goto results;
    }

    /* Test 2: Verify source file exists */
    print("Test 2: open(\"mvtest_old.tmp\") to verify... ");
    fd = sys_open("mvtest_old.tmp", O_RDONLY);
    if (fd >= 0) {
        puts("OK (file exists)");
        sys_close(fd);
        passed++;
    } else {
        puts("FAILED (file not found)");
        failed++;
    }

    /* Test 3: Rename the file */
    print("Test 3: rename(\"mvtest_old.tmp\", \"mvtest_new.tmp\")... ");
    result = sys_rename("mvtest_old.tmp", "mvtest_new.tmp");
    if (result == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 4: Verify old name is gone */
    print("Test 4: open(\"mvtest_old.tmp\") should fail... ");
    fd = sys_open("mvtest_old.tmp", O_RDONLY);
    if (fd < 0) {
        puts("OK (old name gone)");
        passed++;
    } else {
        puts("FAILED (old name still exists)");
        sys_close(fd);
        failed++;
    }

    /* Test 5: Verify new name exists */
    print("Test 5: open(\"mvtest_new.tmp\")... ");
    fd = sys_open("mvtest_new.tmp", O_RDONLY);
    if (fd >= 0) {
        puts("OK (new name exists)");
        sys_close(fd);
        passed++;
    } else {
        puts("FAILED (new name not found)");
        failed++;
    }

    /* Test 6: Try to rename nonexistent file (should fail) */
    print("Test 6: rename(\"nonexistent\", \"other\")... ");
    result = sys_rename("nonexistent", "other");
    if (result < 0) {
        puts("OK (failed as expected)");
        passed++;
    } else {
        puts("FAILED (should have failed)");
        /* Clean up the accidentally created file */
        sys_unlink("other");
        failed++;
    }

    /* Test 7: Rename to overwrite existing file */
    print("Test 7: create \"mvtest_dest.tmp\"... ");
    fd = sys_creat("mvtest_dest.tmp", S_IRUSR | S_IWUSR);
    if (fd >= 0) {
        sys_close(fd);
        print("OK, rename to overwrite... ");
        result = sys_rename("mvtest_new.tmp", "mvtest_dest.tmp");
        if (result == 0) {
            puts("OK");
            passed++;
            /* Clean up */
            sys_unlink("mvtest_dest.tmp");
        } else {
            puts("FAILED");
            failed++;
            /* Clean up both files */
            sys_unlink("mvtest_new.tmp");
            sys_unlink("mvtest_dest.tmp");
        }
    } else {
        puts("FAILED (can't create dest file)");
        failed++;
        /* Clean up source file */
        sys_unlink("mvtest_new.tmp");
    }

results:
    puts("");
    print("Results: ");
    print_num(passed);
    print(" passed, ");
    print_num(failed);
    puts(" failed");

    if (failed == 0) {
        puts("All rename syscalls passed!");
        sys_exit(0);
    } else {
        puts("Some tests failed!");
        sys_exit(1);
    }
}
