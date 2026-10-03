/**
 * Files past 4 GiB: `bigfileprobe MODE NODE TYPE [ARG]`.
 *
 * The probe mounts NODE at `/big` as TYPE and runs one of:
 *
 * `write MIB`: writes `/big/big.bin` of MIB MiB, each 4 KiB block filled
 * with its own number, then stats it, seeks to its end and back, reads
 * blocks near the end and at the 4 GiB line, opens a second descriptor
 * and a duplicate of the first to see one offset shared and one apart,
 * truncates it to three quarters, which may take a few disk commands per
 * pointer block freed and not one per block, appends a mebibyte at the
 * end and reads that back, and reports the write's rate.
 *
 * `verify MIB`: after a remount, reads the whole of `/big/big.bin` and
 * checks every block's number against the size `write` left, three
 * quarters of MIB plus one mebibyte.
 *
 * `sparse`: on a format with holes, writes a mebibyte astride the 4 GiB
 * line of a new file, half a mebibyte on each side, stats it, reads the
 * mebibyte back and a block in the hole, truncates it to a block past the
 * line and removes it; seconds, where `write` at that size is an hour.
 *
 * `limit OFFSET`: seeks a new file to OFFSET, a decimal number of bytes,
 * and requires the one-byte write there to be refused with `EFBIG`; the
 * kernel log has the refusal by name.
 *
 * Prints "BIGFILE OK (mode)" or "BIGFILE FAIL: reason". Freestanding;
 * direct system calls (probe.h).
 */

#include "probe.h"

typedef unsigned long long u64;

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0100
#define O_TRUNC  01000

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define EFBIG 27

#define BLOCK 4096u
#define CHUNK (1u << 20)

static void sys_exit(int status) __attribute__((noreturn));
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

static void put(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
}

/* `v / 10` by shifting and subtracting, since a freestanding program has no
   64-bit division; the remainder goes to `*rem`. */
static u64 div10(u64 v, unsigned *rem)
{
    u64 q = 0, r = 0;
    int i;

    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((v >> i) & 1);
        q <<= 1;
        if (r >= 10) {
            r -= 10;
            q |= 1;
        }
    }
    *rem = (unsigned)r;
    return q;
}

static void put_u64(u64 v)
{
    char digits[24];
    unsigned rem;
    int i = 23;

    digits[i] = '\0';
    do {
        v = div10(v, &rem);
        digits[--i] = (char)('0' + rem);
    } while (v);
    put(digits + i);
}

/* The clock in seconds: times(2) answers ticks of a hundredth. */
static unsigned now(void)
{
    return (unsigned)syscall1(SYS_TIMES, 0) / 100u;
}

static void fail(const char *why) __attribute__((noreturn));
static void fail(const char *why)
{
    put("BIGFILE FAIL: ");
    put(why);
    put("\n");
    sys_exit(1);
}

static void fail_with(const char *why, long long n) __attribute__((noreturn));
static void fail_with(const char *why, long long n)
{
    put("BIGFILE FAIL: ");
    put(why);
    put(" (got ");
    if (n < 0) {
        put("-");
        n = -n;
    }
    put_u64((u64)n);
    put(")\n");
    sys_exit(1);
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static u64 parse(const char *s)
{
    u64 v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (u64)(*s - '0');
        s++;
    }
    return v;
}

/* llseek(2): the resolved offset, or a negative errno. */
static long long seek(int fd, long long off, int whence)
{
    long long result = 0;
    int rc = syscall5(SYS_LLSEEK, fd, (int)(unsigned)((u64)off >> 32), (int)(unsigned)(u64)off,
                      (long)&result, whence);

    return rc < 0 ? rc : result;
}

/* fstat64's st_size, or a negative errno. */
static long long size_of(int fd)
{
    unsigned char st[104];
    u64 size = 0;
    int rc, i;

    for (i = 0; i < (int)sizeof(st); i++) {
        st[i] = 0;
    }
    rc = syscall2(SYS_FSTAT, fd, (long)st);
    if (rc < 0) {
        return rc;
    }
    for (i = 7; i >= 0; i--) {
        size = (size << 8) | st[44 + i];
    }
    return (long long)size;
}

static int ftruncate64(int fd, u64 length)
{
    return syscall3(SYS_FTRUNCATE64, fd, (int)(unsigned)length, (int)(unsigned)(length >> 32));
}

static unsigned char chunk[CHUNK] __attribute__((aligned(4096)));

/* Fill `chunk` with the numbers of the blocks from `first` on. */
static void fill(u64 first)
{
    unsigned i;

    for (i = 0; i < CHUNK / 4; i++) {
        unsigned b = (unsigned)(first + i / (BLOCK / 4));

        ((unsigned *)chunk)[i] = b;
    }
}

/* Whether `len` bytes of `chunk` hold the numbers of the blocks from
   `first`; `len` is whole blocks. */
static int holds(u64 first, unsigned len)
{
    unsigned i;

    for (i = 0; i < len / 4; i++) {
        if (((unsigned *)chunk)[i] != (unsigned)(first + i / (BLOCK / 4))) {
            return 0;
        }
    }
    return 1;
}

static void read_full(int fd, unsigned len, const char *what)
{
    unsigned done = 0;

    while (done < len) {
        int n = syscall3(SYS_READ, fd, (long)(chunk + done), (int)(len - done));

        if (n <= 0) {
            fail_with(what, n);
        }
        done += (unsigned)n;
    }
}

static void write_full(int fd, unsigned len, const char *what)
{
    unsigned done = 0;

    while (done < len) {
        int n = syscall3(SYS_WRITE, fd, (long)(chunk + done), (int)(len - done));

        if (n <= 0) {
            fail_with(what, n);
        }
        done += (unsigned)n;
    }
}

/* One block read at byte `at` and checked. */
static void check_block(int fd, u64 at, const char *what)
{
    if (seek(fd, (long long)at, SEEK_SET) != (long long)at) {
        fail(what);
    }
    read_full(fd, BLOCK, what);
    if (!holds(at >> 12, BLOCK)) {
        fail(what);
    }
}

/* The disk layer's command count from /proc/disk into `out`, a total
   since the boot that outgrows an int on a long run; -1 when the file
   cannot be read or has no such line. */
static int disk_commands(u64 *out)
{
    static char text[2048];
    const char *label = "disk-commands ";
    int fd = syscall3(SYS_OPEN, (long)"/proc/disk", O_RDONLY, 0);
    int n, i, j;

    if (fd < 0) {
        return -1;
    }
    n = syscall3(SYS_READ, fd, (long)text, sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        return -1;
    }
    text[n] = 0;
    for (i = 0; text[i]; i++) {
        for (j = 0; label[j] && text[i + j] == label[j]; j++) {
        }
        if (!label[j]) {
            u64 v = 0;

            i += j;
            while (text[i] >= '0' && text[i] <= '9') {
                v = v * 10 + (u64)(text[i] - '0');
                i++;
            }
            *out = v;
            return 0;
        }
    }
    return -1;
}

/*
 * The write mode: the file is written whole, read back, shrunk to three
 * quarters and grown by a mebibyte. The shrink frees a quarter of the file,
 * and the disk's commands around it are counted: a driver that frees its
 * blocks in a batch needs a few requests per pointer block of 4 MiB and a
 * few for the bitmaps, and one that writes each block's pointer, bit and
 * descriptor through needs thousands, which the bound refuses.
 */
static void write_mode(u64 mib)
{
    u64 total = mib << 20, done = 0, three_quarters = (total >> 2) * 3;
    /* The 4 GiB line, where a 32-bit offset would wrap; a smaller file, for
       a measurement, is probed at its middle instead. */
    u64 line = total > ((u64)1 << 32) + CHUNK ? (u64)1 << 32 : total >> 1;
    int fd, fd2, fd3;
    u64 before, after;
    unsigned t0, t1;
    long long got;

    fd = syscall3(SYS_OPEN, (long)"/big/big.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail_with("creating big.bin", fd);
    }
    t0 = now();
    while (done < total) {
        fill(done >> 12);
        write_full(fd, CHUNK, "a write of the file");
        done += CHUNK;
        if ((done & ((1u << 30) - 1)) == 0) {
            put("bigfile: written ");
            put_u64(done >> 30);
            put(" GiB\n");
        }
    }
    if (syscall1(SYS_FSYNC, fd) != 0) {
        fail("fsync");
    }
    t1 = now();
    put("bigfile: rate: ");
    put_u64(t1 > t0 ? (unsigned)mib / (t1 - t0) : (unsigned)mib);
    put(" MiB/s\n");
    put("bigfile: synced\n");
    got = size_of(fd);
    if (got != (long long)total) {
        fail_with("the size after the write", got);
    }
    put("bigfile: sized\n");
    got = seek(fd, 0, SEEK_END);
    if (got != (long long)total) {
        fail_with("a seek to the end", got);
    }
    got = seek(fd, -(long long)BLOCK, SEEK_CUR);
    if (got != (long long)(total - BLOCK)) {
        fail_with("a seek back from the end", got);
    }
    read_full(fd, BLOCK, "the last block");
    if (!holds((total >> 12) - 1, BLOCK)) {
        fail("the last block's contents");
    }
    check_block(fd, line, "the block at the 4 GiB line");
    check_block(fd, line - BLOCK, "the block before the 4 GiB line");
    check_block(fd, total - 3 * CHUNK + BLOCK, "a block near the end");
    /* Two descriptions: the second open has its own offset, the duplicate
       shares the first's. */
    fd2 = syscall3(SYS_OPEN, (long)"/big/big.bin", O_RDONLY, 0);
    if (fd2 < 0) {
        fail_with("a second open", fd2);
    }
    if (seek(fd, (long long)line, SEEK_SET) != (long long)line) {
        fail("the seek of the first descriptor to the 4 GiB line");
    }
    got = seek(fd2, 0, SEEK_CUR);
    if (got != 0) {
        fail_with("the second open's own offset", got);
    }
    fd3 = syscall1(SYS_DUP, fd);
    if (fd3 < 0) {
        fail_with("dup", fd3);
    }
    read_full(fd3, BLOCK, "a read through the duplicate");
    if (!holds(line >> 12, BLOCK)) {
        fail("the duplicate's read at the 4 GiB line");
    }
    got = seek(fd, 0, SEEK_CUR);
    if (got != (long long)(line + BLOCK)) {
        fail_with("the shared offset after the duplicate's read", got);
    }
    read_full(fd, BLOCK, "a read after the duplicate's");
    if (!holds((line >> 12) + 1, BLOCK)) {
        fail("the first descriptor's read after the duplicate's");
    }
    syscall1(SYS_CLOSE, fd3);
    syscall1(SYS_CLOSE, fd2);
    /* Three quarters, then a mebibyte appended at the end. */
    put("bigfile: read back\n");
    if (disk_commands(&before) != 0) {
        fail("reading /proc/disk before the truncate");
    }
    if (ftruncate64(fd, three_quarters) != 0) {
        fail("the truncate to three quarters");
    }
    if (disk_commands(&after) != 0) {
        fail("reading /proc/disk after the truncate");
    }
    put("bigfile: truncated: ");
    put_u64(after - before);
    put(" commands\n");
    if (after - before > 8 * ((total - three_quarters) >> 22) + 64) {
        fail_with("the truncate's commands", (int)(after - before));
    }
    got = size_of(fd);
    if (got != (long long)three_quarters) {
        fail_with("the size after the truncate", got);
    }
    got = seek(fd, 0, SEEK_END);
    if (got != (long long)three_quarters) {
        fail_with("a seek to the new end", got);
    }
    fill(three_quarters >> 12);
    write_full(fd, CHUNK, "the append");
    got = size_of(fd);
    if (got != (long long)(three_quarters + CHUNK)) {
        fail_with("the size after the append", got);
    }
    check_block(fd, three_quarters + CHUNK - BLOCK, "the appended tail");
    check_block(fd, three_quarters - BLOCK, "the block before the truncate point");
    if (syscall1(SYS_FSYNC, fd) != 0) {
        fail("fsync after the append");
    }
    put("bigfile: appended and synced\n");
    syscall1(SYS_CLOSE, fd);
}

static void verify_mode(u64 mib)
{
    u64 total = ((mib << 20) >> 2) * 3 + CHUNK, done = 0;
    int fd = syscall3(SYS_OPEN, (long)"/big/big.bin", O_RDONLY, 0);
    long long got;

    if (fd < 0) {
        fail_with("opening big.bin", fd);
    }
    got = size_of(fd);
    if (got != (long long)total) {
        fail_with("the size after the remount", got);
    }
    while (done < total) {
        read_full(fd, CHUNK, "a read of the file");
        if (!holds(done >> 12, CHUNK)) {
            put("bigfile: wrong block at ");
            put_u64(done);
            put("\n");
            fail("the contents after the remount");
        }
        done += CHUNK;
    }
    got = syscall3(SYS_READ, fd, (long)chunk, (int)BLOCK);
    if (got != 0) {
        fail_with("a read at the end", got);
    }
    syscall1(SYS_CLOSE, fd);
}

static void sparse_mode(void)
{
    u64 line = (u64)1 << 32, at = line - CHUNK / 2;
    int fd = syscall3(SYS_OPEN, (long)"/big/sparse.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    long long got;

    if (fd < 0) {
        fail_with("creating sparse.bin", fd);
    }
    if (seek(fd, (long long)at, SEEK_SET) != (long long)at) {
        fail("the seek to the 4 GiB line");
    }
    fill(at >> 12);
    write_full(fd, CHUNK, "the write astride the 4 GiB line");
    if (syscall1(SYS_FSYNC, fd) != 0) {
        fail("fsync");
    }
    got = size_of(fd);
    if (got != (long long)(at + CHUNK)) {
        fail_with("the size past the 4 GiB line", got);
    }
    if (seek(fd, (long long)at, SEEK_SET) != (long long)at) {
        fail("the seek back to the write");
    }
    read_full(fd, CHUNK, "the read astride the line");
    if (!holds(at >> 12, CHUNK)) {
        fail("the bytes astride the 4 GiB line");
    }
    if (seek(fd, (long long)((u64)1 << 30), SEEK_SET) != (long long)((u64)1 << 30)) {
        fail("the seek into the hole");
    }
    read_full(fd, BLOCK, "a read in the hole");
    if (!holds(0, BLOCK)) {
        fail("the hole does not read as zeros");
    }
    if (ftruncate64(fd, line + BLOCK) != 0) {
        fail("the truncate to a block past the line");
    }
    got = size_of(fd);
    if (got != (long long)(line + BLOCK)) {
        fail_with("the size after the truncate", got);
    }
    syscall1(SYS_CLOSE, fd);
    syscall1(SYS_UNLINK, (long)"/big/sparse.bin");
}

static void limit_mode(u64 offset)
{
    int fd = syscall3(SYS_OPEN, (long)"/big/limit.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    long long got;
    int rc;

    if (fd < 0) {
        fail_with("creating limit.bin", fd);
    }
    got = seek(fd, (long long)offset, SEEK_SET);
    if (got != (long long)offset) {
        fail_with("the seek to the offset", got);
    }
    rc = syscall3(SYS_WRITE, fd, (long)chunk, 1);
    if (rc != -EFBIG) {
        fail_with("the write at the offset was not refused with EFBIG", rc);
    }
    got = size_of(fd);
    if (got != 0) {
        fail_with("the file grew", got);
    }
    syscall1(SYS_CLOSE, fd);
    syscall1(SYS_UNLINK, (long)"/big/limit.bin");
}

void probe_main(unsigned int *sp);

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(unsigned int *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    const char *mode;
    int rc;

    if (argc < 5) {
        fail("usage: bigfileprobe MODE NODE TYPE ARG");
    }
    mode = argv[1];
    syscall2(SYS_MKDIR, (long)"/big", 0755);
    rc = syscall5(SYS_MOUNT, (long)argv[2], (long)"/big", (long)argv[3], 0, 0);
    if (rc != 0) {
        fail_with("mounting the volume", rc);
    }
    if (streq(mode, "write")) {
        write_mode(parse(argv[4]));
    } else if (streq(mode, "verify")) {
        verify_mode(parse(argv[4]));
    } else if (streq(mode, "sparse")) {
        sparse_mode();
    } else if (streq(mode, "limit")) {
        limit_mode(parse(argv[4]));
    } else {
        fail("unknown mode");
    }
    rc = syscall2(SYS_UMOUNT2, (long)"/big", 0);
    if (rc != 0) {
        fail_with("unmount", rc);
    }
    put("BIGFILE OK (");
    put(mode);
    put(")\n");
    sys_exit(0);
}
