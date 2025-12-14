/*
 * touchtest.c - Create file and update timestamp syscall test for Rach2 OS
 *
 * Tests sys_creat and sys_utimensat syscalls.
 *
 * Compile with:
 *   i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 -o touchtest touchtest.c
 */

typedef unsigned int size_t;
typedef long time_t;

#define STDOUT_FILENO 1

/* Syscall numbers (32-bit Linux / Rach2 compatible) */
#define SYS_EXIT      1
#define SYS_WRITE     4
#define SYS_OPEN      5
#define SYS_CLOSE     6
#define SYS_CREAT     8
#define SYS_UNLINK    10
#define SYS_STAT64    195
#define SYS_UTIMENSAT 320

/* Special value for dirfd meaning "use pathname relative to cwd" */
#define AT_FDCWD (-100)

/* Timespec structure for utimensat */
struct timespec {
    time_t tv_sec;   /* seconds */
    long   tv_nsec;  /* nanoseconds */
};

/* Special nsec value: set to current time */
#define UTIME_NOW  ((1L << 30) - 1L)
/* Special nsec value: leave unchanged */
#define UTIME_OMIT ((1L << 30) - 2L)

/* Open flags */
#define O_RDONLY 0

/* File mode bits */
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IRGRP 0040
#define S_IROTH 0004

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
 * Returns: file descriptor on success, -1 on error
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
 * sys_utimensat - Update file timestamps
 * dirfd: AT_FDCWD for relative to cwd, or an open directory fd
 * pathname: path to file
 * times: array of 2 timespec structs [atime, mtime], or NULL for current time
 * flags: 0 or AT_SYMLINK_NOFOLLOW
 * Returns: 0 on success, -1 on error
 */
static int sys_utimensat(int dirfd, const char *pathname,
                         const struct timespec *times, int flags)
{
    int result;
    __asm__ __volatile__ (
        "int $0x80"
        : "=a"(result)
        : "0"(SYS_UTIMENSAT), "b"(dirfd), "c"(pathname), "d"(times), "S"(flags)
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
 * Tests creat and utimensat syscalls:
 * 1. Create a new file "touchtest.tmp"
 * 2. Verify file exists (by opening it)
 * 3. Create file again (should succeed - file already exists)
 * 4. Update timestamps with utimensat (NULL = current time)
 * 5. Update timestamps with specific values
 * 6. utimensat on nonexistent file (should fail)
 * 7-9. Clean up
 */
void _start(void)
{
    int fd;
    int result;
    int passed = 0;
    int failed = 0;
    struct timespec times[2];

    puts("Creat/utimensat syscall test starting...");
    puts("");

    /* Test 1: Create a new file */
    print("Test 1: creat(\"touchtest.tmp\")... ");
    fd = sys_creat("touchtest.tmp", S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (fd >= 0) {
        puts("OK");
        sys_close(fd);
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 2: Verify file exists by opening it */
    print("Test 2: open(\"touchtest.tmp\") to verify... ");
    fd = sys_open("touchtest.tmp", O_RDONLY);
    if (fd >= 0) {
        puts("OK (file exists)");
        sys_close(fd);
        passed++;
    } else {
        puts("FAILED (file not found)");
        failed++;
    }

    /* Test 3: Create same file again (should succeed) */
    print("Test 3: creat(\"touchtest.tmp\") again... ");
    fd = sys_creat("touchtest.tmp", S_IRUSR | S_IWUSR);
    if (fd >= 0) {
        puts("OK (existing file)");
        sys_close(fd);
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 4: Create file with different name */
    print("Test 4: creat(\"touchtest2.tmp\")... ");
    fd = sys_creat("touchtest2.tmp", S_IRUSR | S_IWUSR);
    if (fd >= 0) {
        puts("OK");
        sys_close(fd);
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 5: utimensat with NULL times (set to current time) */
    print("Test 5: utimensat(\"touchtest.tmp\", NULL)... ");
    result = sys_utimensat(AT_FDCWD, "touchtest.tmp", (void *)0, 0);
    if (result == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 6: utimensat with specific times */
    print("Test 6: utimensat(\"touchtest.tmp\", {1234, 5678})... ");
    times[0].tv_sec = 1234;  /* atime */
    times[0].tv_nsec = 0;
    times[1].tv_sec = 5678;  /* mtime */
    times[1].tv_nsec = 0;
    result = sys_utimensat(AT_FDCWD, "touchtest.tmp", times, 0);
    if (result == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 7: utimensat on nonexistent file (should fail) */
    print("Test 7: utimensat(\"nonexistent\", NULL)... ");
    result = sys_utimensat(AT_FDCWD, "nonexistent", (void *)0, 0);
    if (result < 0) {
        puts("OK (failed as expected)");
        passed++;
    } else {
        puts("FAILED (should have failed)");
        failed++;
    }

    /* Test 8: Clean up - unlink first file */
    print("Test 8: unlink(\"touchtest.tmp\")... ");
    result = sys_unlink("touchtest.tmp");
    if (result == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test 9: Verify first file is gone */
    print("Test 9: open(\"touchtest.tmp\") should fail... ");
    fd = sys_open("touchtest.tmp", O_RDONLY);
    if (fd < 0) {
        puts("OK (file gone)");
        passed++;
    } else {
        puts("FAILED (file still exists)");
        sys_close(fd);
        failed++;
    }

    /* Test 10: Clean up second file */
    print("Test 10: unlink(\"touchtest2.tmp\")... ");
    result = sys_unlink("touchtest2.tmp");
    if (result == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    puts("");
    print("Results: ");
    print_num(passed);
    print(" passed, ");
    print_num(failed);
    puts(" failed");

    if (failed == 0) {
        puts("All creat/utimensat syscalls passed!");
        sys_exit(0);
    } else {
        puts("Some tests failed!");
        sys_exit(1);
    }
}
