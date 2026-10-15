/**
 * blockprobe: the data of files on a disk-backed KRFFS root, written through
 * the page cache and read back.
 *
 * On a KRFFS mounted from a disk through the page cache (rach2.fsdisk), it
 * writes NFILES files, each filled with a pattern of its index and the byte
 * offset, fsyncs each, then reads every file back and checks every byte. A
 * page whose dirty data was dropped, or that was read into the wrong file
 * or at the wrong offset, shows up as a mismatch. Prints "BLOCKPROBE OK"
 * and exits 0 on success, or prints "BLOCKPROBE FAIL: " and the step, and
 * exits 1.
 */

#include "probe.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x200

/* Twenty-four 256 KiB files, 6 MiB in all. The page cache holds PC_SLOTS
   pages, 8 MiB, or an eighth of memory when that is less (init_pagecache in
   rach2/sys/mm/pagecache.c), so the files outgrow it only on a machine of
   under 48 MiB. */
#ifndef NFILES
#define NFILES 24u
#endif
#ifndef FILE_SIZE
#define FILE_SIZE (256u * 1024u)
#endif

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

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

/** Write "/bpNN", the file index `n` in two decimal digits, to `out`, which
 * has room for six bytes; `n` is under 100. */
static void path_of(char *out, unsigned n)
{
    out[0] = '/';
    out[1] = 'b';
    out[2] = 'p';
    out[3] = (char)('0' + (n / 10u));
    out[4] = (char)('0' + (n % 10u));
    out[5] = '\0';
}

/** A byte that depends on the file index and the byte offset, so a page that
 * lands in the wrong file or at the wrong offset is caught. */
static unsigned char pattern(unsigned file, size_t off)
{
    return (unsigned char)((off * 31u + 7u) ^ (file * 131u) ^ (off >> 11));
}

/** Write all `n` bytes of `p` to `fd`: 0, or -1 when a write fails or
 * writes nothing. */
static int write_all(int fd, const unsigned char *p, size_t n)
{
    size_t off = 0;
    while (off < n) {
        int w = syscall3(SYS_WRITE, fd, (long)(p + off), (int)(n - off));
        if (w <= 0) {
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

/** Read `n` bytes from `fd` into `p`: 0, or -1 when a read fails or meets
 * the end of the file first. */
static int read_all(int fd, unsigned char *p, size_t n)
{
    size_t off = 0;
    while (off < n) {
        int r = syscall3(SYS_READ, fd, (long)(p + off), (int)(n - off));
        if (r <= 0) {
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}

/** The probe: write and fsync every file, then read each back, compare it
 * with its pattern and unlink it. */
PROBE_START void _start(void)
{
    char path[8];
    unsigned f;
    size_t i;
    int fd;

    for (f = 0; f < NFILES; ++f) {
        path_of(path, f);
        for (i = 0; i < FILE_SIZE; ++i) {
            buf[i] = pattern(f, i);
        }
        fd = syscall3(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            puts("BLOCKPROBE FAIL: open for write");
            sys_exit(1);
        }
        if (write_all(fd, buf, FILE_SIZE) != 0) {
            puts("BLOCKPROBE FAIL: write");
            sys_exit(1);
        }
        if (syscall1(SYS_FSYNC, fd) != 0) {
            puts("BLOCKPROBE FAIL: fsync");
            sys_exit(1);
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
            puts("BLOCKPROBE FAIL: open for read");
            sys_exit(1);
        }
        if (read_all(fd, buf, FILE_SIZE) != 0) {
            puts("BLOCKPROBE FAIL: read");
            sys_exit(1);
        }
        syscall1(SYS_CLOSE, fd);
        for (i = 0; i < FILE_SIZE; ++i) {
            if (buf[i] != pattern(f, i)) {
                puts("BLOCKPROBE FAIL: data mismatch");
                sys_exit(1);
            }
        }
        syscall3(SYS_UNLINK, (long)path, 0, 0);
    }

    puts("BLOCKPROBE OK");
    sys_exit(0);
}
