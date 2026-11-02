/**
 * blockprobe: the data of files on a disk-backed KRFFS root, written through
 * the page cache and read back.
 *
 * On a KRFFS mounted from a disk through the page cache (rach2.fsdisk), it
 * writes NFILES files, each filled with a pattern of its index and the byte
 * offset, fsyncs each, then reads every file back and checks every byte. A
 * page whose dirty data was dropped, or that was read into the wrong file
 * or at the wrong offset, shows up as a mismatch. A file that compares
 * equal is removed.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, guest.disk-q35.blockprobe without it: `written`, every file written
 * whole and synced; `read`, every file read whole; and `intact`, every byte
 * read equal to its pattern. Beside them it prints "BLOCKPROBE OK" and exits
 * 0, or prints "BLOCKPROBE FAIL: " and the step, and exits 1, which the bus
 * suite reads. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x200

/* Twenty-four 256 KiB files, 6 MiB in all. The page cache holds PC_SLOTS
   pages, 16 MiB, or an eighth of memory when that is less (init_pagecache in
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

/** Report the failure `why` of the planned assertion now checked, print
 * "BLOCKPROBE FAIL: " and `step`, and exit 1. */
static void fail(const char *why, const char *step)
{
    char line[64] = "BLOCKPROBE FAIL: ";
    size_t n = strlen(line), i;

    report_stop(why);
    for (i = 0; step[i] != '\0' && n < sizeof(line) - 1; i++)
        line[n++] = step[i];
    line[n] = '\0';
    puts(line);
    sys_exit(1);
}

/** Write into `out`, of 64 bytes, "file N differs from its pattern at byte
 * B"; return `out`. */
static const char *mismatch(char *out, unsigned file, size_t at)
{
    char num[12];
    const char *part[4];
    size_t n = 0, i, k;

    part[0] = "file ";
    part[1] = report_decimal(num, (int)file);
    part[2] = " differs from its pattern at byte ";
    part[3] = 0;
    for (k = 0; k < 3; k++)
        for (i = 0; part[k][i] != '\0'; i++)
            out[n++] = part[k][i];
    report_decimal(out + n, (int)at);
    return out;
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe, entered with `sp` at argc: write and fsync every file, then
 * read each back, compare it with its pattern and unlink it. */
void probe_main(unsigned long *sp)
{
    char **envp = (char **)(sp + 1) + sp[0] + 1;
    char path[8], why[64];
    unsigned f, bad = NFILES;
    size_t i, bad_at = 0;
    int fd;

    report_begin(report_case(envp, "guest.disk-q35.blockprobe"));
    report_plan("written");
    report_plan("read");
    report_plan("intact");
    for (f = 0; f < NFILES; ++f) {
        path_of(path, f);
        for (i = 0; i < FILE_SIZE; ++i) {
            buf[i] = pattern(f, i);
        }
        fd = syscall3(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            fail("a file did not open for writing", "open for write");
        }
        if (write_all(fd, buf, FILE_SIZE) != 0) {
            fail("a write failed or wrote nothing", "write");
        }
        if (syscall1(SYS_FSYNC, fd) != 0) {
            fail("fsync failed", "fsync");
        }
        syscall1(SYS_CLOSE, fd);
    }
    report_held();

    for (f = 0; f < NFILES; ++f) {
        path_of(path, f);
        for (i = 0; i < FILE_SIZE; ++i) {
            buf[i] = 0;
        }
        fd = syscall3(SYS_OPEN, (long)path, O_RDONLY, 0);
        if (fd < 0) {
            fail("a file did not open for reading", "open for read");
        }
        if (read_all(fd, buf, FILE_SIZE) != 0) {
            fail("a read failed or met the end of the file early", "read");
        }
        syscall1(SYS_CLOSE, fd);
        for (i = 0; i < FILE_SIZE && buf[i] == pattern(f, i); ++i) {
        }
        if (i < FILE_SIZE && bad == NFILES) {
            bad = f;
            bad_at = i;
        }
        if (i == FILE_SIZE) {
            syscall3(SYS_UNLINK, (long)path, 0, 0);
        }
    }
    report_held();
    if (bad < NFILES) {
        fail(mismatch(why, bad, bad_at), "data mismatch");
    }
    report_held();
    report_end();
    puts("BLOCKPROBE OK");
    sys_exit(0);
}
