/**
 * Resident-cap proof: an on-disk file system larger than physical RAM.
 *
 * On a KRFFS mounted from a real disk (rach2.fsdisk), writes 132 files of 256 KiB
 * each (33 MiB, past the kernel's 32 MiB physical-RAM cap), fsyncs them, then
 * re-reads every file checking a file- and offset-dependent pattern. The on-disk
 * data far exceeds both the 32 MiB RAM and the 1 MiB resident cache, so the whole
 * run is a continuous storm of eviction and writeback; that it round-trips
 * exactly is the proof that a file system bigger than memory works through the
 * cache. Prints "STRESSPROBE OK n=<files> bytes=<total>" and exits 0, or exits 1
 * on any mismatch. Each file stays well under the cache budget.
 *
 * Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT   1
#define SYS_READ   3
#define SYS_WRITE  4
#define SYS_OPEN   5
#define SYS_CLOSE  6
#define SYS_UNLINK 10
#define SYS_FSYNC  118

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x200

/* 132 x 256 KiB == 33 MiB, past the 32 MiB physical-RAM cap. */
#define NFILES    132u
#define FILE_SIZE (256u * 1024u)

static unsigned char buf[FILE_SIZE];

static int syscall1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

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
    syscall3(SYS_WRITE, 1, (int)(size_t)s, (int)strlen(s));
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
        syscall3(SYS_WRITE, 1, (int)(size_t)&c, 1);
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

static int write_all(int fd, const unsigned char *p, size_t n)
{
    size_t off = 0;
    while (off < n) {
        int w = syscall3(SYS_WRITE, fd, (int)(size_t)(p + off), (int)(n - off));
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
        int r = syscall3(SYS_READ, fd, (int)(size_t)(p + off), (int)(n - off));
        if (r <= 0) {
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}

void _start(void)
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
        fd = syscall3(SYS_OPEN, (int)(size_t)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || write_all(fd, buf, FILE_SIZE) != 0 || syscall1(SYS_FSYNC, fd) != 0) {
            out("STRESSPROBE FAIL: write\n");
            sys_exit(1);
        }
        syscall1(SYS_CLOSE, fd);
    }

    for (f = 0; f < NFILES; ++f) {
        path_of(path, f);
        for (i = 0; i < FILE_SIZE; ++i) {
            buf[i] = 0;
        }
        fd = syscall3(SYS_OPEN, (int)(size_t)path, O_RDONLY, 0);
        if (fd < 0 || read_all(fd, buf, FILE_SIZE) != 0) {
            out("STRESSPROBE FAIL: read\n");
            sys_exit(1);
        }
        syscall1(SYS_CLOSE, fd);
        for (i = 0; i < FILE_SIZE; ++i) {
            if (buf[i] != pattern(f, i)) {
                out("STRESSPROBE FAIL: data mismatch\n");
                sys_exit(1);
            }
        }
        syscall3(SYS_UNLINK, (int)(size_t)path, 0, 0);
    }

    out("STRESSPROBE OK n=");
    out_num(NFILES);
    out(" bytes=");
    out_num((unsigned long)NFILES * FILE_SIZE);
    out("\n");
    sys_exit(0);
}
