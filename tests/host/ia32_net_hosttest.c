/**
 * Check the i386 socket structures of the x86-64 kernel's compat calls,
 * `arch/x86_64/ia32_abi.h` and `arch/x86_64/ia32_net.c`, against Linux
 * 6.12's layouts, and the conversions and socketcall of ia32_net.c over a
 * model of user memory, compiled with x86-64's `cpu.h`.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -DRACH2_HOST_X86_64
 *
 * The rows hold Linux's numbers for i386, read from include/linux/compat.h
 * (compat_msghdr, compat_mmsghdr, compat_cmsghdr and CMSG_COMPAT_ALIGN),
 * include/linux/time32.h (old_timeval32 and old_timespec32) and
 * include/uapi/linux/time_types.h (__kernel_sock_timeval and
 * __kernel_timespec), with compat_uptr_t and compat_size_t 32-bit words.
 */
#include "arch/x86_64/ia32_net.c"
#include "lib/kstring.h"

extern int printf(const char *format, ...);

#ifndef offsetof
#define offsetof(type, member) ((size_t)&((type *)0)->member)
#endif

static int failures;

/** Count a failure and print `row` when `ok` is 0. */
static void check(int ok, const char *row)
{
    if (!ok) {
        printf("ia32-net-hosttest: fail: %s\n", row);
        failures++;
    }
}

/* A structure's size or a member's offset, and Linux's. */
struct row {
    const char *what;
    unsigned long found;
    unsigned long linux;
};

#define SIZE(t, n)   {#t, sizeof(struct t), n}
#define OFF(t, f, n) {#t "." #f, offsetof(struct t, f), n}

static const struct row rows[] = {
    /* compat_msghdr */
    SIZE(ia32_msghdr, 28),
    OFF(ia32_msghdr, msg_namelen, 4),
    OFF(ia32_msghdr, msg_iov, 8),
    OFF(ia32_msghdr, msg_iovlen, 12),
    OFF(ia32_msghdr, msg_control, 16),
    OFF(ia32_msghdr, msg_controllen, 20),
    OFF(ia32_msghdr, msg_flags, 24),
    /* compat_mmsghdr */
    SIZE(ia32_mmsghdr, 32),
    OFF(ia32_mmsghdr, msg_len, 28),
    /* compat_cmsghdr; its data at CMSG_COMPAT_ALIGN(12) */
    SIZE(ia32_cmsghdr, 12),
    OFF(ia32_cmsghdr, cmsg_level, 4),
    OFF(ia32_cmsghdr, cmsg_type, 8),
    /* old_timeval32 of SO_RCVTIMEO_OLD, __kernel_sock_timeval of _NEW */
    SIZE(ia32_timeval, 8),
    OFF(ia32_timeval, tv_usec, 4),
    SIZE(ia32_sock_timeval, 16),
    OFF(ia32_sock_timeval, tv_usec_lo, 8),
    /* old_timespec32 and __kernel_timespec of recvmmsg's two calls */
    SIZE(ia32_timespec, 8),
    SIZE(ia32_timespec64, 16),
    OFF(ia32_timespec64, tv_nsec_lo, 8),
};

/* The constants the shared socket code reads the layouts by (arch.h), and
 * Linux's: the sizes, the count's offset, the data's offset and the
 * alignment, whose SCM_RIGHTS of one descriptor is CMSG_COMPAT_LEN(4) = 16
 * and of three CMSG_COMPAT_SPACE(12) = 24, and whose SCM_CREDENTIALS is 24. */
static const struct row consts[] = {
    {"IA32_MSGHDR_LEN", IA32_MSGHDR_LEN, 28},
    {"IA32_MMSGHDR_LEN", IA32_MMSGHDR_LEN, 32},
    {"IA32_MMSG_LEN_OFF", IA32_MMSG_LEN_OFF, 28},
    {"IA32_CMSG_HDR", IA32_CMSG_HDR, 12},
    {"sizeof(ia32_msghdr)", sizeof(struct ia32_msghdr), IA32_MSGHDR_LEN},
    {"sizeof(ia32_mmsghdr)", sizeof(struct ia32_mmsghdr), IA32_MMSGHDR_LEN},
    {"sizeof(ia32_cmsghdr)", sizeof(struct ia32_cmsghdr), IA32_CMSG_HDR},
    {"rights of 1", IA32_CMSG_HDR + 4u, 16},
    {"space of 1 fd", IA32_CMSG_ALIGN(IA32_CMSG_HDR + 4u), 16},
    {"space of 3 fds", IA32_CMSG_ALIGN(IA32_CMSG_HDR + 12u), 24},
    {"space of 1 byte", IA32_CMSG_ALIGN(IA32_CMSG_HDR + 1u), 16},
    {"space of a ucred", IA32_CMSG_ALIGN(IA32_CMSG_HDR + 12u), 24},
    /* the kernel's own, for the 64-bit caller of the same code */
    {"sizeof(kernel_msghdr)", sizeof(struct kernel_msghdr), 56},
    {"CMSG_HDR", CMSG_HDR, 16},
};

/* The model's user memory: USER_LEN bytes from USER_BASE, below 4 GiB. */
#define USER_BASE 0x40000000ul
#define USER_LEN  0x1000ul
static uint8_t user[USER_LEN];

/** The model's bytes at user address `a` for `n` bytes, or NULL. */
static uint8_t *at(uintptr_t a, size_t n)
{
    if (a < USER_BASE || a + n > USER_BASE + USER_LEN || a + n < a) {
        return NULL;
    }
    return user + (a - USER_BASE);
}

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    uint8_t *p = at((uintptr_t)src, n);

    if (!p) {
        return -1;
    }
    k_memcpy(dst, p, n);
    return 0;
}

int copy_to_user(void __user *dst, const void *src, size_t n)
{
    uint8_t *p = at((uintptr_t)dst, n);

    if (!p) {
        return -1;
    }
    k_memcpy(p, src, n);
    return 0;
}

/* The model of the shared dispatcher: the sub-call and arguments it got. */
static int got_call;
static unsigned long got_args[6];

/** socketcall's argument counts (syscall_net.c), for the sub-calls the
 * rows make: 3 for socket, 6 for recvfrom, 5 for recvmmsg, none for 0. */
int socketcall_nargs(int call)
{
    return call == 1 ? 3 : call == 12 ? 6 : call == 19 ? 5 : 0;
}

/** Note the sub-call and its arguments; returns 7. */
int32_t socket_call(int call, unsigned long *args)
{
    int i;

    got_call = call;
    for (i = 0; i < 6; i++) {
        got_args[i] = args[i];
    }
    return 7;
}

/** Store the 32-bit word `v` at user address `a`, little-endian. */
static void put32(uintptr_t a, uint32_t v)
{
    k_memcpy(at(a, 4), &v, 4);
}

/** The 32-bit word at user address `a`. */
static uint32_t get32(uintptr_t a)
{
    uint32_t v;

    k_memcpy(&v, at(a, 4), 4);
    return v;
}

/** The msghdr conversions: read with 32-bit pointers and lengths
 * zero-extended and the name's length signed; a receive's results written
 * at Linux's offsets, the name's length only with a name, nothing else. */
static void msghdr_rows(void)
{
    uintptr_t u = USER_BASE + 0x100;
    struct kernel_msghdr h;
    int i;

    put32(u + 0, 0xFFFFFFF0u);
    put32(u + 4, (uint32_t)-1);
    put32(u + 8, 0x80000000u);
    put32(u + 12, 3);
    put32(u + 16, 0x40000200u);
    put32(u + 20, 0xFFFFFFFFu);
    put32(u + 24, 0x40u);
    k_memset(&h, 0xA5, sizeof(h));
    check(ia32_msghdr_get(&h, u) == 0, "msghdr read");
    check(h.msg_name == 0xFFFFFFF0ul && h.msg_iov == 0x80000000ul && h.msg_iovlen == 3 &&
              h.msg_control == 0x40000200ul && h.msg_controllen == 0xFFFFFFFFul,
          "msghdr pointers and lengths zero-extended");
    check(h.msg_namelen == -1 && h.msg_flags == 0x40u, "msghdr name length and flags");
    check(ia32_msghdr_get(&h, USER_BASE + USER_LEN - 27) == -EFAULT, "msghdr past memory");

    for (i = 0; i < 28; i += 4) {
        put32(u + (uintptr_t)i, 0xEEEEEEEEu);
    }
    k_memset(&h, 0, sizeof(h));
    h.msg_name = 0;
    h.msg_namelen = 16;
    h.msg_controllen = 24;
    h.msg_flags = 0x8u;
    check(ia32_msghdr_put(u, &h) == 0, "msghdr write");
    check(get32(u + 4) == 0xEEEEEEEEu, "msghdr name length left without a name");
    check(get32(u + 20) == 24 && get32(u + 24) == 0x8u, "msghdr control length and flags");
    check(get32(u + 0) == 0xEEEEEEEEu && get32(u + 8) == 0xEEEEEEEEu &&
              get32(u + 12) == 0xEEEEEEEEu && get32(u + 16) == 0xEEEEEEEEu,
          "msghdr pointers left as they were");
    h.msg_name = 0x40000300u;
    check(ia32_msghdr_put(u, &h) == 0 && get32(u + 4) == 16, "msghdr name length with a name");
}

/** The control message header: 12 bytes, read and written whole. */
static void cmsg_rows(void)
{
    uintptr_t u = USER_BASE + 0x200;
    struct kernel_cmsghdr c;

    k_memset(at(u, 32), 0xEE, 32);
    c.cmsg_len = IA32_CMSG_HDR + 4u;
    c.cmsg_level = 1;
    c.cmsg_type = 1;
    check(ia32_cmsg_put(u, &c) == 0, "cmsghdr write");
    check(get32(u) == 16 && get32(u + 4) == 1 && get32(u + 8) == 1,
          "cmsghdr SCM_RIGHTS of one descriptor");
    check(get32(u + 12) == 0xEEEEEEEEu, "cmsghdr writes 12 bytes, its data at 12");
    put32(u, 24);
    put32(u + 8, 2);
    k_memset(&c, 0xA5, sizeof(c));
    check(ia32_cmsg_get(&c, u) == 0 && c.cmsg_len == 24 && c.cmsg_level == 1 && c.cmsg_type == 2,
          "cmsghdr read, SCM_CREDENTIALS");
    check(ia32_cmsg_get(&c, USER_BASE + USER_LEN - 8) == -EFAULT, "cmsghdr past memory");
}

/** socketcall: the sub-call's words read, each zero-extended, the rest 0;
 * EINVAL for a sub-call with no count; EFAULT for words past memory. */
static void socketcall_rows(void)
{
    isr_params_t p;
    uintptr_t a = USER_BASE + 0x300;
    int i;

    for (i = 0; i < 6; i++) {
        put32(a + 4u * (uintptr_t)i, 0xFFFFFFF0u + (uint32_t)i);
    }
    k_memset(&p, 0, sizeof(p));
    p.vector = 0x80u;
    p.error_code = SC_ENTRY_IA32;
    p.rbx = 0xFFFFFFFF0000000Cull; /* recvfrom; the high half is not the call's */
    p.rcx = a;
    check(compat_sys_socketcall(&p) == 7 && got_call == 12, "socketcall recvfrom runs");
    check(got_args[0] == 0xFFFFFFF0ul && got_args[5] == 0xFFFFFFF5ul,
          "socketcall words zero-extended");
    p.rbx = 1;
    check(compat_sys_socketcall(&p) == 7 && got_call == 1 && got_args[2] == 0xFFFFFFF2ul &&
              got_args[3] == 0 && got_args[5] == 0,
          "socketcall reads only the sub-call's words");
    p.rbx = 0;
    check(compat_sys_socketcall(&p) == -EINVAL, "socketcall of no sub-call");
    p.rbx = 19;
    p.rcx = USER_BASE + USER_LEN - 16;
    check(compat_sys_socketcall(&p) == -EFAULT, "socketcall words past memory");
}

int main(void)
{
    size_t i;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (rows[i].found != rows[i].linux) {
            printf("ia32-net-hosttest: fail: %s: %lu, Linux %lu\n", rows[i].what, rows[i].found,
                   rows[i].linux);
            failures++;
        }
    }
    for (i = 0; i < sizeof(consts) / sizeof(consts[0]); i++) {
        if (consts[i].found != consts[i].linux) {
            printf("ia32-net-hosttest: fail: %s: %lu, want %lu\n", consts[i].what, consts[i].found,
                   consts[i].linux);
            failures++;
        }
    }
    msghdr_rows();
    cmsg_rows();
    socketcall_rows();
    if (failures) {
        printf("ia32-net-hosttest: result: %d faults\n", failures);
        return 1;
    }
    printf("ia32-net-hosttest: result: %lu layout rows, the conversions and socketcall match "
           "Linux 6.12\n",
           (unsigned long)(sizeof(rows) / sizeof(rows[0]) + sizeof(consts) / sizeof(consts[0])));
    return 0;
}
