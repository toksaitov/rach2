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
#define SYS_RENAME 38
#define SYS_MKDIR  39
#define SYS_RMDIR  40

/* The errno values the negative cases name. */
#define ENOENT  2
#define EPERM   1
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
 * ELF entry point: run the rename tests and exit 0 only if all pass.
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

    /* Test 6b: A directory source is -EPERM. Its children reference its id, so
     * moving it would orphan them; the file system cannot perform the rename at
     * all, which is what EPERM says here. */
    print("Test 6b: rename(a directory, ...) == -EPERM... ");
    if (sys_mkdir("mvtest.dir", 0755) == 0) {
        result = sys_rename("mvtest.dir", "mvtest.dir2");
        if (result == -EPERM) {
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
