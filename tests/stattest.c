/**
 * stattest: the file stat calls, stat (106) and stat64 (195) on i386, and
 * stat (4), the only one, on x86-64, where the rows of the old narrow struct
 * are not built.
 *
 * Each is asked about an existing file, the root directory and a path that
 * does not exist, so that the success and the failure path are both run.
 * Then two files of one name in two directories have different inode
 * numbers, the root is inode 1 by every call and through `..`, and the
 * modification times of back-to-back writes and stamps do not go back and
 * keep a part below the tick (the time rows print "STATTIME OK" or
 * "STATTIME FAIL").
 *
 * Prints a line for each row and exits 0 when every row passed, 1 otherwise.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

typedef unsigned int mode_t;
typedef unsigned int ino_t;
typedef unsigned int dev_t;
typedef unsigned int nlink_t;
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef unsigned int blksize_t;
typedef unsigned int blkcnt_t;
typedef int time_t;
typedef long long off64_t;
typedef unsigned long long ino64_t;

#define STDOUT_FILENO 1

#define AT_FDCWD (-100)

/* File type bits from mode */
#define S_IFMT  0170000
#define S_IFREG 0100000
#define S_IFDIR 0040000

#if !defined __x86_64__
/* 32-bit Linux stat (used by ISH), filled by sys_stat (syscall 106). */
struct stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned short st_mode;
    unsigned short st_nlink;
    unsigned short st_uid;
    unsigned short st_gid;
    unsigned long st_rdev;
    unsigned long st_size;
    unsigned long st_blksize;
    unsigned long st_blocks;
    unsigned long st_atime;
    unsigned long st_atime_nsec;
    unsigned long st_mtime;
    unsigned long st_mtime_nsec;
    unsigned long st_ctime;
    unsigned long st_ctime_nsec;
    unsigned long __unused4;
    unsigned long __unused5;
};

/* 32-bit Linux stat64, filled by sys_stat64 (syscall 195). */
struct stat64 {
    unsigned long long st_dev;
    unsigned char __pad0[4];
    unsigned long __st_ino;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned long st_uid;
    unsigned long st_gid;
    unsigned long long st_rdev;
    unsigned char __pad3[4];
    long long st_size;
    unsigned long st_blksize;
    unsigned long long st_blocks;
    unsigned long st_atime;
    unsigned long st_atime_nsec;
    unsigned long st_mtime;
    unsigned long st_mtime_nsec;
    unsigned long st_ctime;
    unsigned long st_ctime_nsec;
    unsigned long long st_ino;
};

/* The 32-bit inode number stat64 has beside the 64-bit one. */
#define OLD_INO(s) ((s).__st_ino)
#define WIDE_CALL  "stat64 (syscall 195)"
#else
/* x86-64's stat, filled by stat (syscall 4), under the name the i386 rows
 * give the wide struct; it has no second inode number. */
struct stat64 {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned long st_nlink;
    unsigned int st_mode;
    unsigned int st_uid;
    unsigned int st_gid;
    unsigned int __pad0;
    unsigned long st_rdev;
    long st_size;
    long st_blksize;
    long st_blocks;
    unsigned long st_atime;
    unsigned long st_atime_nsec;
    unsigned long st_mtime;
    unsigned long st_mtime_nsec;
    unsigned long st_ctime;
    unsigned long st_ctime_nsec;
    long __unused[3];
};

#define OLD_INO(s) ((s).st_ino)
#define WIDE_CALL  "stat (syscall 4)"
#endif

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

#if !defined __x86_64__
/** i386's old stat(2), call 106, into the narrow struct. */
static int sys_stat(const char *path, struct stat *buf)
{
    return syscall2(SYS_OLDSTAT, (long)path, (long)buf);
}
#endif

/** Make the three-argument system call `nr`. */
static int sys_call3(int nr, long a, long b, long c)
{
    return syscall3(nr, a, b, c);
}

/** Make a four-argument system call, for utimensat and fstatat64. */
static int sys_call4(int nr, long a, long b, long c, long d)
{
    return syscall4(nr, a, b, c, d);
}

/** Make the directory `dir` and the file `file` inside it: 0, or -1 when a
 * call fails. */
static int make_in(const char *dir, const char *file)
{
    int fd;

    if (sys_call3(SYS_MKDIR, (long)dir, 0755, 0) < 0) {
        return -1;
    }
    fd = sys_call3(SYS_OPEN, (long)file, 0101, 0644); /* O_WRONLY|O_CREAT */
    if (fd < 0) {
        return -1;
    }
    return sys_call3(SYS_CLOSE, fd, 0, 0) < 0 ? -1 : 0;
}

/** SYS_STAT into the wide struct: stat64 on i386, stat on x86-64. */
static int sys_stat64(const char *path, struct stat64 *buf)
{
    return syscall2(SYS_STAT, (long)path, (long)buf);
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

/** Write `n` in decimal to standard output. */
static void print_num(unsigned int n)
{
    char buf[12];
    int i = 0;

    if (n == 0) {
        buf[i++] = '0';
    } else {
        while (n > 0) {
            buf[i++] = '0' + (n % 10);
            n /= 10;
        }
    }

    while (i > 0) {
        sys_write(STDOUT_FILENO, &buf[--i], 1);
    }
}

/** Write `m` in octal to standard output. */
static void print_octal(unsigned int m)
{
    char octal[12];
    int j = 0;

    if (m == 0) {
        sys_write(STDOUT_FILENO, "0", 1);
        return;
    }

    do {
        octal[j++] = '0' + (m & 7);
        m >>= 3;
    } while (m > 0);

    while (j > 0) {
        sys_write(STDOUT_FILENO, &octal[--j], 1);
    }
}

/**
 * Part C: two files of one name in two directories have different inode
 * numbers, by every stat call. A tool that finds two files sharing an inode
 * number, as `cp -a` does, copies them as hard links of one file, and KRFFS
 * keeps a file's on-disk id as the hash of its name alone, so the number it
 * reports is made from more. Counts the row in `*passed` or `*failed`.
 */
static void same_name_rows(int *passed, int *failed)
{
    puts("=== Part C: same-named files ===");
    print("Test C1: /statc1/k and /statc2/k have different inode numbers... ");
    if (make_in("/statc1", "/statc1/k") == 0 && make_in("/statc2", "/statc2/k") == 0) {
        struct stat64 one, two;
        int old_ok = 1;
#if !defined __x86_64__
        struct stat oneb, twob;

        old_ok = sys_stat("/statc1/k", &oneb) == 0 && sys_stat("/statc2/k", &twob) == 0 &&
                 oneb.st_ino != twob.st_ino;
#endif
        if (old_ok && sys_stat64("/statc1/k", &one) == 0 && sys_stat64("/statc2/k", &two) == 0 &&
            one.st_ino != two.st_ino) {
            puts("OK");
            (*passed)++;
        } else {
            puts("FAILED (one inode number for two files)");
            (*failed)++;
        }
    } else {
        puts("FAILED (the files could not be made)");
        (*failed)++;
    }
}

/**
 * The root is inode 1, a directory, through `/` and through `/statroot/..`,
 * by stat, stat64, fstat and fstatat, the 32-bit and 64-bit numbers alike.
 * The test image has no /tmp, so the row makes the directory it walks out
 * of. Counts each row in `*passed` or `*failed`.
 */
static void root_ino_rows(int *passed, int *failed)
{
    const char *paths[] = {"/", "/statroot/.."};
    unsigned int i;

    if (sys_call3(SYS_MKDIR, (long)"/statroot", 0755, 0) < 0) {
        puts("Root inode through stat and stat64: FAILED (/statroot could not be made)");
        (*failed)++;
        return;
    }
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        struct stat64 wide;
        int old_ok = 1;
#if !defined __x86_64__
        struct stat old;

        old_ok =
            sys_stat(paths[i], &old) == 0 && old.st_ino == 1 && (old.st_mode & S_IFMT) == S_IFDIR;
#endif
        print("Root inode through stat and stat64: ");
        print(paths[i]);
        if (old_ok && sys_stat64(paths[i], &wide) == 0 && wide.st_ino == 1 && OLD_INO(wide) == 1 &&
            (wide.st_mode & S_IFMT) == S_IFDIR) {
            puts(" OK");
            (*passed)++;
        } else {
            puts(" FAILED");
            (*failed)++;
        }
    }
    {
        struct stat64 byfd, byat;
        int fd = sys_call3(SYS_OPEN, (long)"/", 0, 0); /* open O_RDONLY */

        print("Root inode through fstat64 and fstatat64: ");
        if (fd >= 0 && sys_call3(SYS_FSTAT, fd, (long)&byfd, 0) == 0 &&
            sys_call4(SYS_FSTATAT, AT_FDCWD, (long)"/", (long)&byat, 0) == 0 && byfd.st_ino == 1 &&
            OLD_INO(byfd) == 1 && byat.st_ino == 1 && OLD_INO(byat) == 1 &&
            (byfd.st_mode & S_IFMT) == S_IFDIR && (byat.st_mode & S_IFMT) == S_IFDIR) {
            puts("OK");
            (*passed)++;
        } else {
            puts("FAILED");
            (*failed)++;
        }
        if (fd >= 0) {
            sys_call3(SYS_CLOSE, fd, 0, 0);
        }
    }
}

/** Whether the modification time of `a`, seconds and nanoseconds, is at or
 * after that of `b`. */
static int time_not_before(const struct stat64 *a, const struct stat64 *b)
{
    return a->st_mtime > b->st_mtime ||
           (a->st_mtime == b->st_mtime && a->st_mtime_nsec >= b->st_mtime_nsec);
}

/** Whether the modification time has a part below the 10 ms tick of at
 * least a microsecond. */
static int below_tick(const struct stat64 *s)
{
    return s->st_mtime_nsec % 10000000u >= 1000u;
}

/**
 * The time rows; 1 when they passed. KRFFS keeps nanoseconds and the clock
 * adds the timer's place within the tick, so stamps taken back to back never
 * go back and carry a part below the 10 ms tick, which a clock of whole
 * ticks never gives. They may repeat: a reading that would fall back, as
 * after interrupts were off for longer than a timer period, is given as the
 * last reading again (`rtc_realtime`). No delay separates the first two
 * writes or the fifty stamps. Explicit times keep their fractions, zero
 * included.
 */
static int time_rows(void)
{
    const char *paths[2] = {"/stattime-a", "/stattime-b"};
    struct stat64 previous, next;
    long times[4] = {1234567890, 0, 1234567890, 123456789};
    int fd[2];
    int i;
    int ok = 1;
    int fine = 0;

    fd[0] = sys_call3(SYS_OPEN, (long)paths[0], 0101, 0644);
    fd[1] = sys_call3(SYS_OPEN, (long)paths[1], 0101, 0644);
    if (fd[0] < 0 || fd[1] < 0) {
        ok = 0;
    } else {
        ok = sys_write(fd[0], "a", 1) == 1 && sys_write(fd[1], "b", 1) == 1 &&
             sys_stat64(paths[0], &previous) == 0 && sys_stat64(paths[1], &next) == 0 &&
             time_not_before(&next, &previous);
        fine = ok && (below_tick(&previous) || below_tick(&next));
        if (ok) {
            previous = next;
        }
        for (i = 0; ok && i < 50; i++) {
            ok = sys_call4(SYS_UTIMENSAT, -100, (long)paths[i % 2], 0, 0) == 0 &&
                 sys_stat64(paths[i % 2], &next) == 0 && time_not_before(&next, &previous);
            fine = fine || below_tick(&next);
            previous = next;
        }
        ok = ok && fine;
        if (ok) {
            ok = sys_call4(SYS_UTIMENSAT, -100, (long)paths[0], (long)times, 0) == 0 &&
                 sys_stat64(paths[0], &next) == 0 && next.st_atime == 1234567890u &&
                 next.st_atime_nsec == 0 && next.st_mtime == 1234567890u &&
                 next.st_mtime_nsec == 123456789u;
        }
    }
    for (i = 0; i < 2; i++) {
        if (fd[i] >= 0) {
            sys_call3(SYS_CLOSE, fd[i], 0, 0);
        }
        sys_call3(SYS_UNLINK, (long)paths[i], 0, 0);
    }
    puts(ok ? "STATTIME OK: two writes, fifty stamps, explicit fractions"
            : "STATTIME FAIL: two writes, fifty stamps, explicit fractions");
    return ok;
}

/**
 * Entry point: runs the stat64 (195) probes in part A, the stat (106) probes
 * in part B, then the inode and time rows, and exits 0 only if every row
 * passed. On x86-64 part A is stat (4) and there is no part B.
 */
PROBE_START void _start(void)
{
#if !defined __x86_64__
    struct stat st;
#endif
    struct stat64 st64;
    int result;
    int passed = 0;
    int failed = 0;

    puts("Stat syscall test starting...");
    puts("");

    /* Part A: the wide call, stat64 (195) on i386 and stat (4) on x86-64. */
    puts("=== Part A: " WIDE_CALL " ===");
    puts("");

    /* Test A1: stat64 existing file */
    print("Test A1: stat64(\"hello.txt\")... ");
    result = sys_stat64("hello.txt", &st64);
    if (result == 0) {
        puts("OK");
        print("        size=");
        print_num((unsigned int)st64.st_size);
        print(" bytes, mode=0");
        print_octal(st64.st_mode);
        print(", nlink=");
        print_num(st64.st_nlink);
        puts("");
        passed++;

        print("        type: ");
        if ((st64.st_mode & S_IFMT) == S_IFREG) {
            puts("regular file (OK)");
        } else {
            puts("not a regular file (unexpected)");
        }
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test A2: stat64 root directory */
    print("Test A2: stat64(\"/\")... ");
    result = sys_stat64("/", &st64);
    if (result == 0) {
        puts("OK");
        print("        mode=0");
        print_octal(st64.st_mode);
        print(", nlink=");
        print_num(st64.st_nlink);
        puts("");
        passed++;

        print("        type: ");
        if ((st64.st_mode & S_IFMT) == S_IFDIR) {
            puts("directory (OK)");
        } else {
            puts("not a directory (unexpected)");
        }
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test A3: stat64 nonexistent file */
    print("Test A3: stat64(\"nonexistent\")... ");
    result = sys_stat64("nonexistent", &st64);
    if (result < 0) {
        puts("OK (failed as expected)");
        passed++;
    } else {
        puts("FAILED (should have failed)");
        failed++;
    }

    puts("");

#if !defined __x86_64__
    /* Part B: i386's old stat (106). */
    puts("=== Part B: stat (syscall 106) ===");
    puts("");

    /* Test B1: stat existing file */
    print("Test B1: stat(\"hello.txt\")... ");
    result = sys_stat("hello.txt", &st);
    if (result == 0) {
        puts("OK");
        print("        size=");
        print_num(st.st_size);
        print(" bytes, mode=0");
        print_octal(st.st_mode);
        print(", nlink=");
        print_num(st.st_nlink);
        puts("");
        passed++;

        print("        type: ");
        if ((st.st_mode & S_IFMT) == S_IFREG) {
            puts("regular file (OK)");
        } else {
            puts("not a regular file (unexpected)");
        }
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test B2: stat root directory */
    print("Test B2: stat(\"/\")... ");
    result = sys_stat("/", &st);
    if (result == 0) {
        puts("OK");
        print("        mode=0");
        print_octal(st.st_mode);
        print(", nlink=");
        print_num(st.st_nlink);
        puts("");
        passed++;

        print("        type: ");
        if ((st.st_mode & S_IFMT) == S_IFDIR) {
            puts("directory (OK)");
        } else {
            puts("not a directory (unexpected)");
        }
    } else {
        puts("FAILED");
        failed++;
    }

    /* Test B3: stat nonexistent file */
    print("Test B3: stat(\"nonexistent\")... ");
    result = sys_stat("nonexistent", &st);
    if (result < 0) {
        puts("OK (failed as expected)");
        passed++;
    } else {
        puts("FAILED (should have failed)");
        failed++;
    }

    /* Test B4: stat with leading slash */
    print("Test B4: stat(\"/hello.txt\")... ");
    result = sys_stat("/hello.txt", &st);
    if (result == 0) {
        puts("OK");
        print("        size=");
        print_num(st.st_size);
        print(" bytes, nlink=");
        print_num(st.st_nlink);
        puts("");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

    puts("");
#endif

    same_name_rows(&passed, &failed);
    root_ino_rows(&passed, &failed);
    if (time_rows()) {
        passed++;
    } else {
        failed++;
    }

    puts("");
    print("Results: ");
    print_num(passed);
    print(" passed, ");
    print_num(failed);
    puts(" failed");

    if (failed == 0) {
        puts("All stat syscalls passed!");
        sys_exit(0);
    } else {
        puts("Some tests failed!");
        sys_exit(1);
    }
}
