/**
 * Exercises trusted entropy and the fail-closed RNG policy.
 *
 * The probe adapts to the boot's entropy source, so it passes on both a CPU
 * with RDSEED/RDRAND and one without. It asks getrandom() for bytes. If that
 * succeeds the source is trusted, and the probe verifies that /dev/urandom
 * returns bytes that vary across reads and pass a loose monobit sanity (a
 * stuck/zero stream fails). If getrandom() returns -EIO the source is
 * untrusted, and the probe verifies the fail-closed policy is consistent:
 * /dev/urandom and /dev/random each open and answer a read with -EIO, while
 * getrandom(GRND_INSECURE) still returns bytes (the best-effort escape hatch).
 * The default suite boots `-cpu max` and takes the first branch; `tests/run
 * noentropy` boots a CPU with neither instruction and takes the second.
 *
 * Prints "RNGPROBE OK (trusted)" / "RNGPROBE OK (fail-closed)" /
 * "RNGPROBE FAIL". Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#include "../rach2/sys/arch/x86/kcpu.c"

#define O_RDONLY      0
#define GRND_INSECURE 0x0004u
#define EIO           5

static void sys_exit(int status)
{
    syscall3(SYS_EXIT, status, 0, 0);
    __builtin_unreachable();
}
static int sys_open(const char *p, int flags)
{
    return syscall3(SYS_OPEN, (long)p, flags, 0);
}
static int sys_close(int fd)
{
    return syscall3(SYS_CLOSE, fd, 0, 0);
}
static int sys_read(int fd, void *b, int n)
{
    return syscall3(SYS_READ, fd, (long)b, n);
}
static int getrandom(void *b, int n, unsigned f)
{
    return syscall3(SYS_GETRANDOM, (long)b, n, (int)f);
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

/**
 * True when `path` opens and answers a read with -EIO, as both random devices
 * must while the generator is untrusted. A device that cannot be opened is not
 * the refusal the policy promises, so it counts as a failure.
 */
static int refuses_read(const char *path)
{
    unsigned char buf[32];
    int fd, r;

    fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    r = sys_read(fd, buf, (int)sizeof(buf));
    sys_close(fd);
    return r == -EIO;
}

/** Compare the production helper with a direct leaf-7, subleaf-zero query. */
static int cpuid_subleaf_zero(void)
{
    uint32_t want_a = 7, want_b, want_c = 0, want_d;
    uint32_t got_a, got_b, got_c, got_d;

    kcpu_id(0, &got_a, 0, 0, 0);
    if (got_a < 7) {
        return 1;
    }
    __asm__ __volatile__("cpuid" : "+a"(want_a), "=b"(want_b), "+c"(want_c), "=d"(want_d));
    kcpu_id(0, 0, 0, 0, 0);
    kcpu_id(7, &got_a, &got_b, &got_c, &got_d);
    return got_a == want_a && got_b == want_b && got_c == want_c && got_d == want_d;
}

PROBE_START void _start(void)
{
    unsigned char a[32], b[32];
    int g, fd, bits;

    puts("RNGPROBE START\n");

    if (!cpuid_subleaf_zero()) {
        puts("RNGPROBE FAIL (cpuid subleaf zero)\n");
        sys_exit(1);
    }

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
         * stuck/zero stream is far off. Tolerate a wide band to avoid
         * flakiness. */
        bits = popcount_buf(a, (int)sizeof(a)) + popcount_buf(b, (int)sizeof(b));
        if (bits < 180 || bits > 332) {
            puts("RNGPROBE FAIL (monobit out of range)\n");
            sys_exit(1);
        }
        puts("RNGPROBE OK (trusted)\n");
        sys_exit(0);
    }

    if (g == -EIO) {
        /* Fail-closed: both devices must refuse as well, and GRND_INSECURE
         * must still deliver bytes (the best-effort escape hatch). */
        if (!refuses_read("/dev/urandom")) {
            puts("RNGPROBE FAIL (urandom not fail-closed)\n");
            sys_exit(1);
        }
        if (!refuses_read("/dev/random")) {
            puts("RNGPROBE FAIL (random not fail-closed)\n");
            sys_exit(1);
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
