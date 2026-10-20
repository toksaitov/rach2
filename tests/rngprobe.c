/**
 * rngprobe: trusted entropy, and the random devices' refusal when there is
 * none.
 *
 * First the kernel's `kcpu_id`, compiled into the probe from kcpu.c, must
 * give leaf 7, subleaf 0, as a direct cpuid does; tests/proofs/rng-probe.json
 * puts a fault there. Then the probe follows the boot's entropy source, the
 * branch `source` of its report. It asks getrandom() for bytes. If that
 * succeeds the source is trusted, and /dev/urandom must return bytes that
 * vary across reads and pass a loose monobit test, which a stuck or zero
 * stream fails. If getrandom() returns -EIO the source is untrusted, the
 * branch fail-closed, and the devices must refuse: /dev/urandom and
 * /dev/random each open and answer a read with -EIO, while
 * getrandom(GRND_INSECURE) still returns its best-effort bytes. The probe
 * passes on either branch; etude judges the branch against the processor,
 * which is trusted where it has RDSEED or RDRAND, so that a kernel that
 * distrusts them fails. The default suite boots `-cpu max` and takes the
 * first branch; `tests/run noentropy` boots a processor with neither
 * instruction and requires the second.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, guest.default.rngprobe without it, and beside them "RNGPROBE START",
 * then "RNGPROBE OK (trusted)", "RNGPROBE OK (fail-closed)" or a "RNGPROBE
 * FAIL (why)" line for each check that failed. Freestanding; direct system
 * calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#include "../rach2/sys/arch/x86/kcpu.c"

#define O_RDONLY      0
#define GRND_INSECURE 0x0004u
#define EIO           5

/** exit(2); does not return. */
static void sys_exit(int status)
{
    syscall3(SYS_EXIT, status, 0, 0);
    __builtin_unreachable();
}
/** open(2) with no mode. */
static int sys_open(const char *p, int flags)
{
    return syscall3(SYS_OPEN, (long)p, flags, 0);
}
/** close(2). */
static int sys_close(int fd)
{
    return syscall3(SYS_CLOSE, fd, 0, 0);
}
/** read(2). */
static int sys_read(int fd, void *b, int n)
{
    return syscall3(SYS_READ, fd, (long)b, n);
}
/** getrandom(2). */
static int getrandom(void *b, int n, unsigned f)
{
    return syscall3(SYS_GETRANDOM, (long)b, n, (int)f);
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
/** Write the string `s`, which ends in a newline, to standard output. */
static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
}

/** The number of bits set in the `n` bytes at `b`. */
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

/** Whether the `n` bytes at `a` and `b` are the same. */
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

/** Append `a` and then `b` to the string `buf`, and return `buf`. */
static char *strcat2(char *buf, const char *a, const char *b)
{
    size_t n = strlen(buf);

    while (*a) {
        buf[n++] = *a++;
    }
    while (*b) {
        buf[n++] = *b++;
    }
    buf[n] = '\0';
    return buf;
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

/**
 * Whether the kernel's `kcpu_id`, asked for leaf 7, gives what a direct cpuid
 * of leaf 7, subleaf 0 gives. Returns 1 without the comparison on a processor
 * whose highest leaf is below 7.
 *
 * The cpuid of leaf 0 just before leaves part of the vendor string, which is
 * not zero, in ECX, so that a `kcpu_id` that does not load ECX with subleaf 0
 * asks for another subleaf.
 */
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

/**
 * The trusted branch, after a getrandom() that returned `g`: two reads of
 * /dev/urandom differ, and their bits pass the monobit count. Returns 1 when
 * both held.
 */
static int trusted(int g)
{
    unsigned char a[32], b[32];
    char count[12], why[64];
    int fd, bits, ok = 1;

    report_branch("source", "trusted");
    if (g != (int)sizeof(a)) {
        report_fail("trusted-urandom", "getrandom returned neither 32 bytes nor EIO");
        report_notrun("trusted-monobit", "no bytes were read");
        puts("RNGPROBE FAIL (getrandom unexpected)\n");
        return 0;
    }
    fd = sys_open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        report_fail("trusted-urandom", "/dev/urandom did not open");
        report_notrun("trusted-monobit", "no bytes were read");
        puts("RNGPROBE FAIL (urandom open, trusted)\n");
        return 0;
    }
    if (sys_read(fd, a, (int)sizeof(a)) != (int)sizeof(a) ||
        sys_read(fd, b, (int)sizeof(b)) != (int)sizeof(b)) {
        sys_close(fd);
        report_fail("trusted-urandom", "a read of 32 bytes of /dev/urandom fell short");
        report_notrun("trusted-monobit", "no bytes were read");
        puts("RNGPROBE FAIL (urandom read, trusted)\n");
        return 0;
    }
    sys_close(fd);
    if (bufs_equal(a, b, (int)sizeof(a))) {
        report_fail("trusted-urandom", "two reads of /dev/urandom gave the same 32 bytes");
        puts("RNGPROBE FAIL (urandom did not vary)\n");
        ok = 0;
    } else {
        report_pass("trusted-urandom");
    }
    /* 512 bits hold about 256 ones; a wide band, so a fair run passes. */
    bits = popcount_buf(a, (int)sizeof(a)) + popcount_buf(b, (int)sizeof(b));
    if (bits < 180 || bits > 332) {
        why[0] = '\0';
        report_fail("trusted-monobit",
                    strcat2(why, report_decimal(count, bits), " of 512 bits set"));
        puts("RNGPROBE FAIL (monobit out of range)\n");
        return 0;
    }
    report_pass("trusted-monobit");
    return ok;
}

/** Report the assertion `id` as `held`, with `detail` when it did not. */
static int check(const char *id, int held, const char *detail, const char *text)
{
    if (held) {
        report_pass(id);
    } else {
        report_fail(id, detail);
        puts(text);
    }
    return held;
}

/**
 * The fail-closed branch: both devices answer a read with -EIO, and
 * getrandom(GRND_INSECURE) still gives bytes. Returns 1 when all three held.
 */
static int closed(void)
{
    unsigned char a[32];
    int ok;

    report_branch("source", "fail-closed");
    ok = check("closed-urandom", refuses_read("/dev/urandom"),
               "/dev/urandom did not open and answer a read with EIO",
               "RNGPROBE FAIL (urandom not fail-closed)\n");
    ok &= check("closed-random", refuses_read("/dev/random"),
                "/dev/random did not open and answer a read with EIO",
                "RNGPROBE FAIL (random not fail-closed)\n");
    ok &= check("closed-insecure", getrandom(a, (int)sizeof(a), GRND_INSECURE) == (int)sizeof(a),
                "getrandom with GRND_INSECURE did not give 32 bytes",
                "RNGPROBE FAIL (GRND_INSECURE refused)\n");
    return ok;
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe, entered with `sp` at argc: the cpuid check, then the branch
 * getrandom() picks, each reported. */
void probe_main(unsigned long *sp)
{
    char **envp = (char **)(sp + 1) + sp[0] + 1;
    unsigned char a[32];
    int ok, g;

    report_begin(report_case(envp, "guest.default.rngprobe"));
    puts("RNGPROBE START\n");
    ok = check("cpuid-leaf7", cpuid_subleaf_zero(),
               "kcpu_id of leaf 7 differs from a cpuid of leaf 7, subleaf 0",
               "RNGPROBE FAIL (cpuid subleaf zero)\n");
    g = getrandom(a, (int)sizeof(a), 0);
    if (g == -EIO) {
        ok &= closed();
    } else {
        ok &= trusted(g);
    }
    report_end();
    if (!ok) {
        sys_exit(1);
    }
    puts(g == -EIO ? "RNGPROBE OK (fail-closed)\n" : "RNGPROBE OK (trusted)\n");
    sys_exit(0);
}
