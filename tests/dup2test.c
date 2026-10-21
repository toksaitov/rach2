/**
 * dup2test: dup2 for redirection. It creates a file, puts it on stdout with
 * dup2, writes through stdout, then reads the file back and checks the
 * content. It also covers dup2(fd, fd) and the refusal of invalid
 * descriptors.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, with the assertions `identity`, `redirect` (the file put on
 * descriptor 1 takes the write), `restore` (dup2(2, 1)), `content` (the file
 * reads back the bytes), `bad-oldfd` and `bad-newfd`. A failure that ends
 * the tests leaves the assertions after it not run. Freestanding; direct
 * system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2

#define F_DUPFD 0

/** The write call: the count written, or a negated errno. */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** The read call: the count read, 0 at the end, or a negated errno. */
static int sys_read(int fd, void *buf, size_t count)
{
    return syscall3(SYS_READ, (long)fd, (long)buf, (long)count);
}

/** The open call: a descriptor, or a negated errno. */
static int sys_open(const char *path, int flags)
{
    return syscall2(SYS_OPEN, (long)path, (long)flags);
}

/** The close call: 0, or a negated errno. */
static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

/** The creat call: a descriptor open for writing, or a negated errno. */
static int sys_creat(const char *path, unsigned int mode)
{
    return syscall2(SYS_CREAT, (long)path, (long)mode);
}

/** The unlink call: 0, or a negated errno. */
static int sys_unlink(const char *path)
{
    return syscall1(SYS_UNLINK, (long)path);
}

/** The dup2 call: `newfd`, or a negated errno. */
static int sys_dup2(int oldfd, int newfd)
{
    return syscall2(SYS_DUP2, (long)oldfd, (long)newfd);
}

/** The fcntl call: a result of `cmd`, or a negated errno. */
static int sys_fcntl(int fd, int cmd, long arg)
{
    return syscall3(SYS_FCNTL, (long)fd, (long)cmd, arg);
}

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
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

/** Compare at most `n` bytes of two strings, as strncmp does. */
static int strncmp(const char *s1, const char *s2, size_t n)
{
    while (n > 0 && *s1 && *s1 == *s2) {
        s1++;
        s2++;
        n--;
    }
    if (n == 0)
        return 0;
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

/* Output goes to stderr, which the dup2 tests below leave untouched. */
#define puts_stderr(s)                          \
    do {                                        \
        sys_write(STDERR_FILENO, s, strlen(s)); \
        sys_write(STDERR_FILENO, "\n", 1);      \
    } while (0)

#define print_stderr(s) sys_write(STDERR_FILENO, s, strlen(s))

/** Write `n` in decimal to standard error. */
static void print_num_stderr(int n)
{
    char buf[12];
    int i = 0;

    if (n < 0) {
        sys_write(STDERR_FILENO, "-", 1);
        n = -n;
    }

    if (n == 0) {
        buf[i++] = '0';
    } else {
        while (n > 0) {
            buf[i++] = '0' + (n % 10);
            n /= 10;
        }
    }

    while (i > 0) {
        sys_write(STDERR_FILENO, &buf[--i], 1);
    }
}

static const char *test_file = "dup2test.tmp";
static const char *test_content = "Hello from dup2!";

/* The counts of the tests that passed and failed, and a copy of the
 * descriptor 1 the probe started with, above those the tests use. */
static int passed, failed, saved_stdout;

/* The assertions in order, and the index of the next to begin. */
static const char *const rows[] = {"identity", "redirect",  "restore",
                                   "content",  "bad-oldfd", "bad-newfd"};
static unsigned int next_row;

/** Begin the part of the next assertion. */
static void begin_row(void)
{
    report_part(rows[next_row++]);
}

/** End the open part and the report after a failure that ended the tests:
 * the assertions not yet begun are not run. */
static void report_rest(void)
{
    report_part_end();
    while (next_row < sizeof(rows) / sizeof(rows[0])) {
        report_notrun(rows[next_row++], "an earlier check failed");
    }
    report_end();
}

/** Test 1: dup2(1, 1) returns 1. */
static void identity_row(void)
{
    int result;

    begin_row();
    print_stderr("Test 1: dup2(1, 1) should return 1... ");
    result = sys_dup2(STDOUT_FILENO, STDOUT_FILENO);
    if (result == STDOUT_FILENO) {
        puts_stderr("OK");
        passed++;
    } else {
        print_stderr("FAILED (returned ");
        print_num_stderr(result);
        puts_stderr(")");
        report_miss("dup2(1, 1) did not return 1");
        failed++;
    }
}

/**
 * Test 2: put a created file on descriptor 1, write through it, and put
 * descriptor 1 back from descriptor 2; 0 when the file was not created or
 * not put on descriptor 1, and 1 otherwise.
 *
 * No record is written while descriptor 1 is the file. Descriptor 1 is then
 * put back from the copy the probe kept, which is standard output under
 * etude where descriptor 2 is not, before the next part begins.
 */
static int redirect_rows(void)
{
    int fd, n, result, restored;

    begin_row();
    puts_stderr("");
    print_stderr("Test 2: redirect stdout to file...\n");

    print_stderr("  Creating ");
    print_stderr(test_file);
    print_stderr("... ");
    fd = sys_creat(test_file, 0644);
    if (fd < 0) {
        puts_stderr("FAILED");
        report_miss("creat of dup2test.tmp failed");
        failed++;
        return 0;
    }
    print_stderr("OK (fd=");
    print_num_stderr(fd);
    puts_stderr(")");

    print_stderr("  dup2(");
    print_num_stderr(fd);
    print_stderr(", 1)... ");
    result = sys_dup2(fd, STDOUT_FILENO);
    if (result != STDOUT_FILENO) {
        print_stderr("FAILED (returned ");
        print_num_stderr(result);
        puts_stderr(")");
        report_miss("dup2 of the file onto descriptor 1 failed");
        failed++;
        sys_close(fd);
        sys_dup2(saved_stdout, STDOUT_FILENO);
        return 0;
    }
    puts_stderr("OK");

    /* A write to fd 1 goes to the file. */
    print_stderr("  Writing via stdout... ");
    n = sys_write(STDOUT_FILENO, test_content, strlen(test_content));
    if (n == (int)strlen(test_content)) {
        puts_stderr("OK");
    } else {
        print_stderr("FAILED (wrote ");
        print_num_stderr(n);
        puts_stderr(" bytes)");
        report_miss("the write through descriptor 1 fell short");
        failed++;
    }

    /* fd 1 keeps the file open after the original descriptor closes. */
    sys_close(fd);

    /* fd 2 is still the console, so its copy puts fd 1 back on it. */
    print_stderr("  Restoring stdout from stderr... ");
    result = sys_dup2(STDERR_FILENO, STDOUT_FILENO);
    restored = result == STDOUT_FILENO;
    if (restored) {
        puts_stderr("OK");
    } else {
        puts_stderr("FAILED");
        failed++;
    }
    sys_dup2(saved_stdout, STDOUT_FILENO);
    begin_row();
    if (!restored) {
        report_miss("dup2(2, 1) did not return 1");
    }
    return 1;
}

/** Test 3: the file reads back the bytes written through descriptor 1; 0
 * when it did not open or read, and 1 otherwise. */
static int content_row(void)
{
    char buf[128];
    int fd, n;

    begin_row();
    puts_stderr("");
    print_stderr("Test 3: verify file content...\n");

    print_stderr("  Opening ");
    print_stderr(test_file);
    print_stderr("... ");
    fd = sys_open(test_file, O_RDONLY);
    if (fd < 0) {
        puts_stderr("FAILED");
        report_miss("open of dup2test.tmp failed");
        failed++;
        return 0;
    }
    print_stderr("OK (fd=");
    print_num_stderr(fd);
    puts_stderr(")");

    print_stderr("  Reading... ");
    n = sys_read(fd, buf, sizeof(buf) - 1);
    if (n < 0) {
        puts_stderr("FAILED");
        report_miss("read of dup2test.tmp failed");
        sys_close(fd);
        failed++;
        return 0;
    }
    buf[n] = '\0';
    print_stderr("OK (");
    print_num_stderr(n);
    puts_stderr(" bytes)");

    print_stderr("  Content: \"");
    print_stderr(buf);
    puts_stderr("\"");

    print_stderr("  Comparing... ");
    if (strncmp(buf, test_content, strlen(test_content)) == 0) {
        puts_stderr("OK (content matches!)");
        passed++;
    } else {
        puts_stderr("FAILED (content mismatch)");
        report_miss("the file did not read back the bytes written");
        failed++;
    }
    sys_close(fd);
    return 1;
}

/** Tests 4 and 5: dup2 refuses an invalid old and new descriptor. */
static void invalid_rows(void)
{
    int result;

    /* Test 4: dup2 with an invalid oldfd fails */
    begin_row();
    puts_stderr("");
    print_stderr("Test 4: dup2(99, 5) with invalid oldfd... ");
    result = sys_dup2(99, 5);
    if (result < 0) {
        puts_stderr("OK (failed as expected)");
        passed++;
    } else {
        print_stderr("FAILED (should have failed, got ");
        print_num_stderr(result);
        puts_stderr(")");
        report_miss("dup2(99, 5) succeeded");
        failed++;
    }

    /* Test 5: dup2 onto a number past the table's limit of 1024 fails */
    begin_row();
    print_stderr("Test 5: dup2(1, 2000) with invalid newfd... ");
    result = sys_dup2(STDOUT_FILENO, 2000);
    if (result < 0) {
        puts_stderr("OK (failed as expected)");
        passed++;
    } else {
        print_stderr("FAILED (should have failed, got ");
        print_num_stderr(result);
        puts_stderr(")");
        report_miss("dup2(1, 2000) succeeded");
        failed++;
    }
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * The probe, which `_start` calls with the initial stack. It runs the dup2
 * tests in order: the identity dup2(fd, fd), redirecting stdout onto a file
 * and reading the bytes back, and the failure cases for an invalid oldfd
 * and an invalid newfd. Stdout is put back on the console from stderr after
 * the write. Exits 0 when all pass and 1 otherwise.
 */
void probe_main(unsigned long *sp)
{
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.dup2test"));
    saved_stdout = sys_fcntl(STDOUT_FILENO, F_DUPFD, 64);
    puts_stderr("Dup2 syscall test starting...");
    puts_stderr("");

    identity_row();
    if (redirect_rows() && content_row()) {
        invalid_rows();
        report_finish();
    } else {
        report_rest();
    }
    sys_close(saved_stdout);

    puts_stderr("");
    print_stderr("Cleanup: unlinking ");
    print_stderr(test_file);
    print_stderr("... ");
    if (sys_unlink(test_file) == 0) {
        puts_stderr("OK");
    } else {
        puts_stderr("(file may not exist)");
    }

    puts_stderr("");
    print_stderr("Results: ");
    print_num_stderr(passed);
    print_stderr(" passed, ");
    print_num_stderr(failed);
    puts_stderr(" failed");

    if (failed == 0) {
        puts_stderr("All dup2 syscalls passed!");
        sys_exit(0);
    } else {
        puts_stderr("Some tests failed!");
        sys_exit(1);
    }
}
