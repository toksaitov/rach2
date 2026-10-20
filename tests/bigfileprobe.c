/**
 * Files past 4 GiB: `bigfileprobe MODE NODE TYPE ARG`; `sparse` ignores ARG.
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
 * `write-verify MIB`: `write`, an unmount and a new mount, and `verify`, for
 * a run that cannot boot again between the two.
 *
 * A native run gives the volume as a loop device (probe_args.h): NODE `-` is
 * the first device that `loops=DEVICE,...` names, and `dir=PATH` is the
 * mount point in place of `/big`. On a loop device the truncate's bound
 * counts the device's own writes, from /proc/loop, in place of the commands
 * of every disk, from /proc/disk, which the other processes of a running
 * machine move.
 *
 * Prints "BIGFILE OK (mode)" or "BIGFILE FAIL: reason". Freestanding;
 * direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_args.h"

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

/** exit(2) with `status`; does not return. */
static void sys_exit(int status) __attribute__((noreturn));
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

/** Write `s` to standard output. */
static void put(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
}

/** `v / 10` by shifting and subtracting, since an i386 64-bit division
 * calls libgcc, which a probe does not link; the remainder goes to `*rem`. */
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

/** Write `v` in decimal. */
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

/** The clock in seconds: times(2) answers ticks of a hundredth. */
static unsigned now(void)
{
    return (unsigned)syscall1(SYS_TIMES, 0) / 100u;
}

/* The mount point, the paths of the three files on it, and the name of the
 * volume's loop device, such as "loop2", or "" when the volume is not on
 * one. */
static const char *mount_dir;
static char big_path[128], sparse_path[128], limit_path[128];
static char loop_name[16];

/** Print "BIGFILE FAIL: " and `why`, and exit 1. */
static void fail(const char *why) __attribute__((noreturn));
static void fail(const char *why)
{
    put("BIGFILE FAIL: ");
    put(why);
    put("\n");
    sys_exit(1);
}

/** Print "BIGFILE FAIL: ", `why` and the answer `n` it got, and exit 1. */
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

/** Whether the strings `a` and `b` are equal. */
static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** The decimal number at the start of `s`, 0 when it has none. */
static u64 parse(const char *s)
{
    u64 v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (u64)(*s - '0');
        s++;
    }
    return v;
}

/** llseek(2): the resolved offset, or a negative errno. */
static long long seek(int fd, long long off, int whence)
{
    long long result = 0;
    int rc = probe_llseek(fd, off, whence, &result);

    return rc < 0 ? rc : result;
}

/** fstat's st_size, or a negative errno. */
static long long size_of(int fd)
{
    unsigned char st[KSTAT_SIZE];
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
        size = (size << 8) | st[KSTAT_SIZE_AT + i];
    }
    return (long long)size;
}

/** ftruncate to the 64-bit `length`: 0, or a negated errno. */
static int ftruncate64(int fd, u64 length)
{
    return probe_ftruncate64(fd, (long long)length);
}

static unsigned char chunk[CHUNK] __attribute__((aligned(4096)));

/** Fill `chunk` with the numbers of the blocks from `first` on. */
static void fill(u64 first)
{
    unsigned i;

    for (i = 0; i < CHUNK / 4; i++) {
        unsigned b = (unsigned)(first + i / (BLOCK / 4));

        ((unsigned *)chunk)[i] = b;
    }
}

/** Whether `len` bytes of `chunk` hold the numbers of the blocks from
 * `first`; `len` is whole blocks. */
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

/** Read `len` bytes into `chunk`; fail, naming `what`, at an error or the
 * end. */
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

/** Write `len` bytes of `chunk`; fail, naming `what`, on an error. */
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

/** Read the block at byte `at` and require its number; fail, naming `what`,
 * otherwise. */
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

/** Copy `dir` and `name` into `out`, of 128 bytes, as one string, cut short
 * where `out` is full. */
static void join(char *out, const char *dir, const char *name)
{
    int n = 0;

    while (*dir && n < 127) {
        out[n++] = *dir++;
    }
    while (*name && n < 127) {
        out[n++] = *name++;
    }
    out[n] = '\0';
}

/** The writes of the loop device `loop_name` from /proc/loop into `out`: 0,
 * or -1 when the file cannot be read or has no line for the device. */
static int loop_writes(u64 *out)
{
    static char text[1024];
    const char *label = "writes ";
    int fd = syscall3(SYS_OPEN, (long)"/proc/loop", O_RDONLY, 0);
    int n, i, j, k;

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
        if (i > 0 && text[i - 1] != '\n') {
            continue;
        }
        for (k = 0; loop_name[k] && text[i + k] == loop_name[k]; k++) {
        }
        if (loop_name[k] || text[i + k] != ' ') {
            continue;
        }
        for (i += k; text[i] && text[i] != '\n'; i++) {
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
    return -1;
}

/** The disk layer's command count from /proc/disk into `out`, a total since
 * the boot that outgrows an int on a long run: 0, or -1 when the file cannot
 * be read or has no such line. */
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

/** The count the truncate's bound judges: the writes of the loop device
 * `loop_name` names, or the disk's commands when it names none. Fails with
 * `loop_why` or `disk_why` when the count cannot be read. */
static u64 device_count(const char *loop_why, const char *disk_why)
{
    u64 n;

    if (loop_name[0] ? loop_writes(&n) != 0 : disk_commands(&n) != 0) {
        fail(loop_name[0] ? loop_why : disk_why);
    }
    return n;
}

/**
 * The write mode: the file is written whole, read back, shrunk to three
 * quarters and grown by a mebibyte. The shrink frees a quarter of the file,
 * and the disk's commands around it are counted: a driver that frees its
 * blocks in a batch needs a few requests per pointer block of 4 MiB and a
 * few for the bitmaps, and one that writes each block's pointer, bit and
 * descriptor through needs thousands, which the bound refuses.
 *
 * The reads past the write fall at the 4 GiB line, where a 32-bit offset
 * would wrap, or at the middle of a file too small to reach it, as one
 * written for a measurement is.
 */
static void write_mode(u64 mib)
{
    u64 total = mib << 20, done = 0, three_quarters = (total >> 2) * 3;
    u64 line = total > ((u64)1 << 32) + CHUNK ? (u64)1 << 32 : total >> 1;
    int fd, fd2, fd3;
    u64 before, after;
    unsigned t0, t1;
    long long got;

    fd = syscall3(SYS_OPEN, (long)big_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
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
    fd2 = syscall3(SYS_OPEN, (long)big_path, O_RDONLY, 0);
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
    put("bigfile: read back\n");
    before = device_count("reading /proc/loop before the truncate",
                          "reading /proc/disk before the truncate");
    if (ftruncate64(fd, three_quarters) != 0) {
        fail("the truncate to three quarters");
    }
    after = device_count("reading /proc/loop after the truncate",
                         "reading /proc/disk after the truncate");
    put("bigfile: truncated: ");
    put_u64(after - before);
    put(loop_name[0] ? " writes\n" : " commands\n");
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

/** The verify mode: the file the write mode of `mib` MiB left, three
 * quarters of it and a mebibyte, read whole, every block's number checked,
 * and a read at its end that returns 0. */
static void verify_mode(u64 mib)
{
    u64 total = ((mib << 20) >> 2) * 3 + CHUNK, done = 0;
    int fd = syscall3(SYS_OPEN, (long)big_path, O_RDONLY, 0);
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

/** The sparse mode: a mebibyte written astride the 4 GiB line of a new file,
 * read back, a block of the hole read as zeros, and the file truncated to a
 * block past the line and removed. */
static void sparse_mode(void)
{
    u64 line = (u64)1 << 32, at = line - CHUNK / 2;
    int fd = syscall3(SYS_OPEN, (long)sparse_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
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
    syscall1(SYS_UNLINK, (long)sparse_path);
}

/** The limit mode: a one-byte write at `offset` of a new file must fail with
 * -EFBIG and leave the file empty. */
static void limit_mode(u64 offset)
{
    int fd = syscall3(SYS_OPEN, (long)limit_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
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
    syscall1(SYS_UNLINK, (long)limit_path);
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The node NODE names: NODE itself, or for `-` the first device of
 * `loops=`. Sets `loop_name` to the node's last component when that begins
 * with "loop". */
static const char *volume_node(const char *word)
{
    static char first[64];
    const char *last;
    int n = 0;

    if (streq(word, "-")) {
        const char *loops = probe_arg("loops", "");

        while (n < (int)sizeof(first) - 1 && loops[n] && loops[n] != ',') {
            first[n] = loops[n];
            n++;
        }
        first[n] = '\0';
        word = first;
    }
    for (last = word, n = 0; word[n]; n++) {
        if (word[n] == '/') {
            last = word + n + 1;
        }
    }
    if (last[0] == 'l' && last[1] == 'o' && last[2] == 'o' && last[3] == 'p') {
        for (n = 0; n < (int)sizeof(loop_name) - 1 && last[n]; n++) {
            loop_name[n] = last[n];
        }
        loop_name[n] = '\0';
    }
    return word;
}

/** Mount `node` at the mount point as `type`, failing with `what`. */
static void mount_volume(const char *node, const char *type, const char *what)
{
    int rc = syscall5(SYS_MOUNT, (long)node, (long)mount_dir, (long)type, 0, 0);

    if (rc != 0) {
        fail_with(what, rc);
    }
}

/** Unmount the mount point, failing with `what`. */
static void unmount_volume(const char *what)
{
    int rc = syscall2(SYS_UMOUNT2, (long)mount_dir, 0);

    if (rc != 0) {
        fail_with(what, rc);
    }
}

/** Mount NODE at the mount point as the type TYPE, run the mode MODE names
 * with ARG, unmount, and print "BIGFILE OK (mode)". */
void probe_main(unsigned long *sp)
{
    const char *mode, *node, *type, *arg;

    probe_args((int)sp[0], (char **)(sp + 1));
    if (probe_words() < 4) {
        fail("usage: bigfileprobe MODE NODE TYPE ARG");
    }
    mode = probe_word(1);
    node = volume_node(probe_word(2));
    type = probe_word(3);
    arg = probe_word(4);
    mount_dir = probe_arg("dir", "/big");
    join(big_path, mount_dir, "/big.bin");
    join(sparse_path, mount_dir, "/sparse.bin");
    join(limit_path, mount_dir, "/limit.bin");
    syscall2(SYS_MKDIR, (long)mount_dir, 0755);
    mount_volume(node, type, "mounting the volume");
    if (streq(mode, "write")) {
        write_mode(parse(arg));
    } else if (streq(mode, "verify")) {
        verify_mode(parse(arg));
    } else if (streq(mode, "write-verify")) {
        write_mode(parse(arg));
        unmount_volume("the unmount after the write");
        mount_volume(node, type, "mounting the volume again");
        verify_mode(parse(arg));
    } else if (streq(mode, "sparse")) {
        sparse_mode();
    } else if (streq(mode, "limit")) {
        limit_mode(parse(arg));
    } else {
        fail("unknown mode");
    }
    unmount_volume("unmount");
    put("BIGFILE OK (");
    put(mode);
    put(")\n");
    sys_exit(0);
}
