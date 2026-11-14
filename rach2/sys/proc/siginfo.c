/*
 * siginfo.c: the siginfo_t a program gives and takes, in the native layout,
 * the i386 one of the x86-64 kernel and signalfd's, each read and written by
 * the layout Linux 6.12's siginfo_layout gives its signal and code.
 *
 * A siginfo a program gives keeps its union whole, in the native layout
 * (`fields`), as Linux's kernel_siginfo keeps it, and so does the siginfo of
 * a ptrace stop that no signal makes; every other one the kernel makes has
 * only the members of `struct k_siginfo`, from which the union is built.
 * Each output reads the union by the layout, as Linux's
 * copy_siginfo_to_external32 and signalfd_copyinfo do, and the native one
 * copies it whole, as copy_siginfo_to_user does.
 */

#include "sigframe.h"

#include "../lib/kerrno.h"
#include "../lib/kstring.h"

/* The bytes of a pointer or a long in the native layout, and where the
 * union begins: after si_code on i386, after padding that aligns it on
 * x86-64. */
#define W        ((unsigned int)sizeof(uintptr_t))
#define UNION_AT (W == 8u ? 16u : 12u)

/* Within the union: a child's si_utime and si_stime, two clock_t. */
#define CHLD_UTIME (W == 8u ? 16u : 12u)
#define CHLD_STIME (CHLD_UTIME + W)

/* The last code of each signal whose codes have a layout of their own
 * (include/uapi/asm-generic/siginfo.h); SIGPOLL's last is any signal's last
 * of SIL_POLL. */
#define NSIGILL  11
#define NSIGFPE  15
#define NSIGSEGV 10
#define NSIGBUS  5
#define NSIGTRAP 6
#define NSIGCHLD 6
#define NSIGPOLL 6
#define NSIGSYS  2

/* The code of a SIGIO a descriptor's owner is sent, Linux's SI_SIGIO, and
 * the lowest negative code of the run from SI_USER down, SI_DETHREAD, with
 * the one below it whose layout Linux knows, SI_ASYNCNL. */
#define SI_SIGIO    (-5)
#define SI_DETHREAD (-7)
#define SI_ASYNCNL  (-60)

/** Store `v` at `p`, which need not be aligned. */
static void put16(uint8_t *p, uint16_t v)
{
    k_memcpy(p, &v, sizeof(v));
}

/** Store `v` at `p`, which need not be aligned. */
static void put32(uint8_t *p, uint32_t v)
{
    k_memcpy(p, &v, sizeof(v));
}

/** Store `v` at `p`, which need not be aligned. */
static void put64(uint8_t *p, uint64_t v)
{
    k_memcpy(p, &v, sizeof(v));
}

/** Store the native word `v` at `p`, which need not be aligned. */
static void putw(uint8_t *p, uintptr_t v)
{
    k_memcpy(p, &v, sizeof(v));
}

/** The value at `p`, which need not be aligned. */
static uint16_t get16(const uint8_t *p)
{
    uint16_t v;

    k_memcpy(&v, p, sizeof(v));
    return v;
}

/** The value at `p`, which need not be aligned. */
static uint32_t get32(const uint8_t *p)
{
    uint32_t v;

    k_memcpy(&v, p, sizeof(v));
    return v;
}

/** The native word at `p`, which need not be aligned. */
static uintptr_t getw(const uint8_t *p)
{
    uintptr_t v;

    k_memcpy(&v, p, sizeof(v));
    return v;
}

/** The native long `v` widened to 64 bits as Linux's `(long)` assignment to
 * a __u64 widens it: sign-extended on i386. */
static uint64_t long64(uintptr_t v)
{
    return W == 4u ? (uint64_t)(int64_t)(int32_t)v : (uint64_t)v;
}

int siginfo_layout(int signo, int32_t code)
{
    int32_t last = 0;
    int layout = SIL_FAULT;

    if (code > SI_USER && code < SI_KERNEL) {
        switch (signo) {
        case SIGILL:
            last = NSIGILL;
            break;
        case SIGFPE:
            last = NSIGFPE;
            break;
        case SIGSEGV:
            last = NSIGSEGV;
            if (code == SEGV_BNDERR) {
                layout = SIL_FAULT_BNDERR;
            } else if (code == SEGV_PKUERR) {
                layout = SIL_FAULT_PKUERR;
            }
            break;
        case SIGBUS:
            last = NSIGBUS;
            if (code == BUS_MCEERR_AR || code == BUS_MCEERR_AO) {
                layout = SIL_FAULT_MCEERR;
            }
            break;
        case SIGTRAP:
            last = NSIGTRAP;
            if (code == TRAP_PERF) {
                layout = SIL_FAULT_PERF_EVENT;
            }
            break;
        case SIGCHLD:
            last = NSIGCHLD;
            layout = SIL_CHLD;
            break;
        case SIGSYS:
            last = NSIGSYS;
            layout = SIL_SYS;
            break;
        default:
            break;
        }
        if (code <= last) {
            return layout;
        }
        return code <= NSIGPOLL ? SIL_POLL : SIL_KILL;
    }
    if (code == SI_TIMER) {
        return SIL_TIMER;
    }
    if (code == SI_SIGIO) {
        return SIL_POLL;
    }
    return code < 0 ? SIL_RT : SIL_KILL;
}

/**
 * Whether Linux 6.12 knows the layout of `signo`'s code `code`, as its
 * known_siginfo_layout does: SI_KERNEL; a positive code up to the last of
 * the signal's own codes, or up to SIGPOLL's last for a signal with none;
 * and the codes from SI_USER down to SI_DETHREAD, and SI_ASYNCNL. A
 * siginfo a program gives with any other code must have nothing past the
 * union the kernel keeps, since that union is all a later read gives back.
 */
int siginfo_known(int signo, int32_t code)
{
    int32_t last;

    if (code == SI_KERNEL) {
        return 1;
    }
    if (code <= SI_USER) {
        return code >= SI_DETHREAD || code == SI_ASYNCNL;
    }
    switch (signo) {
    case SIGILL:
        last = NSIGILL;
        break;
    case SIGFPE:
        last = NSIGFPE;
        break;
    case SIGSEGV:
        last = NSIGSEGV;
        break;
    case SIGBUS:
        last = NSIGBUS;
        break;
    case SIGTRAP:
        last = NSIGTRAP;
        break;
    case SIGCHLD:
        last = NSIGCHLD;
        break;
    case SIGSYS:
        last = NSIGSYS;
        break;
    default:
        last = NSIGPOLL;
        break;
    }
    return code <= last;
}

/** Whether `layout` is one of a fault's, whose union begins with si_addr. */
static int is_fault(int layout)
{
    return layout == SIL_FAULT || layout == SIL_FAULT_MCEERR || layout == SIL_FAULT_BNDERR ||
           layout == SIL_FAULT_PKUERR || layout == SIL_FAULT_PERF_EVENT;
}

/**
 * Write into the union `u`, zeroed, the members of `info` that `layout`
 * has, at their native offsets: the sender's pid and uid; a timer's id and
 * overrun in their place; si_value; a band, which on x86-64 is pid in its
 * low half and uid in its high, and a descriptor, kept in status; a fault's
 * address and a protection key; a child's status; and a seccomp SIGSYS's
 * call address, number, kept in uid, and architecture, kept in status.
 */
static void union_build(uint8_t *u, int layout, const struct k_siginfo *info)
{
    k_memset(u, 0, SIGINFO_UNION_SIZE);
    if (is_fault(layout)) {
        putw(u, info->addr);
        if (layout == SIL_FAULT_PKUERR) {
            put32(u + 2 * W, info->pkey);
        }
        return;
    }
    switch (layout) {
    case SIL_SYS:
        putw(u, info->addr);
        put32(u + W, info->uid);
        put32(u + W + 4, (uint32_t)info->status);
        break;
    case SIL_POLL:
        put32(u, info->pid);
        if (W == 8u) {
            put32(u + 4, info->uid);
        }
        put32(u + W, (uint32_t)info->status);
        break;
    case SIL_TIMER:
    case SIL_RT:
        put32(u, info->pid);
        put32(u + 4, info->uid);
        putw(u + 8, info->value);
        break;
    case SIL_CHLD:
        put32(u, info->pid);
        put32(u + 4, info->uid);
        put32(u + 8, (uint32_t)info->status);
        break;
    default:
        put32(u, info->pid);
        put32(u + 4, info->uid);
        break;
    }
}

/** Set the members of `info` that the rest of the kernel reads from the
 * union it keeps, read by `layout`, as `union_build` places them. */
static void union_decode(struct k_siginfo *info, int layout)
{
    const uint8_t *u = info->fields;

    if (is_fault(layout)) {
        info->addr = getw(u);
        if (layout == SIL_FAULT_PKUERR) {
            info->pkey = get32(u + 2 * W);
        }
        return;
    }
    switch (layout) {
    case SIL_SYS:
        info->addr = getw(u);
        info->pid = (uint32_t)info->addr;
        info->uid = get32(u + W);
        info->status = (int32_t)get32(u + W + 4);
        break;
    case SIL_POLL:
        info->pid = get32(u);
        info->uid = W == 8u ? get32(u + 4) : 0;
        info->status = (int32_t)get32(u + W);
        break;
    case SIL_TIMER:
    case SIL_RT:
        info->pid = get32(u);
        info->uid = get32(u + 4);
        info->value = getw(u + 8);
        info->status = (int32_t)get32(u + 8);
        break;
    case SIL_CHLD:
        info->pid = get32(u);
        info->uid = get32(u + 4);
        info->status = (int32_t)get32(u + 8);
        break;
    default:
        info->pid = get32(u);
        info->uid = get32(u + 4);
        break;
    }
}

void siginfo_pack(struct k_siginfo *info, int layout)
{
    union_build(info->fields, layout, info);
    info->packed = 1;
}

/** Fill `u` with the native union of `info` with the signal `signo`: the one
 * it keeps, or else one built by the layout of its signal and code. */
static void union_of(uint8_t *u, int signo, const struct k_siginfo *info)
{
    if (info->packed) {
        k_memcpy(u, info->fields, SIGINFO_UNION_SIZE);
    } else {
        union_build(u, siginfo_layout(signo, info->code), info);
    }
}

/**
 * The native siginfo: signo, errno and code, then the union, at 12 on i386
 * and at 16 on x86-64, with every byte that Linux's kernel_siginfo has and
 * the rest zero, as Linux's copy_siginfo_to_user writes it.
 */
void sigframe_fill_siginfo(uint8_t *out, int signo, const struct k_siginfo *info)
{
    k_memset(out, 0, SIGINFO_SIZE);
    put32(out, (uint32_t)signo);
    put32(out + 4, (uint32_t)info->err);
    put32(out + 8, (uint32_t)info->code);
    union_of(out + UNION_AT, signo, info);
}

/**
 * Keep the native siginfo at `in` whole, as Linux's copy_siginfo_from_user
 * keeps it, and read the members the rest of the kernel uses by the layout
 * of `signo` and its code. Returns 0, or -E2BIG, leaving `info` alone, for a
 * code whose layout Linux does not know (`siginfo_known`) with a byte past
 * the kept union that is not zero, as Linux's post_copy_siginfo_from_user
 * refuses it.
 */
int32_t sigframe_take_siginfo(const uint8_t *in, int signo, struct k_siginfo *info)
{
    int32_t code = (int32_t)get32(in + 8);
    unsigned int i;

    if (!siginfo_known(signo, code)) {
        for (i = SIGINFO_KEPT; i < SIGINFO_SIZE; i++) {
            if (in[i] != 0) {
                return -E2BIG;
            }
        }
    }
    k_memset(info, 0, sizeof(*info));
    info->err = (int32_t)get32(in + 4);
    info->code = (int32_t)get32(in + 8);
    k_memcpy(info->fields, in + UNION_AT, SIGINFO_UNION_SIZE);
    info->packed = 1;
    union_decode(info, siginfo_layout(signo, info->code));
    return 0;
}

/**
 * The signalfd_siginfo of `signo` and `info`, 128 bytes, as Linux's
 * signalfd_copyinfo fills it from the union by the layout: a sender's pid
 * and uid, with the value in ssi_ptr and its low word in ssi_int for a
 * queued signal; a timer's id, overrun and value; a band and a descriptor; a
 * fault's address, and an MCE's address bits; a child's pid, uid, status and
 * times; and a seccomp SIGSYS's call address, number and architecture.
 */
void signalfd_fill_siginfo(uint8_t *out, int signo, const struct k_siginfo *info)
{
    uint8_t u[SIGINFO_UNION_SIZE];
    int layout = siginfo_layout(signo, info->code);

    union_of(u, signo, info);
    k_memset(out, 0, SIGINFO_SIZE);
    put32(out, (uint32_t)signo);
    put32(out + 4, (uint32_t)info->err);
    put32(out + 8, (uint32_t)info->code);
    if (is_fault(layout)) {
        put64(out + 72, long64(getw(u))); /* ssi_addr */
        if (layout == SIL_FAULT_MCEERR) {
            put16(out + 80, get16(u + W)); /* ssi_addr_lsb */
        }
        return;
    }
    switch (layout) {
    case SIL_TIMER:
        put32(out + 24, get32(u));            /* ssi_tid */
        put32(out + 32, get32(u + 4));        /* ssi_overrun */
        put64(out + 48, long64(getw(u + 8))); /* ssi_ptr */
        put32(out + 44, get32(u + 8));        /* ssi_int */
        break;
    case SIL_POLL:
        put32(out + 28, get32(u));     /* ssi_band */
        put32(out + 20, get32(u + W)); /* ssi_fd */
        break;
    case SIL_CHLD:
        put32(out + 12, get32(u));
        put32(out + 16, get32(u + 4));
        put32(out + 40, get32(u + 8));                 /* ssi_status */
        put64(out + 56, long64(getw(u + CHLD_UTIME))); /* ssi_utime */
        put64(out + 64, long64(getw(u + CHLD_STIME))); /* ssi_stime */
        break;
    case SIL_RT:
        put32(out + 12, get32(u));
        put32(out + 16, get32(u + 4));
        put64(out + 48, long64(getw(u + 8))); /* ssi_ptr */
        put32(out + 44, get32(u + 8));        /* ssi_int */
        break;
    case SIL_SYS:
        put64(out + 88, long64(getw(u)));  /* ssi_call_addr */
        put32(out + 84, get32(u + W));     /* ssi_syscall */
        put32(out + 96, get32(u + W + 4)); /* ssi_arch */
        break;
    default:
        put32(out + 12, get32(u));
        put32(out + 16, get32(u + 4));
        break;
    }
}

#if defined(IA32_SYSCALL_TABLE_SIZE)
/**
 * The i386 siginfo of the x86-64 kernel: signo, errno and code, then at
 * byte 12 the fields of the union's layout, read from the native union, as
 * Linux's copy_siginfo_to_external32 writes them, and no others: pid and
 * uid at 12 and 16 (SIL_KILL); those and si_int at 20 (SIL_RT), or a
 * timer's id, overrun and si_int (SIL_TIMER); a child's pid, uid, status
 * and the low words of its times at 12 to 28 (SIL_CHLD); a band's low word
 * and a descriptor (SIL_POLL); a fault's address at 12 and, for the
 * layouts with more, an MCE's address bits at 16, a bound's lower and upper
 * at 20 and 24, a protection key at 20, or a perf event's data, type and
 * flags at 16, 20 and 24; and a seccomp SIGSYS's call address, number and
 * architecture (SIL_SYS). A pointer or long keeps its low 32 bits, and the
 * siginfo of a plain system call stop, whose code is a fault's, gives the
 * low half of its union's pid and uid as the address.
 */
void ia32_fill_siginfo(uint8_t *out, int signo, const struct k_siginfo *info)
{
    uint8_t u[SIGINFO_UNION_SIZE];
    int layout = siginfo_layout(signo, info->code);

    union_of(u, signo, info);
    k_memset(out, 0, SIGINFO_SIZE);
    put32(out, (uint32_t)signo);
    put32(out + 4, (uint32_t)info->err);
    put32(out + 8, (uint32_t)info->code);
    if (is_fault(layout)) {
        put32(out + 12, get32(u));
        switch (layout) {
        case SIL_FAULT_MCEERR:
            put16(out + 16, get16(u + W));
            break;
        case SIL_FAULT_BNDERR:
            put32(out + 20, get32(u + 2 * W));
            put32(out + 24, get32(u + 3 * W));
            break;
        case SIL_FAULT_PKUERR:
            put32(out + 20, get32(u + 2 * W));
            break;
        case SIL_FAULT_PERF_EVENT:
            put32(out + 16, get32(u + W));
            put32(out + 20, get32(u + 2 * W));
            put32(out + 24, get32(u + 2 * W + 4));
            break;
        default:
            break;
        }
        return;
    }
    switch (layout) {
    case SIL_SYS:
        put32(out + 12, get32(u));
        put32(out + 16, get32(u + W));
        put32(out + 20, get32(u + W + 4));
        break;
    case SIL_POLL:
        put32(out + 12, get32(u));
        put32(out + 16, get32(u + W));
        break;
    case SIL_TIMER:
    case SIL_RT:
        put32(out + 12, get32(u));
        put32(out + 16, get32(u + 4));
        put32(out + 20, get32(u + 8));
        break;
    case SIL_CHLD:
        put32(out + 12, get32(u));
        put32(out + 16, get32(u + 4));
        put32(out + 20, get32(u + 8));
        put32(out + 24, get32(u + CHLD_UTIME));
        put32(out + 28, get32(u + CHLD_STIME));
        break;
    default:
        put32(out + 12, get32(u));
        put32(out + 16, get32(u + 4));
        break;
    }
}

/**
 * Read the i386 siginfo at `in` as one of the signal `signo`, which
 * rt_sigqueueinfo and rt_tgsigqueueinfo give from their argument, into the
 * native union `info` keeps, as Linux's post_copy_siginfo_from_user32 does:
 * the fields of the layout and no others, a pointer, a value or an
 * unsigned long zero-extended, and a band and a child's times
 * sign-extended. The members the rest of the kernel uses are read back
 * from that union.
 */
void ia32_take_siginfo(const uint8_t *in, int signo, struct k_siginfo *info)
{
    uint8_t *u = info->fields;
    int32_t code = (int32_t)get32(in + 8);
    int layout = siginfo_layout(signo, code);

    k_memset(info, 0, sizeof(*info));
    info->err = (int32_t)get32(in + 4);
    info->code = code;
    if (is_fault(layout)) {
        putw(u, get32(in + 12));
        switch (layout) {
        case SIL_FAULT_MCEERR:
            put16(u + W, get16(in + 16));
            break;
        case SIL_FAULT_BNDERR:
            putw(u + 2 * W, get32(in + 20));
            putw(u + 3 * W, get32(in + 24));
            break;
        case SIL_FAULT_PKUERR:
            put32(u + 2 * W, get32(in + 20));
            break;
        case SIL_FAULT_PERF_EVENT:
            putw(u + W, get32(in + 16));
            put32(u + 2 * W, get32(in + 20));
            put32(u + 2 * W + 4, get32(in + 24));
            break;
        default:
            break;
        }
    } else {
        switch (layout) {
        case SIL_SYS:
            putw(u, get32(in + 12));
            put32(u + W, get32(in + 16));
            put32(u + W + 4, get32(in + 20));
            break;
        case SIL_POLL:
            putw(u, (uintptr_t)(int64_t)(int32_t)get32(in + 12));
            put32(u + W, get32(in + 16));
            break;
        case SIL_TIMER:
        case SIL_RT:
            put32(u, get32(in + 12));
            put32(u + 4, get32(in + 16));
            put32(u + 8, get32(in + 20));
            break;
        case SIL_CHLD:
            put32(u, get32(in + 12));
            put32(u + 4, get32(in + 16));
            put32(u + 8, get32(in + 20));
            putw(u + CHLD_UTIME, (uintptr_t)(int64_t)(int32_t)get32(in + 24));
            putw(u + CHLD_STIME, (uintptr_t)(int64_t)(int32_t)get32(in + 28));
            break;
        default:
            put32(u, get32(in + 12));
            put32(u + 4, get32(in + 16));
            break;
        }
    }
    info->packed = 1;
    union_decode(info, layout);
}
#endif
