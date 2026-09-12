/**
 * Exercises the rename syscall: rename a file, confirm the old name is gone
 * and the new name resolves, reject renaming a missing source, and overwrite an
 * existing destination.
 *
 * The negative cases assert the exact errno, not the sign. The file system
 * reports every refusal as a bare -1, which userspace decodes as EPERM whatever
 * went wrong, so the two directory cases are the ones that matter: a
 * directory SOURCE is -EPERM, the errno POSIX reserves for a rename the file
 * system cannot perform, while a directory DESTINATION under a plain source is
 * -EISDIR. A test that accepted any negative return could not tell them apart.
 * A directory source is refused with -EPERM on KRFFS, whose children name
 * their parent by id, and performed on ext2, which rewrites `..`; the probe
 * reads the root's type from `/proc/mounts`, prints it, and fails the row on
 * a root that is neither.
 *
 * Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

/* Syscall numbers (32-bit Linux / Rach2 compatible) */
#define SYS_EXIT   1
#define SYS_WRITE  4
#define SYS_READ   3
#define SYS_OPEN   5
#define SYS_CLOSE  6
#define SYS_CREAT  8
#define SYS_UNLINK 10
#define SYS_RENAME 38
#define SYS_MKDIR  39
#define SYS_RMDIR  40

/* The errno values the negative cases name. */
#define ENOENT 2
#define EPERM  1
#define EISDIR 21
#define EINVAL 22

/* Open flags */
#define O_RDONLY 0

/* File mode bits */
#define S_IRUSR 0400
#define S_IWUSR 0200

static int sys_write(int fd, const void *buf, size_t count)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count)
                         : "memory");
    return result;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
    __builtin_unreachable();
}

static int sys_creat(const char *path, unsigned int mode)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(SYS_CREAT), "b"(path), "c"(mode)
                         : "memory");
    return result;
}

static int sys_open(const char *path, int flags)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(SYS_OPEN), "b"(path), "c"(flags)
                         : "memory");
    return result;
}

static int sys_close(int fd)
{
    int result;
    __asm__ __volatile__("int $0x80" : "=a"(result) : "0"(SYS_CLOSE), "b"(fd));
    return result;
}

static int sys_unlink(const char *path)
{
    int result;
    __asm__ __volatile__("int $0x80" : "=a"(result) : "0"(SYS_UNLINK), "b"(path) : "memory");
    return result;
}

static int sys_mkdir(const char *path, int mode)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(SYS_MKDIR), "b"(path), "c"(mode)
                         : "memory");
    return result;
}

static int sys_rmdir(const char *path)
{
    int result;
    __asm__ __volatile__("int $0x80" : "=a"(result) : "0"(SYS_RMDIR), "b"(path) : "memory");
    return result;
}

static int sys_rename(const char *oldpath, const char *newpath)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(SYS_RENAME), "b"(oldpath), "c"(newpath)
                         : "memory");
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
/* Whether the root under the probe is ext2, read from the mount table rather
 * than assumed: the two roots answer a directory's rename differently and both
 * answers are right for their file system. */
static int root_is_ext2(void)
{
    static char mounts[2048];
    int fd = sys_open("/proc/mounts", O_RDONLY);
    int n, i;

    if (fd < 0) {
        return -1;
    }
    __asm__ __volatile__("int $0x80"
                         : "=a"(n)
                         : "0"(SYS_READ), "b"(fd), "c"(mounts), "d"(sizeof(mounts) - 1)
                         : "memory");
    sys_close(fd);
    if (n <= 0) {
        return -1;
    }
    mounts[n] = 0;
    /* Each line is "device mountpoint type options 0 0"; the root's is the one
       whose mount point is a bare slash, and the field after it is the type,
       which is matched whole. A type this probe does not know is -1, which
       ends it: the answers below are one file system's or the other's. */
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
 * ELF entry point: run the rename tests and exit 0 only if all pass.
 */
void _start(void)
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

    /* Test 6: Rename a missing source: -ENOENT */
    print("Test 6: rename(\"nonexistent\", \"other\") == -ENOENT... ");
    result = sys_rename("nonexistent", "other");
    if (result == -ENOENT) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED (wrong errno)");
        /* Clean up the accidentally created file */
        sys_unlink("other");
        failed++;
    }

    /* Test 6b: what a directory's rename answers, which the file system under
     * the probe decides. On the boot root a directory's children reference its
     * id, so moving it would orphan them and KRFFS refuses with EPERM; ext2
     * rewrites `..` and moves it, as Linux does. The type was read at the top
     * and printed; a root that is neither fails this row rather than taking
     * one of the two answers by default. */
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

    /* Test 6c: A directory destination under a plain source is -EISDIR, the
     * other half of the pair, and the case that separates the two errnos. */
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

    /* Test 6d: The root as a source is -EINVAL, not -ENOENT: the root exists,
     * so "no such file" would be false, and canonicalizing "/" leaves no
     * component to name. */
    print("Test 6d: rename(\"/\", \"mvtest.x\") == -EINVAL... ");
    result = sys_rename("/", "mvtest.x");
    if (result == -EINVAL) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED (wrong errno)");
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
