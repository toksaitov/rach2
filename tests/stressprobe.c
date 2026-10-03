/**
 * The page cache under load: more data through a disk-backed file system
 * than the cache holds.
 *
 * On a KRFFS mounted from a real disk (rach2.fsdisk), writes 132 files of 256
 * KiB each, 33 MiB, fsyncing each, then reads every file back and checks a
 * pattern that depends on the file and the offset. The page cache holds at
 * most 8 MiB, so once its slots are full the writes and the reads replace
 * pages in it, whatever the machine's memory; that the data round-trips
 * exactly is the proof that a file system larger than the cache works
 * through it. Prints "STRESSPROBE OK n=<files> bytes=<total>" and exits 0,
 * or prints "STRESSPROBE FAIL:" with the step, the file and the call's
 * result or the offset that differs, and exits 1. A file, 256 KiB, is a
 * thirty-second of the cache's largest size.
 *
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x200

/* 132 x 256 KiB == 33 MiB, past the 32 MiB physical-RAM cap. */
#define NFILES    132u
#define FILE_SIZE (256u * 1024u)

static unsigned char buf[FILE_SIZE];

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void out(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
}

static void out_num(unsigned long v)
{
    char d[24];
    int i = 0;
    if (v == 0) {
        out("0");
        return;
    }
    while (v > 0) {
        d[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (i > 0) {
        char c = d[--i];
        syscall3(SYS_WRITE, 1, (long)&c, 1);
    }
}

/* "/sNNN" for file N (N < 1000). */
static void path_of(char *o, unsigned n)
{
    o[0] = '/';
    o[1] = 's';
    o[2] = (char)('0' + (n / 100u) % 10u);
    o[3] = (char)('0' + (n / 10u) % 10u);
    o[4] = (char)('0' + n % 10u);
    o[5] = '\0';
}

static unsigned char pattern(unsigned file, size_t off)
{
    return (unsigned char)((off * 31u + 7u) ^ (file * 131u) ^ (off >> 11));
}

/** Write all `n` bytes: 1, or the result of the write that failed, a
 * negative errno or 0 for one that moved nothing. */
static int write_all(int fd, const unsigned char *p, size_t n)
{
    size_t off = 0;
    while (off < n) {
        int w = syscall3(SYS_WRITE, fd, (long)(p + off), (int)(n - off));
        if (w <= 0) {
            return w;
        }
        off += (size_t)w;
    }
    return 1;
}

/** Read all `n` bytes: 1, or the result of the read that failed. */
static int read_all(int fd, unsigned char *p, size_t n)
{
    size_t off = 0;
    while (off < n) {
        int r = syscall3(SYS_READ, fd, (long)(p + off), (int)(n - off));
        if (r <= 0) {
            return r;
        }
        off += (size_t)r;
    }
    return 1;
}

/** Report that the step `what` of file `file` failed with `rc`, a call's
 * result or the offset of a byte that differs, and end the probe. */
static void fail_at(const char *what, unsigned file, int rc)
{
    out("STRESSPROBE FAIL: ");
    out(what);
    out(" of file ");
    out_num(file);
    out(": ");
    if (rc < 0) {
        out("-");
        out_num((unsigned long)-rc);
    } else {
        out_num((unsigned long)rc);
    }
    out("\n");
    sys_exit(1);
}

PROBE_START void _start(void)
{
    char path[8];
    unsigned f;
    size_t i;
    int fd, rc;

    for (f = 0; f < NFILES; ++f) {
        path_of(path, f);
        for (i = 0; i < FILE_SIZE; ++i) {
            buf[i] = pattern(f, i);
        }
        fd = syscall3(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fail_at("the open to write", f, fd);
        }
        rc = write_all(fd, buf, FILE_SIZE);
        if (rc != 1) {
            fail_at("a write", f, rc);
        }
        rc = syscall1(SYS_FSYNC, fd);
        if (rc != 0) {
            fail_at("the fsync", f, rc);
        }
        syscall1(SYS_CLOSE, fd);
    }

    for (f = 0; f < NFILES; ++f) {
        path_of(path, f);
        for (i = 0; i < FILE_SIZE; ++i) {
            buf[i] = 0;
        }
        fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
        if (fd < 0) {
            fail_at("the open to read", f, fd);
        }
        rc = read_all(fd, buf, FILE_SIZE);
        if (rc != 1) {
            fail_at("a read", f, rc);
        }
        syscall1(SYS_CLOSE, fd);
        for (i = 0; i < FILE_SIZE; ++i) {
            if (buf[i] != pattern(f, i)) {
                fail_at("the bytes", f, (int)i);
            }
        }
        syscall3(SYS_UNLINK, (long)path, 0, 0);
    }

    out("STRESSPROBE OK n=");
    out_num(NFILES);
    out(" bytes=");
    out_num((unsigned long)NFILES * FILE_SIZE);
    out("\n");
    sys_exit(0);
}
