/**
 * stressprobe: the page cache under load, with more data through a
 * disk-backed file system than the cache holds.
 *
 * On a KRFFS mounted from a disk (rach2.fsdisk), writes 132 files of 256
 * KiB each, 33 MiB, fsyncing each, then reads every file back and checks a
 * pattern that depends on the file and the offset. The page cache holds at
 * most 8 MiB, so once its slots are full the writes and the reads replace
 * pages in it, whatever the machine's memory; data that comes back
 * unchanged shows that a file system larger than the cache works through
 * it. Prints "STRESSPROBE OK n=<files> bytes=<total>" and exits 0, or
 * prints "STRESSPROBE FAIL:" with the step, the file and the call's result
 * or the offset that differs, and exits 1. A file, 256 KiB, is a
 * thirty-second of the cache's largest size.
 *
 * The files are `/s000` to `/s131` at the root, or under the directory the
 * argument `dir=PATH` names (probe_args.h), which a native run gives for a
 * KRFFS volume mounted in its workspace.
 */

#include "probe.h"
#include "probe_args.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x200

/* 132 x 256 KiB == 33 MiB, over four times the most the page cache holds. */
#define NFILES    132u
#define FILE_SIZE (256u * 1024u)

static unsigned char buf[FILE_SIZE];

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/** Write `s` to standard output, with no newline. */
static void out(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
}

/** Write `v` in decimal to standard output. */
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

/* The room for a file's path: the directory's and "/sNNN". */
#define PATH_ROOM 160

/** Write "DIR/sNNN" for file `n`, under 1000, to `o`, which has room for
 * PATH_ROOM bytes; DIR is the argument `dir`, or empty, the root, when there
 * is none. 0, or -1 when DIR is too long. */
static int path_of(char *o, unsigned n)
{
    const char *dir = probe_arg("dir", "");
    int k = 0;

    while (dir[k] != '\0') {
        if (k >= PATH_ROOM - 6) {
            return -1;
        }
        o[k] = dir[k];
        k++;
    }
    o[k] = '/';
    o[k + 1] = 's';
    o[k + 2] = (char)('0' + (n / 100u) % 10u);
    o[k + 3] = (char)('0' + (n / 10u) % 10u);
    o[k + 4] = (char)('0' + n % 10u);
    o[k + 5] = '\0';
    return 0;
}

/** A byte that depends on the file index and the byte offset. */
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

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe: write and fsync every file, then read each back, compare it
 * with its pattern and unlink it. */
void probe_main(unsigned long *sp)
{
    char path[PATH_ROOM];
    unsigned f;
    size_t i;
    int fd, rc;

    probe_args((int)sp[0], (char **)(sp + 1));
    for (f = 0; f < NFILES; ++f) {
        if (path_of(path, f) != 0) {
            fail_at("the path under dir=", f, 0);
        }
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
