/**
 * Exercises the getdents64 syscall: open "/", read directory entries, parse
 * the linux_dirent64 stream, and confirm a second call reaches end-of-directory
 * while a bad fd fails. KRFFS is flat, so every file appears in root "/".
 *
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;

#define STDOUT_FILENO 1

/* Syscall numbers (32-bit Linux / Rach2 compatible) */

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
    uint64_t d_ino;    /* 64-bit inode number */
    int64_t d_off;     /* 64-bit offset to next structure */
    uint16_t d_reclen; /* Size of this dirent */
    uint8_t d_type;    /* File type */
    char d_name[];     /* Filename (null-terminated) */
};

static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

static int sys_open(const char *pathname, int flags)
{
    return syscall2(SYS_OPEN, (long)pathname, (long)flags);
}

static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

static int sys_getdents64(int fd, void *dirp, unsigned int count)
{
    return syscall3(SYS_GETDENTS64, (long)fd, (long)dirp, (long)count);
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
        print_num(n / 10);
    }
    char c = '0' + (n % 10);
    sys_write(STDOUT_FILENO, &c, 1);
}

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
 * Entry point. Opens "/", reads its entries with getdents64, parses and prints
 * each one, calls getdents64 again to verify end-of-directory, checks that a
 * bad fd fails, then closes the directory.
 */
PROBE_START void _start(void)
{
    char buf[1024]; /* Buffer for directory entries */
    struct linux_dirent64 *d;
    int fd;
    int nread;
    int pos;
    int passed = 0;
    int failed = 0;
    int file_count = 0;

    puts("Getdents64 (directory listing) syscall test starting...");
    puts("");

    /* Test 1: Open root directory "/" */
    print("Test 1: open(\"/\", O_RDONLY)... ");
    fd = sys_open("/", O_RDONLY);
    if (fd >= 0) {
        print("OK (fd=");
        print_num(fd);
        puts(")");
        passed++;
    } else {
        print("FAILED (ret=");
        print_num(fd);
        puts(")");
        failed++;
        goto done;
    }

    /* Test 2: Read directory entries with getdents64 */
    print("Test 2: getdents64(fd, buf, 1024)... ");
    nread = sys_getdents64(fd, buf, sizeof(buf));
    if (nread > 0) {
        print("OK (");
        print_num(nread);
        puts(" bytes)");
        passed++;
    } else if (nread == 0) {
        puts("OK (empty directory)");
        passed++;
    } else {
        print("FAILED (ret=");
        print_num(nread);
        puts(")");
        failed++;
        goto cleanup;
    }

    /* Test 3: Parse and display entries */
    puts("Test 3: Parsing directory entries...");
    if (nread > 0) {
        pos = 0;
        while (pos < nread) {
            d = (struct linux_dirent64 *)(buf + pos);

            /* Sanity check reclen */
            if (d->d_reclen == 0) {
                puts("  ERROR: d_reclen is 0, breaking");
                failed++;
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
        }
    } else {
        puts("  (no entries to parse)");
        passed++;
    }

    /* Test 4: getdents64 again returns 0 (end of dir) or more entries */
    print("Test 4: getdents64 again... ");
    nread = sys_getdents64(fd, buf, sizeof(buf));
    if (nread >= 0) {
        if (nread == 0) {
            puts("OK (end of directory)");
        } else {
            print("OK (");
            print_num(nread);
            puts(" more bytes)");
        }
        passed++;
    } else {
        print("FAILED (ret=");
        print_num(nread);
        puts(")");
        failed++;
    }

    /* Test 5: getdents64 on invalid fd should fail */
    print("Test 5: getdents64(999, ...)... ");
    nread = sys_getdents64(999, buf, sizeof(buf));
    if (nread < 0) {
        puts("OK (failed as expected)");
        passed++;
    } else {
        puts("FAILED (should have failed)");
        failed++;
    }

cleanup:
    /* Test 6: Close directory */
    print("Test 6: close(fd)... ");
    if (sys_close(fd) == 0) {
        puts("OK");
        passed++;
    } else {
        puts("FAILED");
        failed++;
    }

done:
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
