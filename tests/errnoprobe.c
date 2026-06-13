/*
 * errnoprobe.c - Phase 3 probe: verify the kernel's -errno syscall contract.
 *
 * A freestanding program (no libc) that makes raw int 0x80 calls and checks the
 * returned value is the correct NEGATIVE Linux errno -- not a bare -1. An
 * unimplemented or out-of-range syscall number must return -ENOSYS (so musl's
 * feature-detection fallbacks work), and a deliberately failing real call must
 * return the specific errno Linux would (-ENOENT, -EBADF, -EFAULT). Prints
 * "All errno checks passed!" and exits 0 only if every check matches.
 *
 * Linux/BSD analogue: the kernel side of the errno contract -- the kernel
 * returns -errno, the C library negates it into errno.
 */

typedef unsigned int u32;

#define SYS_EXIT   1
#define SYS_READ   3
#define SYS_WRITE  4
#define SYS_OPEN   5
#define SYS_CLOSE  6
#define SYS_WRITEV 146
#define STDOUT     1

/* Linux i386 errno values we expect back (negated). */
#define ENOENT 2
#define EBADF  9
#define EFAULT 14
#define ENOSYS 38

static int syscall3(int num, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "a"(num), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

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
    syscall3(SYS_WRITE, STDOUT, (int)s, (int)slen(s));
}

static void put_int(int v)
{
    char buf[12];
    int i = 11;
    int neg = 0;
    u32 u;

    buf[i--] = '\0';
    if (v < 0) {
        neg = 1;
        u = (u32)(-v);
    } else {
        u = (u32)v;
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

/* Check that syscall 'got' equals the expected negative errno. */
static void expect(int got, int want_errno, const char *label)
{
    if (got != -want_errno) {
        puts_("FAIL: ");
        puts_(label);
        puts_(" expected -");
        put_int(want_errno);
        puts_(" got ");
        put_int(got);
        puts_("\n");
        fails++;
    }
}

void probe_main(u32 *sp);

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(u32 *sp)
{
    static const char missing[] = "/no_such_file_errnoprobe";
    char rbuf[16];

    (void)sp;

    puts_("errnoprobe: starting\n");

    /* Unimplemented in-range and out-of-range numbers must be -ENOSYS. */
    expect(syscall3(222, 0, 0, 0), ENOSYS, "unimpl(222)");
    expect(syscall3(909, 0, 0, 0), ENOSYS, "outofrange(909)");

    /* open() of a nonexistent path -> -ENOENT. */
    expect(syscall3(SYS_OPEN, (int)missing, 0, 0), ENOENT, "open(missing)");

    /* Operations on a bad fd -> -EBADF. */
    expect(syscall3(SYS_CLOSE, 999, 0, 0), EBADF, "close(999)");
    expect(syscall3(SYS_READ, 999, (int)rbuf, (int)sizeof(rbuf)), EBADF, "read(999)");

    /* writev() with an iov pointer outside user space -> -EFAULT. */
    expect(syscall3(SYS_WRITEV, STDOUT, (int)0xFFFFF000u, 1), EFAULT, "writev(bad iov)");

    if (fails == 0) {
        puts_("All errno checks passed!\n");
    } else {
        puts_("errnoprobe: FAILED\n");
    }
    syscall3(SYS_EXIT, fails == 0 ? 0 : 1, 0, 0);
}
