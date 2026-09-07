/**
 * Files past 4 GiB: `bigfileprobe MODE NODE TYPE [ARG]`.
 *
 * The probe mounts NODE at `/big` as TYPE and runs one of:
 *
 * `write MIB`: writes `/big/big.bin` of MIB MiB, each 4 KiB block filled
 * with its own number, then stats it, seeks to its end and back, reads
 * blocks near the end and at the 4 GiB line, opens a second descriptor
 * and a duplicate of the first to see one offset shared and one apart,
 * truncates it to three quarters, appends a mebibyte at the end and reads
 * that back, and reports the write's rate.
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
 * int 0x80.
 */

typedef unsigned int size_t;
typedef unsigned long long u64;

#define SYS_EXIT        1
#define SYS_READ        3
#define SYS_WRITE       4
#define SYS_OPEN        5
#define SYS_CLOSE       6
#define SYS_UNLINK      10
#define SYS_TIMES       43
#define SYS_MOUNT       21
#define SYS_MKDIR       39
#define SYS_DUP         41
#define SYS_UMOUNT2     52
#define SYS_FSYNC       118
#define SYS_LLSEEK      140
#define SYS_FTRUNCATE64 194
#define SYS_FSTAT64     197

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

static int sys1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static int sys2(int n, int a, int b)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static int sys5(int n, int a, int b, int c, int d, int e)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                         : "memory");
    return r;
}

static void sys_exit(int status) __attribute__((noreturn));
static void sys_exit(int status)
{
    sys1(SYS_EXIT, status);
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
    sys3(SYS_WRITE, 1, (int)s, (int)strlen(s));
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
    return (unsigned)sys1(SYS_TIMES, 0) / 100u;
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
    int rc = sys5(SYS_LLSEEK, fd, (int)(unsigned)((u64)off >> 32), (int)(unsigned)(u64)off,
                  (int)&result, whence);

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
    rc = sys2(SYS_FSTAT64, fd, (int)st);
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
    return sys3(SYS_FTRUNCATE64, fd, (int)(unsigned)length, (int)(unsigned)(length >> 32));
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
        int n = sys3(SYS_READ, fd, (int)(chunk + done), (int)(len - done));

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
        int n = sys3(SYS_WRITE, fd, (int)(chunk + done), (int)(len - done));

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

static void write_mode(u64 mib)
{
    u64 total = mib << 20, done = 0, three_quarters = (total >> 2) * 3;
    /* The 4 GiB line, where a 32-bit offset would wrap; a smaller file, for
       a measurement, is probed at its middle instead. */
    u64 line = total > ((u64)1 << 32) + CHUNK ? (u64)1 << 32 : total >> 1;
    int fd, fd2, fd3;
    unsigned t0, t1;
    long long got;

    fd = sys3(SYS_OPEN, (int)"/big/big.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
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
    if (sys1(SYS_FSYNC, fd) != 0) {
        fail("fsync");
    }
    t1 = now();
    put("bigfile: rate: ");
    put_u64(t1 > t0 ? (unsigned)mib / (t1 - t0) : (unsigned)mib);
    put(" MiB/s\n");
    got = size_of(fd);
    if (got != (long long)total) {
        fail_with("the size after the write", got);
    }
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
    fd2 = sys3(SYS_OPEN, (int)"/big/big.bin", O_RDONLY, 0);
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
    fd3 = sys1(SYS_DUP, fd);
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
    sys1(SYS_CLOSE, fd3);
    sys1(SYS_CLOSE, fd2);
    /* Three quarters, then a mebibyte appended at the end. */
    if (ftruncate64(fd, three_quarters) != 0) {
        fail("the truncate to three quarters");
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
    if (sys1(SYS_FSYNC, fd) != 0) {
        fail("fsync after the append");
    }
    sys1(SYS_CLOSE, fd);
}

static void verify_mode(u64 mib)
{
    u64 total = ((mib << 20) >> 2) * 3 + CHUNK, done = 0;
    int fd = sys3(SYS_OPEN, (int)"/big/big.bin", O_RDONLY, 0);
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
    got = sys3(SYS_READ, fd, (int)chunk, (int)BLOCK);
    if (got != 0) {
        fail_with("a read at the end", got);
    }
    sys1(SYS_CLOSE, fd);
}

static void sparse_mode(void)
{
    u64 line = (u64)1 << 32, at = line - CHUNK / 2;
    int fd = sys3(SYS_OPEN, (int)"/big/sparse.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    long long got;

    if (fd < 0) {
        fail_with("creating sparse.bin", fd);
    }
    if (seek(fd, (long long)at, SEEK_SET) != (long long)at) {
        fail("the seek to the 4 GiB line");
    }
    fill(at >> 12);
    write_full(fd, CHUNK, "the write astride the 4 GiB line");
    if (sys1(SYS_FSYNC, fd) != 0) {
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
    sys1(SYS_CLOSE, fd);
    sys1(SYS_UNLINK, (int)"/big/sparse.bin");
}

static void limit_mode(u64 offset)
{
    int fd = sys3(SYS_OPEN, (int)"/big/limit.bin", O_RDWR | O_CREAT | O_TRUNC, 0644);
    long long got;
    int rc;

    if (fd < 0) {
        fail_with("creating limit.bin", fd);
    }
    got = seek(fd, (long long)offset, SEEK_SET);
    if (got != (long long)offset) {
        fail_with("the seek to the offset", got);
    }
    rc = sys3(SYS_WRITE, fd, (int)chunk, 1);
    if (rc != -EFBIG) {
        fail_with("the write at the offset was not refused with EFBIG", rc);
    }
    got = size_of(fd);
    if (got != 0) {
        fail_with("the file grew", got);
    }
    sys1(SYS_CLOSE, fd);
    sys1(SYS_UNLINK, (int)"/big/limit.bin");
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
    sys2(SYS_MKDIR, (int)"/big", 0755);
    rc = sys5(SYS_MOUNT, (int)argv[2], (int)"/big", (int)argv[3], 0, 0);
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
    rc = sys2(SYS_UMOUNT2, (int)"/big", 0);
    if (rc != 0) {
        fail_with("unmount", rc);
    }
    put("BIGFILE OK (");
    put(mode);
    put(")\n");
    sys_exit(0);
}
