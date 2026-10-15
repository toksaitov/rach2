/**
 * Exercises the rename syscall: rename a file, confirm the old name is gone
 * and the new name resolves, reject renaming a missing source, refuse the root
 * as a source with EBUSY, as Linux does, and overwrite an existing
 * destination.
 *
 * The negative cases assert the exact errno, not the sign. The KRFFS core
 * answers every refusal of a rename with a bare -1, so the system call tells
 * the cases apart before it calls the core (syscall_fs.c), and the two
 * directory cases are the ones that matter: a directory as the source is
 * -EPERM, the errno POSIX reserves for a rename the file system cannot
 * perform, while a directory as the destination under a plain source is
 * -EISDIR. A test that accepted any negative return could not tell them
 * apart. A directory source is refused with -EPERM on KRFFS, whose children
 * name their parent by id, and performed on ext2, which rewrites `..`; the
 * probe reads the root's type from `/proc/mounts`, prints it, and fails the
 * row on a root that is neither.
 *
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

/* The errno values the negative cases name. */
#define ENOENT 2
#define EPERM  1
#define EBUSY  16
#define EISDIR 21

/* Open flags */
#define O_RDONLY 0

/* File mode bits */
#define S_IRUSR 0400
#define S_IWUSR 0200

/** write(2): the bytes written, or a negated errno. */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** exit(2) with `status`; does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** creat(2): a descriptor, or a negated errno. */
static int sys_creat(const char *path, unsigned int mode)
{
    return syscall2(SYS_CREAT, (long)path, (long)mode);
}

/** open(2) with no mode: a descriptor, or a negated errno. */
static int sys_open(const char *path, int flags)
{
    return syscall2(SYS_OPEN, (long)path, (long)flags);
}

/** close(2): 0, or a negated errno. */
static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

/** unlink(2): 0, or a negated errno. */
static int sys_unlink(const char *path)
{
    return syscall1(SYS_UNLINK, (long)path);
}

/** mkdir(2): 0, or a negated errno. */
static int sys_mkdir(const char *path, int mode)
{
    return syscall2(SYS_MKDIR, (long)path, (long)mode);
}

/** rmdir(2): 0, or a negated errno. */
static int sys_rmdir(const char *path)
{
    return syscall1(SYS_RMDIR, (long)path);
}

/** rename(2): 0, or a negated errno. */
static int sys_rename(const char *oldpath, const char *newpath)
{
    return syscall2(SYS_RENAME, (long)oldpath, (long)newpath);
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

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

/**
 * Whether the root under the probe is ext2, read from the mount table rather
 * than assumed: the two roots answer a directory's rename differently and both
 * answers are right for their file system. 1 for ext2 and 0 for KRFFS, the
 * type of the line whose mount point is a bare slash, matched whole; -1 for
 * another type, which fails the directory row, or when /proc/mounts cannot be
 * read.
 */
static int root_is_ext2(void)
{
    static char mounts[2048];
    int fd = sys_open("/proc/mounts", O_RDONLY);
    int n, i;

    if (fd < 0) {
        return -1;
    }
    n = syscall3(SYS_READ, (long)fd, (long)mounts, (long)(sizeof(mounts) - 1));
    sys_close(fd);
    if (n <= 0) {
        return -1;
    }
    mounts[n] = 0;
    /* Each line is "device mountpoint type options 0 0". */
    for (i = 0; i < n; i++) {
        if (mounts[i] == ' ' && mounts[i + 1] == '/' && mounts[i + 2] == ' ') {
            const char *t = &mounts[i + 3];

            if (t[0] == 'e' && t[1] == 'x' && t[2] == 't' && t[3] == '2' && t[4] == ' ') {
                return 1;
            }
            if (t[0] == 'k' && t[1] == 'r' && t[2] == 'f' && t[3] == 'f' && t[4] == 's' &&
                t[5] == ' ') {
                return 0;
            }
            return -1;
        }
    }
    return -1;
}

/** Write `s` to standard output. */
static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

/** Write `n`, from 0 to 99, in decimal. */
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
 * ELF entry point: run the rename tests and exit 0 only if all pass. A row
 * removes the names it made, and a failed row the names a wrong answer may
 * have made, such as the destination of a rename whose source was missing.
 */
PROBE_START void _start(void)
{
    int fd;
    int result;
    int root = root_is_ext2();
    int passed = 0;
    int failed = 0;

    puts("Rename syscall test starting...");
    if (root == 1) {
        puts("mvtest: the root is ext2");
    } else if (root == 0) {
        puts("mvtest: the root is krffs");
    } else {
        puts("mvtest: the root is neither krffs nor ext2");
    }
    puts("");

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

    print("Test 3: rename(\"mvtest_old.tmp\", \"mvtest_new.tmp\")... ");
    result = sys_rename("mvtest_old.tmp", "mvtest_new.tmp");
    if (result == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

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

    print("Test 6: rename(\"nonexistent\", \"other\") == -ENOENT... ");
    result = sys_rename("nonexistent", "other");
    if (result == -ENOENT) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED (wrong errno)");
        sys_unlink("other");
        failed++;
    }

    print("Test 6b: rename of a directory, the answer this root owes... ");
    if (root == -1) {
        puts("FAILED (the root is neither krffs nor ext2)");
        failed++;
    } else if (sys_mkdir("mvtest.dir", 0755) == 0) {
        int dir_rename_refused = !root;

        result = sys_rename("mvtest.dir", "mvtest.dir2");
        if (dir_rename_refused ? (result == -EPERM) : (result == 0)) {
            puts("OK");
            passed++;
        } else {
            puts("FAILED (wrong errno)");
            failed++;
        }
        sys_rmdir("mvtest.dir");
        sys_rmdir("mvtest.dir2");
    } else {
        puts("FAILED (mkdir fixture failed)");
        failed++;
    }

    print("Test 6c: rename(file, a directory) == -EISDIR... ");
    if (sys_mkdir("mvtest.dst", 0755) == 0) {
        fd = sys_creat("mvtest.src", S_IRUSR | S_IWUSR);
        if (fd >= 0) {
            sys_close(fd);
            result = sys_rename("mvtest.src", "mvtest.dst");
            if (result == -EISDIR) {
                puts("OK");
                passed++;
            } else {
                puts("FAILED (wrong errno)");
                failed++;
            }
            sys_unlink("mvtest.src");
        } else {
            puts("FAILED (creat fixture failed)");
            failed++;
        }
        sys_rmdir("mvtest.dst");
    } else {
        puts("FAILED (mkdir fixture failed)");
        failed++;
    }

    print("Test 6d: rename(\"/\", \"mvtest.x\") == -EBUSY... ");
    result = sys_rename("/", "mvtest.x");
    if (result == -EBUSY) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED (wrong errno)");
        failed++;
    }

    print("Test 7: create \"mvtest_dest.tmp\"... ");
    fd = sys_creat("mvtest_dest.tmp", S_IRUSR | S_IWUSR);
    if (fd >= 0) {
        sys_close(fd);
        print("OK, rename to overwrite... ");
        result = sys_rename("mvtest_new.tmp", "mvtest_dest.tmp");
        if (result == 0) {
            puts("OK");
            passed++;
            sys_unlink("mvtest_dest.tmp");
        } else {
            puts("FAILED");
            failed++;
            sys_unlink("mvtest_new.tmp");
            sys_unlink("mvtest_dest.tmp");
        }
    } else {
        puts("FAILED (can't create dest file)");
        failed++;
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
