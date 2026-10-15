/**
 * touchtest: file creation and timestamp updates through `creat` and
 * `utimensat`, as `touch` makes them.
 *
 * Prints a line for each of the ten tests and exits 0 when every one passed,
 * 1 otherwise. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

typedef long time_t;

#define STDOUT_FILENO 1

/* The dirfd that resolves a relative path against the working directory. */
#define AT_FDCWD (-100)

/* The times utimensat takes. */
struct timespec {
    time_t tv_sec;
    long tv_nsec;
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

/** creat(2). */
static int sys_creat(const char *path, unsigned int mode)
{
    return syscall2(SYS_CREAT, (long)path, (long)mode);
}

/** open(2) with no mode. */
static int sys_open(const char *path, int flags)
{
    return syscall2(SYS_OPEN, (long)path, (long)flags);
}

/** close(2). */
static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

/** unlink(2). */
static int sys_unlink(const char *path)
{
    return syscall1(SYS_UNLINK, (long)path);
}

/**
 * utimensat(2): set the access and modification times of `pathname`,
 * resolved against `dirfd` or, for AT_FDCWD, the working directory, to
 * `times`, [atime, mtime], or to the time now when `times` is NULL. `flags`
 * is 0 or AT_SYMLINK_NOFOLLOW.
 */
static int sys_utimensat(int dirfd, const char *pathname, const struct timespec *times, int flags)
{
    return syscall4(SYS_UTIMENSAT, (long)dirfd, (long)pathname, (long)times, (long)flags);
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

/** Write `n`, 0 to 99, in decimal to standard output. */
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
 * The probe's entry: the creat, utimensat and unlink tests in order, then
 * the count of each verdict; exits 0 when every test passed, 1 otherwise.
 */
PROBE_START void _start(void)
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
    times[0].tv_sec = 1234; /* atime */
    times[0].tv_nsec = 0;
    times[1].tv_sec = 5678; /* mtime */
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

    /* Test 8: unlink the first file. */
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

    /* Test 10: unlink the second file. */
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
