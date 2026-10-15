/**
 * limitprobe: the resource limits, proved from user space.
 *
 * `prlimit64` keeps three limits in the process record: the processes of a
 * real user (`RLIMIT_NPROC`), a process's open descriptors (`RLIMIT_NOFILE`)
 * and its address space in bytes (`RLIMIT_AS`), each a soft value enforced
 * and a hard value the soft may be raised to. The probe runs as root and
 * proves, in children of its own: the limits read back as the table's; a
 * descriptor limit of eight refuses the ninth open with `EMFILE` and a
 * `dup2` onto a number past it with `EBADF`, a child forked under a limit
 * reads that limit, and an address-space limit of eight megabytes refuses
 * a larger mapping with `ENOMEM` and leaves the break where it was; a hard
 * limit raised by a user is refused with `EPERM` while a lowering and
 * root's raising are allowed; the limits are seen after a fork and after
 * an exec, which is `limitprobe show` checking the limit it inherited;
 * one user with four processes allowed is refused a fork past the four
 * with `EAGAIN` while another user forks; and a process that took that
 * user's identity with `setuid` counts against that user's four. Last, a
 * child under the limits OpenSSH's rlimit sandbox sets, none for the files
 * it may grow, the descriptors it may open and the processes it may fork,
 * still selects and polls a descriptor it opened before, as the test of
 * that sandbox in OpenSSH's configure requires. A child with a file limit
 * of 4096 bytes has a write of 8192 cut to 4096, and the next write and a
 * truncation past the limit refused with `EFBIG`.
 *
 * Needs: root privilege, readable /hello, a writable /tmp, this binary at
 * /limitprobe, and only descriptors 0, 1 and 2 open. Start with
 * RLIMIT_NOFILE at 1024/1024, unlimited RLIMIT_AS, and no other processes
 * owned by uid 1000 or 1001. Neither path nor limit depends on the root
 * file system's type.
 *
 * Prints "LIMITPROBE OK" and exits 0, or "LIMITPROBE FAIL: " and the reason
 * and exits 1.
 */

#include "probe.h"

#define RLIMIT_FSIZE  1
#define RLIMIT_NPROC  6
#define RLIMIT_NOFILE 7
#define RLIMIT_AS     9

#define EPERM  1
#define EAGAIN 11
#define ENOMEM 12
#define EINVAL 22
#define EMFILE 24
#define EBADF  9
#define EFBIG  27

#define O_RDONLY 0
#define O_WRONLY 01
#define O_CREAT  0100
#define O_TRUNC  01000

#define SIGXFSZ 25
#define SIG_IGN 1

struct rlimit64 {
    unsigned cur_lo, cur_hi, max_lo, max_hi;
};

struct pollfd {
    int fd;
    short events, revents;
};

#define POLLIN 1

/** An anonymous read-write mapping of `len` bytes; its address, or a negated
 * errno. */
static long mmap_anon(unsigned long len)
{
    return probe_mmap(0, (long)len, 3, 0x22, -1, 0);
}

/** End the process with `status`; it does not return. */
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

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

/** Print "LIMITPROBE FAIL: " and `why`, and exit 1. */
static void fail(const char *why)
{
    syscall3(SYS_WRITE, 1, (long)"LIMITPROBE FAIL: ", 17);
    puts(why);
    sys_exit(1);
}

/** Read the caller's limit `resource` into `rl`: 0, or a negated errno. */
static int get_limit(int resource, struct rlimit64 *rl)
{
    return syscall4(SYS_PRLIMIT64, 0, resource, 0, (long)rl);
}

/** Set the caller's limit `resource` to the soft value `cur` and the hard
 * value `max`: 0, or a negated errno. */
static int set_limit(int resource, unsigned cur, unsigned max)
{
    struct rlimit64 rl;

    rl.cur_lo = cur;
    rl.cur_hi = 0;
    rl.max_lo = max;
    rl.max_hi = 0;
    return syscall4(SYS_PRLIMIT64, 0, resource, (long)&rl, 0);
}

/** The exit status of the child `pid`, 128 and the signal for a child a
 * signal ended, or -1 when the wait fails. */
static int wait_status(int pid)
{
    int status = -1;

    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        return -1;
    }
    if ((status & 0x7f) != 0) {
        return 128 + (status & 0x7f); /* ended by a signal: never an answer */
    }
    return (status >> 8) & 0xff;
}

/** Sleep for `ms` milliseconds. */
static void sleep_ms(int ms)
{
    long ts[2];

    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000;
    syscall2(SYS_NANOSLEEP, (long)ts, 0);
}

/** Run `body` in a child that exits with its answer, and fail with `what`
 * unless the answer is `want`. */
static void child_must(int (*body)(void), int want, const char *what)
{
    int pid = syscall1(SYS_FORK, 0);

    if (pid < 0) {
        fail("fork of a child");
    }
    if (pid == 0) {
        sys_exit(body());
    }
    if (wait_status(pid) != want) {
        fail(what);
    }
}

/** Eight descriptors: the opens past the three of stdio stop at the ninth,
 * with EMFILE, a dup2 onto 9 is EBADF, and the limit is seen across an exec
 * of `limitprobe show`, whose status is then the child's. Returns 2 to 5 for
 * the step that fails. */
static int body_nofile(void)
{
    int i, fd = 0;
    char *argv[3];

    if (set_limit(RLIMIT_NOFILE, 8, 8) != 0) {
        return 2;
    }
    for (i = 0; i < 8; i++) {
        fd = syscall3(SYS_OPEN, (long)"/hello", O_RDONLY, 0);
        if (fd < 0) {
            break;
        }
    }
    if (i != 5 || fd != -EMFILE) {
        return 3;
    }
    if (syscall2(SYS_DUP2, 0, 9) != -EBADF) {
        return 5; /* a number past the limit */
    }
    argv[0] = (char *)"/limitprobe";
    argv[1] = (char *)"show";
    argv[2] = 0;
    syscall3(SYS_EXECVE, (long)argv[0], (long)argv, 0);
    return 4;
}

/** OpenSSH's rlimit sandbox: with limits of 0 on the files it may grow, the
 * descriptors it may open and the processes it may fork, the child selects
 * and polls a descriptor it opened before; each answers it ready. Returns
 * 0, or 2 to 4 for the step that fails. */
static int body_sandbox(void)
{
    unsigned rset[32];
    struct pollfd pfd;
    long tv[2];
    int fd, i;

    fd = syscall3(SYS_OPEN, (long)"/hello", O_RDONLY, 0);
    if (fd < 0 || set_limit(RLIMIT_FSIZE, 0, 0) != 0 || set_limit(RLIMIT_NOFILE, 0, 0) != 0 ||
        set_limit(RLIMIT_NPROC, 0, 0) != 0) {
        return 2;
    }
    for (i = 0; i < 32; i++) {
        rset[i] = 0;
    }
    rset[fd / 32] |= 1u << (fd % 32);
    tv[0] = 1;
    tv[1] = 0;
    if (syscall5(SYS_SELECT, fd + 1, (long)rset, 0, 0, (long)tv) != 1) {
        return 3;
    }
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (syscall3(SYS_POLL, (long)&pfd, 1, 1) != 1 || !(pfd.revents & POLLIN)) {
        return 4;
    }
    return 0;
}

/** A file of at most 4096 bytes: a write of 8192 writes 4096, the next one
 * and a truncation to 8192 are EFBIG, and SIGXFSZ, ignored here, would
 * otherwise have ended the child. Returns 0, or 2 to 4 for the step that
 * fails. */
static int body_fsize(void)
{
    static char block[8192];
    /* The kernel's sigaction: handler, flags, restorer and a 64-bit mask. */
    static const struct {
        unsigned long handler, flags, restorer;
        unsigned long long mask;
    } ignore = {SIG_IGN, 0, 0, 0};
    int fd, wrote, again, cut;

    if (set_limit(RLIMIT_FSIZE, 4096, 4096) != 0 ||
        syscall4(SYS_RT_SIGACTION, SIGXFSZ, (long)&ignore, 0, 8) != 0) {
        return 2;
    }
    fd = syscall3(SYS_OPEN, (long)"/tmp/limitprobe.f", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return 3;
    }
    wrote = syscall3(SYS_WRITE, fd, (long)block, (int)sizeof(block));
    again = syscall3(SYS_WRITE, fd, (long)block, 1);
    cut = syscall2(SYS_FTRUNCATE, fd, 8192);
    syscall1(SYS_CLOSE, fd);
    syscall1(SYS_UNLINK, (long)"/tmp/limitprobe.f");
    return wrote == 4096 && again == -EFBIG && cut == -EFBIG ? 0 : 4;
}

/** Eight megabytes of address space: a mapping of sixteen is refused, one of
 * one is not, and the break does not move past the limit. Returns 0, or 2
 * to 5 for the step that fails. */
static int body_as(void)
{
    long r;
    long brk0, brk1;

    if (set_limit(RLIMIT_AS, 8u << 20, 8u << 20) != 0) {
        return 2;
    }
    r = mmap_anon(16u << 20);
    if (r != -ENOMEM) {
        return 3;
    }
    r = mmap_anon(1u << 20);
    if (sys_failed(r)) {
        return 4;
    }
    brk0 = syscall1(SYS_BRK, 0);
    brk1 = syscall1(SYS_BRK, brk0 + (16 << 20));
    if (brk1 != brk0) {
        return 5;
    }
    return 0;
}

/** The hard limit: a user may lower it and not raise it; root raises it.
 * Returns 0, or 2 to 7 for the step that fails. */
static int body_hard(void)
{
    if (set_limit(RLIMIT_NOFILE, 512, 512) != 0) {
        return 2; /* root lowering */
    }
    if (set_limit(RLIMIT_NOFILE, 512, 1024) != 0) {
        return 3; /* root raising it back */
    }
    if (set_limit(RLIMIT_NOFILE, 512, 512) != 0 || syscall1(SYS_SETUID, 1000) != 0) {
        return 4;
    }
    if (set_limit(RLIMIT_NOFILE, 512, 1000) != -EPERM) {
        return 5; /* a user raising */
    }
    if (set_limit(RLIMIT_NOFILE, 256, 256) != 0) {
        return 6; /* a user lowering */
    }
    if (set_limit(RLIMIT_NOFILE, 300, 256) != -EINVAL) {
        return 7; /* soft above hard */
    }
    return 0;
}

/* The pipe on which `body_nproc` tells the parent that its four exist. */
static int report_fd = -1;

/** Uid 1000 with four processes allowed: three forks succeed and the fourth
 * is EAGAIN; the parent is told once the four exist, and the three children
 * exit after 1.5 seconds. Returns 0, or 2 to 4 for the step that fails. */
static int body_nproc(void)
{
    int i, pid;

    if (syscall1(SYS_SETUID, 1000) != 0 || set_limit(RLIMIT_NPROC, 4, 4) != 0) {
        return 2;
    }
    for (i = 0; i < 3; i++) {
        pid = syscall1(SYS_FORK, 0);
        if (pid < 0) {
            return 3;
        }
        if (pid == 0) {
            sleep_ms(1500);
            sys_exit(0);
        }
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid != -EAGAIN) {
        return 4;
    }
    syscall3(SYS_WRITE, report_fd, (long)"s", 1);
    for (i = 0; i < 3; i++) {
        syscall3(SYS_WAITPID, -1, 0, 0);
    }
    return 0;
}

/** Uid 1001 forks beside the limited uid 1000. Returns 0, or 2 to 4 for the
 * step that fails. */
static int body_others(void)
{
    int pid;

    if (syscall1(SYS_SETUID, 1001) != 0) {
        return 2;
    }
    pid = syscall1(SYS_FORK, 0);
    if (pid < 0) {
        return 3;
    }
    if (pid == 0) {
        sys_exit(0);
    }
    return wait_status(pid) == 0 ? 0 : 4;
}

/** Five allowed, with the family's four of uid 1000 standing: this process,
 * once it is uid 1000, is the fifth, and its fork is refused only if it is
 * counted; a kernel that left it out would see four and allow the fork.
 * Returns 0, 2 when the setup fails, and 3 when the fork is not EAGAIN. */
static int body_setuid(void)
{
    if (set_limit(RLIMIT_NPROC, 5, 5) != 0 || syscall1(SYS_SETUID, 1000) != 0) {
        return 2;
    }
    return syscall1(SYS_FORK, 0) == -EAGAIN ? 0 : 3;
}

/** The limits are the parent's at the fork: a child forked under a
 * descriptor limit of 200 reads 200. Returns 0, or 2. */
static int body_inherit(void)
{
    struct rlimit64 rl;

    return get_limit(RLIMIT_NOFILE, &rl) == 0 && rl.cur_lo == 200 && rl.max_lo == 200 ? 0 : 2;
}

/** `limitprobe show`: exit 0 when the descriptor limit is the 8 and 8
 * `body_nofile` set before its exec, and 9 otherwise. */
static void run_show(void)
{
    struct rlimit64 rl;

    if (get_limit(RLIMIT_NOFILE, &rl) != 0 || rl.cur_lo != 8 || rl.max_lo != 8) {
        sys_exit(9);
    }
    sys_exit(0);
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe, entered with `sp` at argc: `run_show` when the first argument
 * begins with 's', and otherwise the rows of the opening comment. */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    struct rlimit64 rl;
    static int fds[2];
    int family, others;
    char byte;

    if (argc >= 2 && argv[1][0] == 's') {
        run_show();
    }
    if (get_limit(RLIMIT_NOFILE, &rl) != 0 || rl.cur_lo != 1024 || rl.max_lo != 1024) {
        fail("the descriptor limit is not the table's");
    }
    if (get_limit(RLIMIT_AS, &rl) != 0 || rl.cur_lo != 0xFFFFFFFFu || rl.cur_hi != 0xFFFFFFFFu) {
        fail("the address-space limit is not unlimited");
    }
    child_must(body_nofile, 0,
               "eight descriptors: the ninth open is EMFILE and the exec sees eight");
    child_must(body_as, 0, "eight megabytes: a larger mapping is ENOMEM and the break stays");
    child_must(body_hard, 0, "the hard limit: a user lowers, root raises");
    child_must(body_sandbox, 0, "select and poll under the limits of 0 of OpenSSH's sandbox");
    child_must(body_fsize, 0, "a file limit of 4096: a write is cut to it and the next is EFBIG");
    if (set_limit(RLIMIT_NOFILE, 200, 200) != 0) {
        fail("setting the limit a child inherits");
    }
    child_must(body_inherit, 0, "a child forked under a limit of 200 reads 200");
    if (set_limit(RLIMIT_NOFILE, 200, 1024) != 0 || set_limit(RLIMIT_NOFILE, 1024, 1024) != 0) {
        fail("raising the limit back as root");
    }
    fds[0] = fds[1] = -1;
    if (syscall1(SYS_PIPE, (long)fds) != 0) {
        fail("the report pipe");
    }
    report_fd = fds[1];
    family = syscall1(SYS_FORK, 0);
    if (family < 0) {
        fail("fork of the family");
    }
    if (family == 0) {
        sys_exit(body_nproc());
    }
    /* A family that dies unheard is then end of input on the read below. */
    syscall1(SYS_CLOSE, fds[1]);
    if (syscall3(SYS_READ, fds[0], (long)&byte, 1) != 1 || byte != 's') {
        fail("four processes of one user: three forks and a fourth refused with EAGAIN");
    }
    child_must(body_others, 0, "another user forks beside the limited one");
    child_must(body_setuid, 0, "a process that became the limited user counts against it");
    others = wait_status(family);
    if (others != 0) {
        fail("four processes of one user: three forks and a fourth refused with EAGAIN");
    }
    puts("LIMITPROBE OK");
    sys_exit(0);
}
