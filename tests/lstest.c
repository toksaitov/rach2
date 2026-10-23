/**
 * lstest: the getdents64 call, as `ls` lists a directory.
 *
 * The probe opens "/", requires a read of 0 bytes to be EINVAL, with a null
 * buffer too, reads its entries, of which the root has some, walks the
 * linux_dirent64 records and prints each, reads again, which gives the end of
 * the directory or more entries, requires a read of 0 bytes at the end to
 * give 0, and requires a descriptor that is not open to fail with EBADF.
 *
 * Prints a line for each test and exits 0 when every one passed, 1
 * otherwise. Writes etude's report records (probe_report.h) for the case
 * ETUDE_CASE names, an assertion for each test; a failed open of "/" leaves
 * the rest not run, and a failed first read the tests up to the close.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;

#define STDOUT_FILENO 1

#define EBADF  9
#define EINVAL 22

/* Open flags */
#define O_RDONLY    0
#define O_DIRECTORY 0200000 /* Must be a directory */

/* Directory entry types (d_type values) */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12
#define DT_WHT     14

/** The record format returned by getdents64. */
struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;     /* the position the next read resumes from */
    uint16_t d_reclen; /* this record's length, padding included */
    uint8_t d_type;
    char d_name[]; /* NUL-terminated */
};

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

/** open(2) with no mode. */
static int sys_open(const char *pathname, int flags)
{
    return syscall2(SYS_OPEN, (long)pathname, (long)flags);
}

/** close(2). */
static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

/** getdents64(2). */
static int sys_getdents64(int fd, void *dirp, unsigned int count)
{
    return syscall3(SYS_GETDENTS64, (long)fd, (long)dirp, (long)count);
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

/** Write `n` in decimal to standard output, with a minus sign before it when
 * it is negative. */
static void print_num(int n)
{
    unsigned int u = (unsigned int)n;
    char c;

    if (n < 0) {
        sys_write(STDOUT_FILENO, "-", 1);
        u = 0u - u;
    }
    if (u >= 10) {
        print_num((int)(u / 10));
    }
    c = (char)('0' + u % 10);
    sys_write(STDOUT_FILENO, &c, 1);
}

/* The tests, one assertion each, which were reported, and the count of each
 * verdict. */
enum { OPEN, COUNT_ZERO, READ, ENTRIES, READ_AGAIN, COUNT_ZERO_END, BAD_FD, CLOSE, TESTS };

static const char *const test_ids[TESTS] = {"open",       "count-zero",     "read",   "entries",
                                            "read-again", "count-zero-end", "bad-fd", "close"};
static char reported[TESTS];
static int passed, failed;

/** Count the test `test` as passed when `ok` is nonzero, and as failed for
 * `why` otherwise, and report it. */
static void count(int test, int ok, const char *why)
{
    if (ok) {
        passed++;
        report_pass(test_ids[test]);
    } else {
        failed++;
        report_fail(test_ids[test], why);
    }
    reported[test] = 1;
}

/** Print "OK" when `ok` is nonzero, else "FAILED", and count the test `test`
 * as `count` does. */
static void verdict(int test, int ok, const char *why)
{
    puts(ok ? "OK" : "FAILED");
    count(test, ok, why);
}

/** Report each test not reported as not run, and end the report. */
static void report_rest(void)
{
    int i;

    for (i = 0; i < TESTS; i++) {
        if (!reported[i]) {
            report_notrun(test_ids[i], "an earlier test failed");
        }
    }
    report_end();
}

/** Write `n` as "0x" and eight hexadecimal digits to standard output. */
static void print_hex(uint32_t n)
{
    const char *hex = "0123456789abcdef";
    char buf[9];
    int i;

    for (i = 7; i >= 0; i--) {
        buf[i] = hex[n & 0xF];
        n >>= 4;
    }
    buf[8] = '\0';
    print("0x");
    print(buf);
}

/** The name of the record type `d_type`, or "???" for one not listed. */
static const char *get_type_name(uint8_t d_type)
{
    switch (d_type) {
    case DT_REG:
        return "REG";
    case DT_DIR:
        return "DIR";
    case DT_LNK:
        return "LNK";
    case DT_CHR:
        return "CHR";
    case DT_BLK:
        return "BLK";
    case DT_FIFO:
        return "FIFO";
    case DT_SOCK:
        return "SOCK";
    default:
        return "???";
    }
}

/**
 * Test 4: walk the `nread` bytes of records in `buf` and print each. The
 * test passes when there was a record and none had a length of 0; a record
 * of length 0 is counted as a failure of its own, as the walk always did.
 */
static void walk(char *buf, int nread)
{
    struct linux_dirent64 *d;
    int pos = 0;
    int file_count = 0;
    const char *why = 0;

    puts("Test 4: Parsing directory entries...");
    while (pos < nread) {
        d = (struct linux_dirent64 *)(buf + pos);

        /* A record of length 0 would not advance. */
        if (d->d_reclen == 0) {
            puts("  ERROR: d_reclen is 0, breaking");
            failed++;
            why = "a record has d_reclen 0";
            break;
        }

        print("  [");
        print_num(file_count);
        print("] \"");
        print(d->d_name);
        print("\" type=");
        print(get_type_name(d->d_type));
        print(" ino=");
        print_hex((uint32_t)d->d_ino);
        print(" reclen=");
        print_num(d->d_reclen);
        puts("");

        file_count++;
        pos += d->d_reclen;
    }

    if (file_count > 0) {
        print("  Found ");
        print_num(file_count);
        puts(" entries... OK");
        passed++;
    } else {
        puts("  No entries found... FAILED");
        failed++;
        why = why != 0 ? why : "no entries were found";
    }
    if (why != 0) {
        report_fail(test_ids[ENTRIES], why);
    } else {
        report_pass(test_ids[ENTRIES]);
    }
    reported[ENTRIES] = 1;
}

/** Tests 5 to 7 on the directory `fd`: a second read, a read of 0 bytes at
 * the end, and a read of a descriptor that is not open. */
static void read_rest(int fd, char *buf, int size)
{
    int nread;

    /* Test 5: getdents64 again returns 0 (end of dir) or more entries */
    print("Test 5: getdents64 again... ");
    nread = sys_getdents64(fd, buf, size);
    if (nread >= 0) {
        if (nread == 0) {
            puts("OK (end of directory)");
        } else {
            print("OK (");
            print_num(nread);
            puts(" more bytes)");
        }
        count(READ_AGAIN, 1, 0);
    } else {
        print("FAILED (ret=");
        print_num(nread);
        puts(")");
        count(READ_AGAIN, 0, "the second read failed");
    }

    /* Test 6: At the end of the directory a count of 0 gives 0. */
    print("Test 6: getdents64(fd, buf, 0) at the end... ");
    while (nread > 0) {
        nread = sys_getdents64(fd, buf, size);
    }
    verdict(COUNT_ZERO_END, nread == 0 && sys_getdents64(fd, buf, 0) == 0,
            "a read of 0 bytes at the end did not give 0");

    /* Test 7: getdents64 on a descriptor that is not open fails with EBADF. */
    print("Test 7: getdents64(999, ...)... ");
    verdict(BAD_FD, sys_getdents64(999, buf, size) == -EBADF,
            "a read of descriptor 999 did not fail with EBADF");
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * The probe's entry: opens "/", checks that a count of 0 is EINVAL, reads its
 * entries with getdents64, walks and prints each one, reads again, checks
 * that a count of 0 at the end gives 0 and that a bad fd fails, closes the
 * directory, and prints the count of each verdict. `_start` calls it with
 * the initial stack, whose environment gives the case's identity.
 */
void probe_main(unsigned long *sp)
{
    char buf[1024];
    int fd;
    int nread;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.lstest"));
    puts("Getdents64 (directory listing) syscall test starting...");
    puts("");

    /* Test 1: Open root directory "/" */
    print("Test 1: open(\"/\", O_RDONLY)... ");
    fd = sys_open("/", O_RDONLY);
    if (fd >= 0) {
        print("OK (fd=");
        print_num(fd);
        puts(")");
        count(OPEN, 1, 0);
    } else {
        print("FAILED (ret=");
        print_num(fd);
        puts(")");
        count(OPEN, 0, "open of / failed");
        goto done;
    }

    /* Test 2: A count of 0 is EINVAL while an entry remains, as on Linux. */
    print("Test 2: getdents64(fd, buf, 0)... ");
    verdict(COUNT_ZERO,
            sys_getdents64(fd, buf, 0) == -EINVAL && sys_getdents64(fd, 0, 0) == -EINVAL,
            "a read of 0 bytes was not EINVAL");

    /* Test 3: Read directory entries with getdents64 */
    print("Test 3: getdents64(fd, buf, 1024)... ");
    nread = sys_getdents64(fd, buf, sizeof(buf));
    if (nread > 0) {
        print("OK (");
        print_num(nread);
        puts(" bytes)");
        count(READ, 1, 0);
    } else {
        print("FAILED (ret=");
        print_num(nread);
        puts(")");
        count(READ, 0, "the first read gave no bytes");
        goto cleanup;
    }

    walk(buf, nread);
    read_rest(fd, buf, (int)sizeof(buf));

cleanup:
    /* Test 8: Close directory */
    print("Test 8: close(fd)... ");
    verdict(CLOSE, sys_close(fd) == 0, "close of the directory failed");

done:
    report_rest();
    puts("");
    print("Results: ");
    print_num(passed);
    print(" passed, ");
    print_num(failed);
    puts(" failed");

    if (failed == 0) {
        puts("All getdents64 syscalls passed!");
        sys_exit(0);
    } else {
        puts("Some tests failed!");
        sys_exit(1);
    }
}
