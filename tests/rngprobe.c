/*
 * rngprobe.c - Phase 20 probe: trusted entropy + the fail-closed policy.
 *
 * Self-adapting to the boot's entropy source (so it passes on BOTH a CPU with
 * RDSEED/RDRAND and one without):
 *   - it asks getrandom() for bytes. If that SUCCEEDS the source is trusted, and
 *     the probe verifies /dev/urandom returns bytes that VARY across reads and
 *     pass a loose monobit sanity (a stuck/zero stream fails);
 *   - if getrandom() returns -EIO the source is untrusted, and the probe verifies
 *     the FAIL-CLOSED policy is consistent: /dev/urandom also returns -EIO, while
 *     getrandom(GRND_INSECURE) still returns bytes (the best-effort escape hatch).
 *
 * Prints "RNGPROBE OK (trusted)" / "RNGPROBE OK (fail-closed)" / "RNGPROBE FAIL".
 * Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT      1
#define SYS_READ      3
#define SYS_WRITE     4
#define SYS_OPEN      5
#define SYS_CLOSE     6
#define SYS_GETRANDOM 355

#define O_RDONLY      0
#define GRND_INSECURE 0x0004u
#define EIO           5

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static void sys_exit(int status)
{
    syscall3(SYS_EXIT, status, 0, 0);
    __builtin_unreachable();
}
static int sys_open(const char *p, int flags)
{
    return syscall3(SYS_OPEN, (int)(size_t)p, flags, 0);
}
static int sys_close(int fd)
{
    return syscall3(SYS_CLOSE, fd, 0, 0);
}
static int sys_read(int fd, void *b, int n)
{
    return syscall3(SYS_READ, fd, (int)(size_t)b, n);
}
static int getrandom(void *b, int n, unsigned f)
{
    return syscall3(SYS_GETRANDOM, (int)(size_t)b, n, (int)f);
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
    syscall3(SYS_WRITE, 1, (int)(size_t)s, (int)strlen(s));
}

static int popcount_buf(const unsigned char *b, int n)
{
    int i, bits = 0;
    unsigned int x;
    for (i = 0; i < n; i++) {
        for (x = b[i]; x; x &= x - 1) {
            bits++;
        }
    }
    return bits;
}

static int bufs_equal(const unsigned char *a, const unsigned char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

void _start(void)
{
    unsigned char a[32], b[32];
    int g, fd, bits;

    puts("RNGPROBE START\n");

    g = getrandom(a, (int)sizeof(a), 0);

    if (g == (int)sizeof(a)) {
        /* Trusted: /dev/urandom must return bytes that vary and look random. */
        fd = sys_open("/dev/urandom", O_RDONLY);
        if (fd < 0) {
            puts("RNGPROBE FAIL (urandom open, trusted)\n");
            sys_exit(1);
        }
        if (sys_read(fd, a, (int)sizeof(a)) != (int)sizeof(a) ||
            sys_read(fd, b, (int)sizeof(b)) != (int)sizeof(b)) {
            puts("RNGPROBE FAIL (urandom read, trusted)\n");
            sys_exit(1);
        }
        sys_close(fd);
        if (bufs_equal(a, b, (int)sizeof(a))) {
            puts("RNGPROBE FAIL (urandom did not vary)\n");
            sys_exit(1);
        }
        /* Loose monobit over 512 bits (a + b): a uniform stream is ~256 ones; a
         * stuck/zero stream is far off. Tolerate a wide band to avoid flakiness. */
        bits = popcount_buf(a, (int)sizeof(a)) + popcount_buf(b, (int)sizeof(b));
        if (bits < 180 || bits > 332) {
            puts("RNGPROBE FAIL (monobit out of range)\n");
            sys_exit(1);
        }
        puts("RNGPROBE OK (trusted)\n");
        sys_exit(0);
    }

    if (g == -EIO) {
        /* Fail-closed: /dev/urandom must ALSO refuse, and GRND_INSECURE must still
         * deliver bytes (the best-effort escape hatch). */
        fd = sys_open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            int r = sys_read(fd, a, (int)sizeof(a));
            sys_close(fd);
            if (r != -EIO) {
                puts("RNGPROBE FAIL (urandom not fail-closed)\n");
                sys_exit(1);
            }
        }
        if (getrandom(a, (int)sizeof(a), GRND_INSECURE) != (int)sizeof(a)) {
            puts("RNGPROBE FAIL (GRND_INSECURE refused)\n");
            sys_exit(1);
        }
        puts("RNGPROBE OK (fail-closed)\n");
        sys_exit(0);
    }

    puts("RNGPROBE FAIL (getrandom unexpected)\n");
    sys_exit(1);
}
