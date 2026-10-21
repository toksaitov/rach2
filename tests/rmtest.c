/**
 * Exercises the `unlink` syscall: create, verify, and delete files, then
 * confirm that deleting a missing file, a directory, the root and a path
 * through a file all fail with the errno POSIX names for each.
 *
 * The exact errno is the point of the negative cases, not the sign. The KRFFS
 * core answers every refusal with a bare -1, so the system call tells the
 * cases apart before it calls the core (`unlink_named` in syscall_fs.c), and
 * a test that accepted any negative return could not tell "no such file"
 * from "is a directory" from a permission denial.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, an assertion for each test: `create`, `exists`, `unlink`, `gone`,
 * `enoent`, `eisdir-root`, `eisdir-dir`, `enotdir` and `create-unlink`; a
 * failed first creation leaves the others not run. Freestanding; direct
 * system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define STDOUT_FILENO 1

/* The errno values the negative cases name. */
#define ENOENT  2
#define EISDIR  21
#define ENOTDIR 20

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

/* The assertions after `create`, which a failed creation leaves not run. */
static const char *const after_create[] = {"exists",      "unlink",     "gone",    "enoent",
                                           "eisdir-root", "eisdir-dir", "enotdir", "create-unlink"};

/** Report the assertion `create` failed and those after it not run. */
static void report_no_file(void)
{
    unsigned int i;

    report_fail("create", "creat of rmtest.tmp failed");
    for (i = 0; i < sizeof(after_create) / sizeof(after_create[0]); i++) {
        report_notrun(after_create[i], "the test file was not created");
    }
    report_end();
}

/* The counts of the tests that passed and failed. */
static int passed, failed;

/** Tests 2 to 4: the created file opens, unlinks and is gone. */
static void file_rows(void)
{
    int fd, result;

    report_part("exists");
    print("Test 2: open(\"rmtest.tmp\") to verify... ");
    fd = sys_open("rmtest.tmp", O_RDONLY);
    if (fd >= 0) {
        puts("OK (file exists)");
        sys_close(fd);
        passed++;
    } else {
        puts("FAILED (file not found)");
        report_miss("open of the created rmtest.tmp failed");
        failed++;
    }

    report_part("unlink");
    print("Test 3: unlink(\"rmtest.tmp\")... ");
    result = sys_unlink("rmtest.tmp");
    if (result == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        report_miss("unlink of rmtest.tmp failed");
        failed++;
    }

    report_part("gone");
    print("Test 4: open(\"rmtest.tmp\") should fail... ");
    fd = sys_open("rmtest.tmp", O_RDONLY);
    if (fd < 0) {
        puts("OK (file gone)");
        passed++;
    } else {
        puts("FAILED (file still exists)");
        report_miss("rmtest.tmp still opened after its unlink");
        sys_close(fd);
        failed++;
    }
}

/** Tests 5 to 7: the errno of each refused unlink, and one more file
 * created and removed. */
static void errno_rows(void)
{
    int fd, result;

    report_part("enoent");
    print("Test 5: unlink(\"nonexistent.tmp\") == -ENOENT... ");
    result = sys_unlink("nonexistent.tmp");
    if (result == -ENOENT) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED (wrong errno)");
        report_miss("unlink of a missing name did not fail with ENOENT");
        failed++;
    }

    report_part("eisdir-root");
    print("Test 6: unlink(\"/\") == -EISDIR... ");
    result = sys_unlink("/");
    if (result == -EISDIR) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED (wrong errno)");
        report_miss("unlink of / did not fail with EISDIR");
        failed++;
    }

    report_part("eisdir-dir");
    print("Test 6b: unlink(a directory) == -EISDIR... ");
    if (sys_mkdir("rmtest.dir", 0755) == 0) {
        result = sys_unlink("rmtest.dir");
        if (result == -EISDIR) {
            puts("OK");
            passed++;
        } else {
            puts("FAILED (wrong errno)");
            report_miss("unlink of a directory did not fail with EISDIR");
            failed++;
        }
        sys_rmdir("rmtest.dir");
    } else {
        puts("FAILED (mkdir fixture failed)");
        report_miss("mkdir of rmtest.dir failed");
        failed++;
    }

    report_part("enotdir");
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
            report_miss("unlink of a path through a file did not fail with ENOTDIR");
            failed++;
        }
        sys_unlink("rmtest.plain");
    } else {
        puts("FAILED (creat fixture failed)");
        report_miss("creat of rmtest.plain failed");
        failed++;
    }

    report_part("create-unlink");
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
            report_miss("unlink of rmtest2.tmp failed");
            failed++;
        }
    } else {
        puts("FAILED (create failed)");
        report_miss("creat of rmtest2.tmp failed");
        failed++;
    }
    report_finish();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * The probe, which `_start` calls with the initial stack. Creates a file,
 * confirms it exists, unlinks it, and confirms it is gone; then checks the
 * errno of an unlink of a missing path, of the root, of a directory and of
 * a path through a file, and creates and removes one more file. Exits 0
 * when every test passed, else 1.
 */
void probe_main(unsigned long *sp)
{
    int fd;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.rmtest"));
    puts("Unlink syscall test starting...");
    puts("");

    print("Test 1: creat(\"rmtest.tmp\")... ");
    fd = sys_creat("rmtest.tmp", S_IRUSR | S_IWUSR);
    if (fd >= 0) {
        puts("OK");
        sys_close(fd);
        passed++;
        report_pass("create");
    } else {
        puts("FAILED (can't create test file)");
        failed++;
        report_no_file();
        goto results;
    }

    file_rows();
    errno_rows();

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
