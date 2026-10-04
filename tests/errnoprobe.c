/**
 * Verify the kernel's negative-errno syscall return contract.
 *
 * Raw system calls must return the correct negative Linux errno, not a bare
 * -1. An unimplemented or out-of-range syscall number returns -ENOSYS so musl's
 * feature-detection fallbacks work, and a deliberately failing real call
 * returns the specific errno Linux would (-ENOENT, -EBADF, -EFAULT). Prints
 * "All errno checks passed!" and exits 0 only if every check matches.
 *
 * Freestanding; direct system calls (probe.h), so the test sees the raw -errno
 * the kernel returns (a libc caller would negate it into a positive errno).
 */

#include "probe.h"

typedef unsigned int u32;

#define STDOUT 1

/* Linux errno values we expect back (negated), the same on i386 and x86-64. */
#define ENOENT 2
#define EBADF  9
#define EFAULT 14
#define ENOSYS 38

/* A number in the table's range that no call has: 222 on i386, and on x86-64,
 * where 222 is timer_create, 236, which Linux leaves unimplemented there. */
#if defined __x86_64__
#define NR_UNIMPL      236
#define NR_UNIMPL_TEXT "236"
#else
#define NR_UNIMPL      222
#define NR_UNIMPL_TEXT "222"
#endif

static u32 slen(const char *s)
{
    u32 n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static void puts_(const char *s)
{
    syscall3(SYS_WRITE, STDOUT, (long)s, (int)slen(s));
}

/** Write `v` in decimal: a system call's result is a long, whose width is
 * the architecture's. */
static void put_long(long v)
{
    char buf[24];
    int i = 23;
    int neg = 0;
    unsigned long u;

    buf[i--] = '\0';
    if (v < 0) {
        neg = 1;
        u = 0ul - (unsigned long)v;
    } else {
        u = (unsigned long)v;
    }
    if (u == 0) {
        buf[i--] = '0';
    }
    while (u > 0 && i >= 0) {
        buf[i--] = (char)('0' + (u % 10));
        u /= 10;
    }
    if (neg && i >= 0) {
        buf[i--] = '-';
    }
    puts_(&buf[i + 1]);
}

static int fails;

/**
 * Check that syscall result `got` equals the expected negative errno, at the
 * result's whole width: an error the kernel zero-extended on x86-64, such as
 * 0x00000000ffffffda for -ENOSYS, is a success to musl and must fail here.
 */
static void expect(long got, int want_errno, const char *label)
{
    if (got != -(long)want_errno) {
        puts_("FAIL: ");
        puts_(label);
        puts_(" expected -");
        put_long(want_errno);
        puts_(" got ");
        put_long(got);
        puts_("\n");
        fails++;
    }
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

void probe_main(unsigned long *sp)
{
    static const char missing[] = "/no_such_file_errnoprobe";
    char rbuf[16];

    (void)sp;

    puts_("errnoprobe: starting\n");

    /* Unimplemented in-range and out-of-range numbers must be -ENOSYS. */
    expect(syscall3(NR_UNIMPL, 0, 0, 0), ENOSYS, "unimpl(" NR_UNIMPL_TEXT ")");
    expect(syscall3(909, 0, 0, 0), ENOSYS, "outofrange(909)");

    /* open() of a nonexistent path -> -ENOENT. */
    expect(syscall3(SYS_OPEN, (long)missing, 0, 0), ENOENT, "open(missing)");

    /* Operations on a bad fd -> -EBADF. */
    expect(syscall3(SYS_CLOSE, 999, 0, 0), EBADF, "close(999)");
    expect(syscall3(SYS_READ, 999, (long)rbuf, (int)sizeof(rbuf)), EBADF, "read(999)");

    /* writev() with an iov pointer outside user space, in the last page of
     * the address space, which is the kernel's, -> -EFAULT. */
    expect(syscall3(SYS_WRITEV, STDOUT, -4096L, 1), EFAULT, "writev(bad iov)");

    if (fails == 0) {
        puts_("All errno checks passed!\n");
    } else {
        puts_("errnoprobe: FAILED\n");
    }
    syscall3(SYS_EXIT, fails == 0 ? 0 : 1, 0, 0);
}
