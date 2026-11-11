/**
 * pagecachemusl: check that read(2) of an ext2 file comes from the file
 * page cache the second time, and that every way of changing the file is
 * seen by the next read.
 *
 * Usage: pagecachemusl NODE MIB SMALL, where NODE is an ext2 volume, which
 * the program mounts at /pc, MIB the size of the large file in MiB, more
 * than the buffer cache's half of the kernel's heap, and SMALL a small ext2
 * volume for the unmount row.
 *
 * `twice`: writes /pc/big.bin of MIB MiB, each 4 KiB block stamped with its
 * number, syncs it, and reads it whole twice, checking every block. The
 * second read goes by read, pread64, readv and preadv2, a quarter each, and
 * must take nothing from the disks: /proc/vmstat's pgpgin, the kilobytes
 * read from the disks since boot, must not move. /proc/meminfo's Cached
 * must have grown by the file's size after the first read.
 *
 * `eof`: a file of three pages and 100 bytes, written and synced but not
 * read, is read with pread64 at its size, one byte past it, a page past it,
 * at 2^44 and at 2^44 and a page, past the 32-bit page numbers. Each read
 * must give 0 bytes and leave /proc/meminfo's Cached as it was, so that no
 * page of the file joins the cache.
 *
 * The rows: a file of three pages and 100 bytes is written whole and read
 * whole, which brings its pages into the cache; then one way of changing it
 * runs, and a second read must give the bytes and the size the change left:
 * write, pwrite, writev, pwritev, pwritev2, truncate down and up, ftruncate
 * down and up, fallocate past the end, copy_file_range, sendfile and splice
 * into the file, stores through a shared mapping read before msync and
 * after munmap, and a change made through the volume's node while it is
 * unmounted, found by the inode's block pointers, after which the mount
 * must not give the pages it had before. A kernel that is not a recovery
 * build writes no disk's node, so that row copies SMALL to a tmpfs at /pt,
 * attaches the copy to /dev/loop0, mounts it at /pu and writes the loop
 * device's node, in whole sectors as it takes them.
 *
 * Prints `PAGECACHEMUSL OK` and exits 0, or prints `PAGECACHEMUSL FAIL
 * (detail)` and exits 1. The counts are fixed; nothing reads a clock.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#define DIR      "/pc"
#define BIG      DIR "/big.bin"
#define FILE_    DIR "/co.bin"
#define SRC      DIR "/src.bin"
#define TMP      "/pt"
#define COPY     TMP "/small.ext2"
#define LOOP     "/dev/loop0"
#define LOOPDIR  "/pu"
#define SECTOR   512u
#define BLOCK    4096u
#define CHUNK    (1u << 20)
#define ROW_SIZE (3u * BLOCK + 100u)
#define ROW_MOST (ROW_SIZE + 2u * BLOCK) /* the largest a row makes the file */

static const char *node, *small;

/* The file the rows change. */
static const char *row_path = FILE_;

/* What the row's file holds, and its size. */
static unsigned char want[ROW_MOST];
static size_t want_size;

/** Print the failure `fmt` and exit 1. */
static __attribute__((noreturn, format(printf, 1, 2))) void fail(const char *fmt, ...)
{
    va_list ap;

    printf("PAGECACHEMUSL FAIL (");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf(")\n");
    exit(1);
}

/** The number after `label` at the start of a line of the file `path`, or
 * fail. */
static unsigned long long proc_number(const char *path, const char *label)
{
    static char text[8192];
    size_t len = strlen(label);
    int fd = open(path, O_RDONLY);
    ssize_t n;
    char *line;

    if (fd < 0) {
        fail("open %s: %s", path, strerror(errno));
    }
    n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0) {
        fail("read %s", path);
    }
    text[n] = '\0';
    for (line = text; line != NULL; line = strchr(line, '\n')) {
        line += *line == '\n';
        if (strncmp(line, label, len) == 0) {
            return strtoull(line + len, NULL, 10);
        }
    }
    fail("%s has no %s", path, label);
}

/** The kilobytes read from the disks since boot. */
static unsigned long long disk_read_kib(void)
{
    return proc_number("/proc/vmstat", "pgpgin ");
}

/** /proc/meminfo's Cached, in kilobytes. */
static unsigned long long cached_kib(void)
{
    return proc_number("/proc/meminfo", "Cached:");
}

/** Fill `buf` with block `n` of the large file. */
static void stamp(unsigned char *buf, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < BLOCK; i += 4) {
        uint32_t w = n * 2654435761u ^ i;

        memcpy(buf + i, &w, 4);
    }
    memcpy(buf, &n, 4);
}

/** Check `len` bytes at `buf`, whole blocks from block `first` on. */
static void check_blocks(const unsigned char *buf, size_t len, uint32_t first, const char *how)
{
    static unsigned char block[BLOCK];
    size_t at;

    for (at = 0; at < len; at += BLOCK) {
        stamp(block, first + (uint32_t)(at / BLOCK));
        if (memcmp(buf + at, block, BLOCK) != 0) {
            fail("twice: %s: block %u differs", how, first + (unsigned)(at / BLOCK));
        }
    }
}

/** Read the large file from byte `from` to byte `to` with `how`, 0 to 3:
 * read, pread64, readv or preadv2, checking each block. */
static void read_part(int fd, unsigned char *buf, uint64_t from, uint64_t to, int how)
{
    static const char *const names[] = {"read", "pread64", "readv", "preadv2"};
    uint64_t at = from;

    if ((how == 0 || how == 2) && lseek(fd, (off_t)from, SEEK_SET) != (off_t)from) {
        fail("twice: lseek: %s", strerror(errno));
    }
    while (at < to) {
        size_t want_n = to - at < CHUNK ? (size_t)(to - at) : CHUNK;
        struct iovec iov[2];
        ssize_t n;

        iov[0].iov_base = buf;
        iov[0].iov_len = want_n / 2;
        iov[1].iov_base = buf + want_n / 2;
        iov[1].iov_len = want_n - want_n / 2;
        if (how == 0) {
            n = read(fd, buf, want_n);
        } else if (how == 1) {
            n = pread(fd, buf, want_n, (off_t)at);
        } else if (how == 2) {
            n = readv(fd, iov, 2);
        } else {
            n = preadv2(fd, iov, 2, (off_t)at, 0);
        }
        if (n != (ssize_t)want_n) {
            fail("twice: %s at %llu gave %ld: %s", names[how], (unsigned long long)at, (long)n,
                 n < 0 ? strerror(errno) : "short");
        }
        check_blocks(buf, want_n, (uint32_t)(at / BLOCK), names[how]);
        at += want_n;
    }
}

/** The large file read twice, the second time from memory. */
static void twice(unsigned mib)
{
    uint64_t bytes = (uint64_t)mib << 20, at;
    unsigned long long cached0, cached1, in0, in1, in2;
    unsigned char *buf = malloc(CHUNK);
    int fd, how;

    if (buf == NULL) {
        fail("twice: no buffer");
    }
    fd = open(BIG, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail("twice: open %s: %s", BIG, strerror(errno));
    }
    for (at = 0; at < bytes; at += CHUNK) {
        size_t i;

        for (i = 0; i < CHUNK; i += BLOCK) {
            stamp(buf + i, (uint32_t)((at + i) / BLOCK));
        }
        if (write(fd, buf, CHUNK) != (ssize_t)CHUNK) {
            fail("twice: write at %llu: %s", (unsigned long long)at, strerror(errno));
        }
    }
    if (fsync(fd) != 0) {
        fail("twice: fsync: %s", strerror(errno));
    }
    cached0 = cached_kib();
    in0 = disk_read_kib();
    read_part(fd, buf, 0, bytes, 0);
    in1 = disk_read_kib();
    cached1 = cached_kib();
    for (how = 0; how < 4; how++) {
        read_part(fd, buf, bytes / 4 * (unsigned)how, bytes / 4 * (unsigned)(how + 1), how);
    }
    in2 = disk_read_kib();
    printf("pagecachemusl: twice: %u MiB: first read %llu KiB from the disks, second %llu KiB; "
           "Cached %llu kB, then %llu kB\n",
           mib, in1 - in0, in2 - in1, cached0, cached1);
    if (cached1 < cached0 + ((unsigned long long)mib << 10)) {
        fail("twice: Cached grew by %llu kB, less than the file's %u MiB", cached1 - cached0, mib);
    }
    if (in2 != in1) {
        fail("twice: the second read took %llu KiB from the disks", in2 - in1);
    }
    close(fd);
    if (unlink(BIG) != 0) {
        fail("twice: unlink: %s", strerror(errno));
    }
    free(buf);
}

/** The row of reads at and past the end of a file that the cache does not
 * hold yet. */
static void row_eof(void)
{
    static const uint64_t past[] = {0, 1, BLOCK, 1ull << 44, (1ull << 44) + BLOCK};
    unsigned char buf[BLOCK];
    unsigned long long before, after;
    size_t i;
    int fd;

    for (i = 0; i < ROW_SIZE; i++) {
        want[i] = (unsigned char)('a' + i % 23);
    }
    fd = open(FILE_, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, want, ROW_SIZE) != (ssize_t)ROW_SIZE || fsync(fd) != 0) {
        fail("eof: write the file: %s", strerror(errno));
    }
    before = cached_kib();
    for (i = 0; i < sizeof(past) / sizeof(past[0]); i++) {
        uint64_t at = ROW_SIZE + past[i];
        ssize_t n = pread(fd, buf, sizeof(buf), (off_t)at);

        if (n != 0) {
            fail("eof: pread64 at %llu of a file of %u bytes gave %ld: %s", (unsigned long long)at,
                 ROW_SIZE, (long)n, n < 0 ? strerror(errno) : "bytes");
        }
    }
    after = cached_kib();
    if (after != before) {
        fail("eof: Cached went from %llu kB to %llu kB", before, after);
    }
    close(fd);
}

/** Write the row's file whole, with `want` as its bytes, and read it,
 * which brings its pages into the cache. */
static int row_start(const char *row)
{
    unsigned char got[ROW_MOST];
    size_t i;
    int fd;

    for (i = 0; i < ROW_SIZE; i++) {
        want[i] = (unsigned char)('a' + i % 23);
    }
    want_size = ROW_SIZE;
    fd = open(row_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, want, want_size) != (ssize_t)want_size) {
        fail("%s: write the file: %s", row, strerror(errno));
    }
    if (pread(fd, got, sizeof(got), 0) != (ssize_t)want_size || memcmp(got, want, want_size)) {
        fail("%s: the first read differs", row);
    }
    return fd;
}

/** Read the row's file whole through `fd` and a new open, and require the
 * bytes and the size the change left. */
static void row_check(const char *row, int fd, const char *when)
{
    unsigned char got[ROW_MOST + 1];
    ssize_t n, total = 0;
    struct stat st;
    int fresh = open(row_path, O_RDONLY);
    size_t i;

    if (fresh < 0) {
        fail("%s: open: %s", row, strerror(errno));
    }
    while ((n = read(fresh, got + total, sizeof(got) - (size_t)total)) > 0) {
        total += n;
    }
    close(fresh);
    if (n < 0) {
        fail("%s: read %s: %s", row, when, strerror(errno));
    }
    if ((size_t)total != want_size) {
        fail("%s: %s the read gave %ld bytes, not %lu", row, when, (long)total,
             (unsigned long)want_size);
    }
    for (i = 0; i < want_size; i++) {
        if (got[i] != want[i]) {
            fail("%s: %s byte %lu is %u, not %u", row, when, (unsigned long)i, got[i], want[i]);
        }
    }
    if (fd >= 0 && (pread(fd, got, sizeof(got), 0) != (ssize_t)want_size ||
                    memcmp(got, want, want_size) != 0)) {
        fail("%s: %s pread through the row's descriptor differs", row, when);
    }
    if (stat(row_path, &st) != 0 || (size_t)st.st_size != want_size) {
        fail("%s: %s the size is not %lu", row, when, (unsigned long)want_size);
    }
}

/** Set `len` bytes of `want` at `at` to `c`, growing it to `at + len`. */
static void expect(size_t at, size_t len, int c)
{
    memset(want + at, c, len);
    if (at + len > want_size) {
        want_size = at + len;
    }
}

/** Resize `want` to `size`, with zeros past the old size. */
static void expect_size(size_t size)
{
    if (size > want_size) {
        memset(want + want_size, 0, size - want_size);
    }
    want_size = size;
}

/** The rows of write, pwrite and the vectored writes. */
static void rows_write(void)
{
    unsigned char a[3000], b[2500];
    struct iovec iov[2];
    int fd;

    memset(a, 'A', sizeof(a));
    memset(b, 'B', sizeof(b));
    iov[0].iov_base = a;
    iov[0].iov_len = sizeof(a);
    iov[1].iov_base = b;
    iov[1].iov_len = sizeof(b);

    fd = row_start("write");
    if (lseek(fd, 100, SEEK_SET) != 100 || write(fd, a, 50) != 50) {
        fail("write: %s", strerror(errno));
    }
    expect(100, 50, 'A');
    row_check("write", fd, "after write");
    close(fd);

    fd = row_start("pwrite");
    if (pwrite(fd, b, sizeof(b), 4000) != (ssize_t)sizeof(b)) {
        fail("pwrite: %s", strerror(errno));
    }
    expect(4000, sizeof(b), 'B');
    row_check("pwrite", fd, "after pwrite");
    close(fd);

    fd = row_start("writev");
    if (lseek(fd, 5000, SEEK_SET) != 5000 || writev(fd, iov, 2) != 5500) {
        fail("writev: %s", strerror(errno));
    }
    expect(5000, sizeof(a), 'A');
    expect(8000, sizeof(b), 'B');
    row_check("writev", fd, "after writev");
    close(fd);

    fd = row_start("pwritev");
    if (pwritev(fd, iov, 2, 8190) != 5500) {
        fail("pwritev: %s", strerror(errno));
    }
    expect(8190, sizeof(a), 'A');
    expect(11190, sizeof(b), 'B');
    row_check("pwritev", fd, "after pwritev");
    close(fd);

    fd = row_start("pwritev2");
    if (pwritev2(fd, iov, 1, 12000, 0) != (ssize_t)sizeof(a)) {
        fail("pwritev2: %s", strerror(errno));
    }
    expect(12000, sizeof(a), 'A');
    row_check("pwritev2", fd, "after pwritev2");
    close(fd);
}

/** The rows of truncate, ftruncate and fallocate. */
static void rows_size(void)
{
    int fd;

    fd = row_start("truncate");
    if (truncate(FILE_, 6000) != 0) {
        fail("truncate: down: %s", strerror(errno));
    }
    expect_size(6000);
    row_check("truncate", fd, "after truncate down");
    if (truncate(FILE_, ROW_SIZE) != 0) {
        fail("truncate: up: %s", strerror(errno));
    }
    expect_size(ROW_SIZE);
    row_check("truncate", fd, "after truncate up");
    close(fd);

    fd = row_start("ftruncate");
    if (ftruncate(fd, BLOCK) != 0) {
        fail("ftruncate: down: %s", strerror(errno));
    }
    expect_size(BLOCK);
    row_check("ftruncate", fd, "after ftruncate down");
    if (ftruncate(fd, ROW_SIZE) != 0) {
        fail("ftruncate: up: %s", strerror(errno));
    }
    expect_size(ROW_SIZE);
    row_check("ftruncate", fd, "after ftruncate up");
    close(fd);

    fd = row_start("fallocate");
    if (fallocate(fd, 0, ROW_SIZE, 2 * BLOCK) != 0) {
        fail("fallocate: %s", strerror(errno));
    }
    expect_size(ROW_SIZE + 2 * BLOCK);
    row_check("fallocate", fd, "after fallocate");
    close(fd);
}

/** The rows of copy_file_range, sendfile and splice into the file. */
static void rows_copy(void)
{
    unsigned char s[2 * BLOCK];
    off_t from, to;
    int src, fd, pipefd[2];

    memset(s, 'S', sizeof(s));
    src = open(SRC, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (src < 0 || write(src, s, sizeof(s)) != (ssize_t)sizeof(s)) {
        fail("copy: write the source: %s", strerror(errno));
    }

    fd = row_start("copy_file_range");
    from = 0;
    to = 1000;
    if (copy_file_range(src, &from, fd, &to, 3000, 0) != 3000) {
        fail("copy_file_range: %s", strerror(errno));
    }
    expect(1000, 3000, 'S');
    row_check("copy_file_range", fd, "after copy_file_range");
    close(fd);

    fd = row_start("sendfile");
    from = 0;
    if (lseek(fd, 7000, SEEK_SET) != 7000 || sendfile(fd, src, &from, 2000) != 2000) {
        fail("sendfile: %s", strerror(errno));
    }
    expect(7000, 2000, 'S');
    row_check("sendfile", fd, "after sendfile");
    close(fd);

    fd = row_start("splice");
    to = 9000;
    if (pipe(pipefd) != 0 || write(pipefd[1], s, 1500) != 1500 ||
        splice(pipefd[0], NULL, fd, &to, 1500, 0) != 1500) {
        fail("splice: %s", strerror(errno));
    }
    close(pipefd[0]);
    close(pipefd[1]);
    expect(9000, 1500, 'S');
    row_check("splice", fd, "after splice");
    close(fd);
    close(src);
    if (unlink(SRC) != 0) {
        fail("copy: unlink the source: %s", strerror(errno));
    }
}

/** The row of a shared mapping's stores, read while it is mapped, after
 * msync and after munmap. */
static void row_mmap(void)
{
    unsigned char *map;
    int fd = row_start("mmap");

    map = mmap(NULL, ROW_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fail("mmap: %s", strerror(errno));
    }
    memset(map + BLOCK, 'M', 10);
    memset(map + 3 * BLOCK + 50, 'M', 50);
    expect(BLOCK, 10, 'M');
    expect(3 * BLOCK + 50, 50, 'M');
    row_check("mmap", fd, "before msync");
    if (msync(map, ROW_SIZE, MS_SYNC) != 0) {
        fail("mmap: msync: %s", strerror(errno));
    }
    row_check("mmap", fd, "after msync");
    memset(map + 2 * BLOCK, 'N', 20);
    expect(2 * BLOCK, 20, 'N');
    if (munmap(map, ROW_SIZE) != 0) {
        fail("mmap: munmap: %s", strerror(errno));
    }
    row_check("mmap", fd, "after munmap");
    close(fd);
}

/** Read or, with `out` set, write the sector at byte `at` of the device
 * `dev` to or from `sector`, by lseek and a transfer of the whole sector,
 * as a device's node takes them. */
static void sector_io(int dev, uint64_t at, unsigned char *sector, int out)
{
    ssize_t n;

    if (lseek(dev, (off_t)at, SEEK_SET) != (off_t)at) {
        fail("unmount: seek the device to %llu: %s", (unsigned long long)at, strerror(errno));
    }
    n = out ? write(dev, sector, SECTOR) : read(dev, sector, SECTOR);
    if (n != (ssize_t)SECTOR) {
        fail("unmount: %s the device at %llu: %s", out ? "write" : "read", (unsigned long long)at,
             n < 0 ? strerror(errno) : "short");
    }
}

/** A little-endian word of `len` bytes at `at` of the device `dev`, inside
 * one sector. */
static uint32_t volume_word(int dev, uint64_t at, unsigned len)
{
    unsigned char sector[SECTOR], *b = sector + at % SECTOR;
    uint32_t w = 0;

    sector_io(dev, at - at % SECTOR, sector, 0);
    while (len-- > 0) {
        w = w << 8 | b[len];
    }
    return w;
}

/** The byte of the volume `dev` where byte `offset` of inode `ino`'s data
 * lies, by its superblock, group descriptor and direct block pointer. */
static uint64_t volume_byte(int dev, uint32_t ino, uint32_t offset)
{
    uint32_t bsize = 1024u << volume_word(dev, 1024 + 24, 4);
    uint32_t first = volume_word(dev, 1024 + 20, 4);
    uint32_t per_group = volume_word(dev, 1024 + 40, 4);
    uint32_t isize = volume_word(dev, 1024 + 76, 4) >= 1 ? volume_word(dev, 1024 + 88, 2) : 128;
    uint32_t group = (ino - 1) / per_group, index = (ino - 1) % per_group;
    uint32_t table = volume_word(dev, (uint64_t)(first + 1) * bsize + group * 32u + 8, 4);
    uint64_t inode = (uint64_t)table * bsize + (uint64_t)index * isize;
    uint32_t blk = volume_word(dev, inode + 40 + 4u * (offset / bsize), 4);

    if (offset / bsize >= 12 || blk == 0) {
        fail("unmount: byte %u of inode %u has no direct block", offset, ino);
    }
    return (uint64_t)blk * bsize + offset % bsize;
}

/** Copy the ext2 volume SMALL, as long as its superblock says, to COPY on a
 * new tmpfs at TMP, attach the copy to LOOP and mount it at LOOPDIR. */
static void small_mount(void)
{
    unsigned char buf[64 * SECTOR];
    uint64_t bytes, done;
    int in, out, ctl;

    in = open(small, O_RDONLY);
    if (in < 0) {
        fail("unmount: open %s: %s", small, strerror(errno));
    }
    bytes = (uint64_t)volume_word(in, 1024 + 4, 4) * (1024u << volume_word(in, 1024 + 24, 4));
    if ((mkdir(TMP, 0755) != 0 && errno != EEXIST) || mount("tmpfs", TMP, "tmpfs", 0, NULL) != 0) {
        fail("unmount: mount a tmpfs at %s: %s", TMP, strerror(errno));
    }
    out = open(COPY, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0 || lseek(in, 0, SEEK_SET) != 0) {
        fail("unmount: start the copy: %s", strerror(errno));
    }
    for (done = 0; done < bytes; done += sizeof(buf)) {
        if (read(in, buf, sizeof(buf)) != (ssize_t)sizeof(buf) ||
            write(out, buf, sizeof(buf)) != (ssize_t)sizeof(buf)) {
            fail("unmount: copy %s at %llu: %s", small, (unsigned long long)done, strerror(errno));
        }
    }
    close(in);
    close(out);
    ctl = open("/dev/loop-control", O_WRONLY);
    if (ctl < 0 || write(ctl, "attach 0 " COPY, strlen("attach 0 " COPY)) < 0) {
        fail("unmount: attach %s to %s: %s", COPY, LOOP, strerror(errno));
    }
    close(ctl);
    if ((mkdir(LOOPDIR, 0755) != 0 && errno != EEXIST) ||
        mount(LOOP, LOOPDIR, "ext2", 0, NULL) != 0) {
        fail("unmount: mount %s: %s", LOOP, strerror(errno));
    }
}

/** Unmount LOOPDIR, detach LOOP and unmount the tmpfs at TMP. */
static void small_unmount(void)
{
    int ctl;

    if (umount(LOOPDIR) != 0) {
        fail("unmount: clean up: umount %s: %s", LOOPDIR, strerror(errno));
    }
    ctl = open("/dev/loop-control", O_WRONLY);
    if (ctl < 0 || write(ctl, "detach 0", 8) < 0) {
        fail("unmount: clean up: detach %s: %s", LOOP, strerror(errno));
    }
    close(ctl);
    if (umount(TMP) != 0) {
        fail("unmount: clean up: umount %s: %s", TMP, strerror(errno));
    }
}

/** The row of a change made through the volume's node while it is
 * unmounted, on the small volume. */
static void row_unmount(void)
{
    unsigned char sector[SECTOR];
    struct stat st;
    uint64_t at;
    int dev, fd;

    small_mount();
    row_path = LOOPDIR "/co.bin";
    fd = row_start("unmount");
    if (fstat(fd, &st) != 0) {
        fail("unmount: fstat: %s", strerror(errno));
    }
    close(fd);
    if (umount(LOOPDIR) != 0) {
        fail("unmount: umount: %s", strerror(errno));
    }
    dev = open(LOOP, O_RDWR);
    if (dev < 0) {
        fail("unmount: open %s: %s", LOOP, strerror(errno));
    }
    at = volume_byte(dev, (uint32_t)st.st_ino, BLOCK + 8);
    sector_io(dev, at - at % SECTOR, sector, 0);
    memset(sector + at % SECTOR, 'U', 64);
    sector_io(dev, at - at % SECTOR, sector, 1);
    if (fsync(dev) != 0) {
        fail("unmount: fsync %s: %s", LOOP, strerror(errno));
    }
    close(dev);
    if (mount(LOOP, LOOPDIR, "ext2", 0, NULL) != 0) {
        fail("unmount: mount again: %s", strerror(errno));
    }
    expect(BLOCK + 8, 64, 'U');
    row_check("unmount", -1, "after the remount");
    if (unlink(row_path) != 0) {
        fail("unmount: unlink: %s", strerror(errno));
    }
    small_unmount();
    row_path = FILE_;
}

int main(int argc, char **argv)
{
    unsigned mib;

    if (argc != 4 || (mib = (unsigned)strtoul(argv[2], NULL, 10)) == 0) {
        fail("usage: pagecachemusl NODE MIB SMALL");
    }
    node = argv[1];
    small = argv[3];
    if (mkdir(DIR, 0755) != 0 && errno != EEXIST) {
        fail("mkdir %s: %s", DIR, strerror(errno));
    }
    if (mount(node, DIR, "ext2", 0, NULL) != 0) {
        fail("mount %s: %s", node, strerror(errno));
    }
    twice(mib);
    row_eof();
    rows_write();
    rows_size();
    rows_copy();
    row_mmap();
    if (unlink(FILE_) != 0 || umount(DIR) != 0) {
        fail("clean up: %s", strerror(errno));
    }
    row_unmount();
    printf("PAGECACHEMUSL OK\n");
    return 0;
}
