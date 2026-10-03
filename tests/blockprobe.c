/**
 * Disk-backed page-cache integrity probe.
 *
 * On a KRFFS mounted from a real disk through the page cache (rach2.fsdisk),
 * writes several files whose combined size exceeds the resident cache, each one
 * filled with a position-dependent pattern, fsyncs them, then reads every file
 * back and checks every byte. Because the working set is larger than the cache,
 * writing later files evicts and writes back the earlier files' dirty pages,
 * and reading them back re-fills from the disk; a dropped-dirty,
 * freed-while-mapped, or mis-mapped page then shows up as a checksum mismatch,
 * not a mere crash. Each file stays well under the cache budget, so
 * this exercises eviction across files without the single-file grow-relocation
 * limit. Prints "BLOCKPROBE OK" and exits 0 on success, or exits 1 on any
 * mismatch.
 *
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x200

/* Twenty-four 256 KiB files: 6 MiB of working set against a 4 MiB cache
   forces eviction across files, while each file stays well under the cache
   budget. The count grew with the cache (it was eight files against 1 MiB);
   a working set the cache can hold outright proves nothing about eviction
   or write-back. */
#ifndef NFILES
#define NFILES 24u
#endif
#ifndef FILE_SIZE
#define FILE_SIZE (256u * 1024u)
#endif

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

static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

/* "/bpNN", the file's index in two decimal digits. It was one byte, '0' + n,
   which ran past '9' into punctuation for the files past the tenth: unique,
   but a name nobody could read in a diagnostic. */
static void path_of(char *out, unsigned n)
{
    out[0] = '/';
    out[1] = 'b';
    out[2] = 'p';
    out[3] = (char)('0' + (n / 10u));
    out[4] = (char)('0' + (n % 10u));
    out[5] = '\0';
}

/* A byte that depends on the file index and the byte offset, so a page that
   lands in the wrong file or at the wrong offset is caught. */
static unsigned char pattern(unsigned file, size_t off)
{
    return (unsigned char)((off * 31u + 7u) ^ (file * 131u) ^ (off >> 11));
}

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
