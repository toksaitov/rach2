/**
 * limitprobe: the resource limits, proved from user space.
 *
 * `prlimit64` keeps three limits in the process record: the processes of a
 * real user (`RLIMIT_NPROC`), a process's open descriptors (`RLIMIT_NOFILE`)
 * and its address space in bytes (`RLIMIT_AS`), each a soft value enforced
 * and a hard value the soft may be raised to. The probe runs as root and
 * proves, in children of its own: the limits read back as the table's, a
 * soft 1024 and a hard 524288 for descriptors; a descriptor limit of eight
 * refuses the ninth open with `EMFILE` and a `dup2` onto a number past it
 * with `EBADF`; root raises the hard descriptor limit to fs.nr_open,
 * 1048576, and no further, and under a soft 70000 holds, selects and polls
 * descriptor 65536; a table that a process cloned with `CLONE_FILES` shares
 * grows while that process sleeps in a read with a descriptor of it, and
 * the two see each other's descriptors after; a child forked under a limit
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
 * truncation past the limit refused with `EFBIG`. A zombie not yet reaped
 * keeps its limits, which root reads and sets and another user is refused
 * with `EPERM`; once reaped, its pid is `ESRCH`.
 *
 * Needs: root privilege, readable /hello, a writable /tmp, this binary at
 * /limitprobe, and only descriptors 0, 1 and 2 open. Start with
 * RLIMIT_NOFILE at 1024/524288, unlimited RLIMIT_AS, and no other processes
 * owned by uid 1000 or 1001. Neither path nor limit depends on the root
 * file system's type.
 *
 * A native run gives these as arguments (probe_args.h): `hello=PATH` is the
 * readable file in place of /hello, `scratch=PATH` the file the size limit
 * writes in place of /tmp/limitprobe.f, `ids=OLD:NEW,...` the users in place
 * of 1000 and 1001, and the probe executes itself by the absolute path it was
 * started by in place of /limitprobe. Without them it uses the paths and ids
 * above.
 *
 * Prints "LIMITPROBE OK" and exits 0, or "LIMITPROBE FAIL: " and the reason
 * and exits 1. Writes etude's report records (probe_report.h) for the case
 * ETUDE_CASE names, an assertion for each check `checks` lists; a failure
 * leaves the checks after it not run. `limitprobe show` writes none.
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

#define RLIMIT_FSIZE  1
#define RLIMIT_NPROC  6
#define RLIMIT_NOFILE 7
#define RLIMIT_AS     9

#define EPERM  1
#define ESRCH  3
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

#define P_PID   1
#define WEXITED 4
#define WNOWAIT 0x01000000

#define SIGCHLD 17
#define SIGXFSZ 25
#define SIG_IGN 1

/* The readable file, the size limit's scratch file, this program's path and
 * the two users, or the values the arguments give in their place. */
static const char *hello_path, *scratch_path, *self_path;
static long id_1000, id_1001;

struct rlimit64 {
    unsigned cur_lo, cur_hi, max_lo, max_hi;
};

struct pollfd {
    int fd;
    short events, revents;
};

#define POLLIN  1
#define POLLOUT 4

#define F_DUPFD     0
#define F_GETFD     1
#define CLONE_FILES 0x400

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

/* The checks, one assertion each, in the order the program makes them, and
 * the text it prints for each that fails. */
enum {
    NOFILE_TABLE,
    AS_UNLIMITED,
    NOFILE_EIGHT,
    NOFILE_HIGH,
    NOFILE_SHARED,
    AS_EIGHT,
    HARD_LIMIT,
    SANDBOX,
    FSIZE,
    ZOMBIE,
    INHERIT_SET,
    INHERIT,
    RAISE_BACK,
    PIPES,
    FAMILY_FORK,
    NPROC_FOUR,
    OTHERS,
    SETUID_COUNTS,
    FAMILY_EXIT,
    CHECKS
};

static const struct {
    const char *id;
    const char *text;
} checks[CHECKS] = {
    {"nofile-table", "the descriptor limit is not the table's"},
    {"as-unlimited", "the address-space limit is not unlimited"},
    {"nofile-eight", "eight descriptors: the ninth open is EMFILE and the exec sees eight"},
    {"nofile-high", "descriptor 65536 under a hard limit of 1048576"},
    {"nofile-shared", "a shared table grows while a sharer sleeps holding a descriptor"},
    {"as-eight", "eight megabytes: a larger mapping is ENOMEM and the break stays"},
    {"hard-limit", "the hard limit: a user lowers, root raises"},
    {"sandbox", "select and poll under the limits of 0 of OpenSSH's sandbox"},
    {"fsize", "a file limit of 4096: a write is cut to it and the next is EFBIG"},
    {"zombie", "a zombie's limits: root reads and sets them, a user is EPERM"},
    {"inherit-set", "setting the limit a child inherits"},
    {"inherit", "a child forked under a limit of 200 reads 200"},
    {"raise-back", "raising the limit back as root"},
    {"pipes", "the report pipes"},
    {"family-fork", "fork of the family"},
    {"nproc-four", "four processes of one user: three forks and a fourth refused with EAGAIN"},
    {"others", "another user forks beside the limited one"},
    {"setuid-counts", "a process that became the limited user counts against it"},
    {"family-exit", "four processes of one user: three forks and a fourth refused with EAGAIN"},
};

/** In the process that began the report, report every check before `check`
 * as held, `check` as failed with `why`, those after it as not run, and the
 * report's end; with `check` CHECKS, every check held. */
static void report_at(int check, const char *why)
{
    int i;

    if (report_ended || report_getpid() != report_owner) {
        return;
    }
    for (i = 0; i < CHECKS; i++) {
        if (i < check) {
            report_pass(checks[i].id);
        } else if (i == check) {
            report_fail(checks[i].id, why);
        } else {
            report_notrun(checks[i].id, "an earlier check failed");
        }
    }
    report_end();
}

/** Print "LIMITPROBE FAIL: " and `why`, report that the check `check` failed
 * with it, and exit 1. */
static void fail_as(int check, const char *why)
{
    syscall3(SYS_WRITE, 1, (long)"LIMITPROBE FAIL: ", 17);
    puts(why);
    report_at(check, why);
    sys_exit(1);
}

/** Fail the check `check` with its text, as `fail_as` does. */
static void fail(int check)
{
    fail_as(check, checks[check].text);
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

/** Run `body` in a child that exits with its answer, and fail the check
 * `check` unless the answer is `want`. */
static void child_must(int (*body)(void), int want, int check)
{
    int pid = syscall1(SYS_FORK, 0);

    if (pid < 0) {
        fail_as(check, "fork of a child");
    }
    if (pid == 0) {
        sys_exit(body());
    }
    if (wait_status(pid) != want) {
        fail(check);
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
        fd = syscall3(SYS_OPEN, (long)hello_path, O_RDONLY, 0);
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
    argv[0] = (char *)self_path;
    argv[1] = (char *)"show";
    argv[2] = 0;
    syscall3(SYS_EXECVE, (long)argv[0], (long)argv, 0);
    return 4;
}

/* A select set wide enough for descriptor 65536. */
static unsigned wide_set[65568 / 32];

/** High descriptors: a hard descriptor limit past fs.nr_open is EPERM even
 * to root, and nr_open itself is allowed; under a soft limit of 70000 a
 * dup2 onto 65536 grows the table to it, an open still gets the lowest free
 * descriptor, F_DUPFD from 69999 gets 69999 and then EMFILE, a dup2 onto
 * 70000 is EBADF, and select, with a set wider than FD_SETSIZE, and poll
 * answer the write end of a pipe at 65536 ready. Returns 0, or 2 to 9 for
 * the step that fails. */
static int body_high(void)
{
    struct rlimit64 rl;
    struct pollfd pfd;
    int pipefd[2];
    long tv[2];

    if (set_limit(RLIMIT_NOFILE, 1024, 1048577) != -EPERM) {
        return 2;
    }
    if (set_limit(RLIMIT_NOFILE, 70000, 1048576) != 0 || get_limit(RLIMIT_NOFILE, &rl) != 0 ||
        rl.cur_lo != 70000 || rl.max_lo != 1048576) {
        return 3;
    }
    if (syscall1(SYS_PIPE, (long)pipefd) != 0 || syscall2(SYS_DUP2, pipefd[1], 65536) != 65536) {
        return 4;
    }
    if (syscall3(SYS_OPEN, (long)hello_path, O_RDONLY, 0) != pipefd[1] + 1) {
        return 5; /* the lowest free descriptor, not one past 65536 */
    }
    if (syscall3(SYS_FCNTL, 0, F_DUPFD, 69999) != 69999 ||
        syscall3(SYS_FCNTL, 0, F_DUPFD, 69999) != -EMFILE) {
        return 6;
    }
    if (syscall2(SYS_DUP2, 0, 70000) != -EBADF) {
        return 7;
    }
    wide_set[65536 / 32] = 1u << (65536 % 32);
    tv[0] = 1;
    tv[1] = 0;
    if (syscall5(SYS_SELECT, 65537, 0, (long)wide_set, 0, (long)tv) != 1 ||
        wide_set[65536 / 32] != 1u << (65536 % 32)) {
        return 8;
    }
    pfd.fd = 65536;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    if (syscall3(SYS_POLL, (long)&pfd, 1, 1000) != 1 || !(pfd.revents & POLLOUT)) {
        return 9;
    }
    return 0;
}

/** A table shared while it grows: a process cloned with CLONE_FILES sleeps
 * in a read of a pipe, which holds the pipe's descriptor for the read, while
 * this one makes 3000 descriptors, which moves the table to larger arrays
 * eight times; the read then ends with the byte written, the clone finds
 * descriptor 3006, the last made, and closes it, and this one finds it
 * closed. Returns 0, or 2 to 8 for the step that fails. */
static int body_shared(void)
{
    static int go[2], back[2];
    long ts[2];
    char byte;
    int i, pid;

    if (set_limit(RLIMIT_NOFILE, 4096, 4096) != 0 || syscall1(SYS_PIPE, (long)go) != 0 ||
        syscall1(SYS_PIPE, (long)back) != 0 || back[1] != 6) {
        return 2;
    }
    pid = syscall5(SYS_CLONE, CLONE_FILES | SIGCHLD, 0, 0, 0, 0);
    if (pid < 0) {
        return 3;
    }
    if (pid == 0) {
        if (syscall3(SYS_WRITE, back[1], (long)"r", 1) != 1 ||
            syscall3(SYS_READ, go[0], (long)&byte, 1) != 1 || byte != 'g') {
            sys_exit(2);
        }
        if (syscall3(SYS_FCNTL, 3006, F_GETFD, 0) != 0 || syscall1(SYS_CLOSE, 3006) != 0) {
            sys_exit(3);
        }
        sys_exit(syscall3(SYS_WRITE, back[1], (long)"c", 1) == 1 ? 0 : 4);
    }
    if (syscall3(SYS_READ, back[0], (long)&byte, 1) != 1 || byte != 'r') {
        return 4;
    }
    ts[0] = 0;
    ts[1] = 100000000;
    /* interval: the clone is given 100 ms to fall asleep in its read */
    syscall2(SYS_NANOSLEEP, (long)ts, 0);
    for (i = 0; i < 3000; i++) {
        if (syscall1(SYS_DUP, 0) != 7 + i) {
            return 5;
        }
    }
    if (syscall3(SYS_WRITE, go[1], (long)"g", 1) != 1 ||
        syscall3(SYS_READ, back[0], (long)&byte, 1) != 1 || byte != 'c') {
        return 6;
    }
    if (syscall3(SYS_FCNTL, 3006, F_GETFD, 0) != -EBADF) {
        return 7;
    }
    return wait_status(pid) == 0 ? 0 : 8;
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

    fd = syscall3(SYS_OPEN, (long)hello_path, O_RDONLY, 0);
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

/** A file of at most 4096 bytes. A file of 16384 bytes, written before the
 * limit is set, shrinks to 12288 by ftruncate and to 8192 by truncate while
 * SIGXFSZ, which would end the child, still has its default action, since
 * only growth is checked against the limit. Then, with SIGXFSZ ignored, a
 * write of 8192 to an emptied file writes 4096, and the next one and a
 * truncation to 8192 are EFBIG. Returns 0, or 2 to 6 for the step that
 * fails. */
static int body_fsize(void)
{
    static char block[8192];
    /* The kernel's sigaction: handler, flags, restorer and a 64-bit mask. */
    static const struct {
        unsigned long handler, flags, restorer;
        unsigned long long mask;
    } ignore = {SIG_IGN, 0, 0, 0};
    int fd, wrote, again, cut, shrunk;

    fd = syscall3(SYS_OPEN, (long)scratch_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return 3;
    }
    wrote = syscall3(SYS_WRITE, fd, (long)block, (int)sizeof(block));
    again = syscall3(SYS_WRITE, fd, (long)block, (int)sizeof(block));
    if (wrote != (int)sizeof(block) || again != (int)sizeof(block) ||
        set_limit(RLIMIT_FSIZE, 4096, 4096) != 0) {
        syscall1(SYS_CLOSE, fd);
        syscall1(SYS_UNLINK, (long)scratch_path);
        return 2;
    }
    cut = syscall2(SYS_FTRUNCATE, fd, 12288);
    shrunk = syscall2(SYS_TRUNCATE, (long)scratch_path, 8192);
    syscall1(SYS_CLOSE, fd);
    if (cut != 0 || shrunk != 0) {
        syscall1(SYS_UNLINK, (long)scratch_path);
        return 5;
    }
    if (syscall4(SYS_RT_SIGACTION, SIGXFSZ, (long)&ignore, 0, 8) != 0) {
        syscall1(SYS_UNLINK, (long)scratch_path);
        return 6;
    }
    fd = syscall3(SYS_OPEN, (long)scratch_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        syscall1(SYS_UNLINK, (long)scratch_path);
        return 3;
    }
    wrote = syscall3(SYS_WRITE, fd, (long)block, (int)sizeof(block));
    again = syscall3(SYS_WRITE, fd, (long)block, 1);
    cut = syscall2(SYS_FTRUNCATE, fd, 8192);
    syscall1(SYS_CLOSE, fd);
    syscall1(SYS_UNLINK, (long)scratch_path);
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
    if (set_limit(RLIMIT_NOFILE, 512, 512) != 0 || syscall1(SYS_SETUID, id_1000) != 0) {
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

/** The limits of `pid`, a process other than the caller: read into `old`
 * and, with `set` nonzero, set to `cur` and `cur`. 0, or a negated errno. */
static int pid_limit(int pid, int set, unsigned cur, struct rlimit64 *old)
{
    struct rlimit64 rl;

    rl.cur_lo = cur;
    rl.cur_hi = 0;
    rl.max_lo = cur;
    rl.max_hi = 0;
    return syscall4(SYS_PRLIMIT64, pid, RLIMIT_NOFILE, set ? (long)&rl : 0, (long)old);
}

/** A zombie keeps its limits until it is reaped: a child that set a
 * descriptor limit of 100 and exited reads 100 to root, takes 90, and is
 * refused to uid 1001 with EPERM; reaped, its pid is ESRCH. Returns 0, or 2
 * to 8 for the step that fails. */
static int body_zombie(void)
{
    struct rlimit64 rl;
    int info[32]; /* siginfo_t, 128 bytes on both architectures */
    int pid = syscall1(SYS_FORK, 0);

    if (pid < 0) {
        return 2;
    }
    if (pid == 0) {
        sys_exit(set_limit(RLIMIT_NOFILE, 100, 100) == 0 ? 0 : 1);
    }
    if (syscall5(SYS_WAITID, P_PID, pid, (long)info, WEXITED | WNOWAIT, 0) != 0 ||
        info[0] != SIGCHLD) {
        return 3;
    }
    if (pid_limit(pid, 0, 0, &rl) != 0 || rl.cur_lo != 100 || rl.max_lo != 100) {
        return 4; /* root reads the zombie's limit */
    }
    if (pid_limit(pid, 1, 90, &rl) != 0 || rl.cur_lo != 100) {
        return 5; /* root sets it */
    }
    if (pid_limit(pid, 0, 0, &rl) != 0 || rl.cur_lo != 90 || rl.max_lo != 90) {
        return 6; /* and reads what it set */
    }
    if (syscall1(SYS_SETUID, id_1001) != 0 || pid_limit(pid, 0, 0, &rl) != -EPERM) {
        return 7; /* another user is refused */
    }
    if (wait_status(pid) != 0 || pid_limit(pid, 0, 0, &rl) != -ESRCH) {
        return 8; /* the reaped pid is gone */
    }
    return 0;
}

/* The pipe on which `body_nproc` tells the parent that its four exist. */
static int report_fd = -1;

/* The pipe whose end of input lets the family's three children exit: the
 * parent closes `release_w` once its rows that count the four are done. */
static int release_r = -1, release_w = -1;

/** Uid 1000 with four processes allowed: three forks succeed and the fourth
 * is EAGAIN; the parent is told once the four exist, and the three children
 * live until the parent closes the release pipe. Returns 0, or 2 to 4 for
 * the step that fails. */
static int body_nproc(void)
{
    int i, pid;
    char byte;

    syscall1(SYS_CLOSE, release_w);
    if (syscall1(SYS_SETUID, id_1000) != 0 || set_limit(RLIMIT_NPROC, 4, 4) != 0) {
        return 2;
    }
    for (i = 0; i < 3; i++) {
        pid = syscall1(SYS_FORK, 0);
        if (pid < 0) {
            return 3;
        }
        if (pid == 0) {
            syscall1(SYS_CLOSE, report_fd);
            syscall3(SYS_READ, release_r, (long)&byte, 1);
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

    if (syscall1(SYS_SETUID, id_1001) != 0) {
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
    if (set_limit(RLIMIT_NPROC, 5, 5) != 0 || syscall1(SYS_SETUID, id_1000) != 0) {
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

/** The probe, entered with `sp` at argc: `run_show` when the first
 * positional argument begins with 's', and otherwise the rows of the opening
 * comment. */
void probe_main(unsigned long *sp)
{
    struct rlimit64 rl;
    static int fds[2], release[2];
    int family, others;
    char byte;

    probe_args((int)sp[0], (char **)(sp + 1));
    if (probe_word(1) != 0 && probe_word(1)[0] == 's') {
        run_show();
    }
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.limitprobe"));
    hello_path = probe_arg("hello", "/hello");
    scratch_path = probe_arg("scratch", "/tmp/limitprobe.f");
    self_path = probe_self("/limitprobe");
    id_1000 = probe_id(1000);
    id_1001 = probe_id(1001);
    if (get_limit(RLIMIT_NOFILE, &rl) != 0 || rl.cur_lo != 1024 || rl.max_lo != 524288) {
        fail(NOFILE_TABLE);
    }
    if (get_limit(RLIMIT_AS, &rl) != 0 || rl.cur_lo != 0xFFFFFFFFu || rl.cur_hi != 0xFFFFFFFFu) {
        fail(AS_UNLIMITED);
    }
    child_must(body_nofile, 0, NOFILE_EIGHT);
    child_must(body_high, 0, NOFILE_HIGH);
    child_must(body_shared, 0, NOFILE_SHARED);
    child_must(body_as, 0, AS_EIGHT);
    child_must(body_hard, 0, HARD_LIMIT);
    child_must(body_sandbox, 0, SANDBOX);
    child_must(body_fsize, 0, FSIZE);
    child_must(body_zombie, 0, ZOMBIE);
    if (set_limit(RLIMIT_NOFILE, 200, 200) != 0) {
        fail(INHERIT_SET);
    }
    child_must(body_inherit, 0, INHERIT);
    if (set_limit(RLIMIT_NOFILE, 200, 1024) != 0 || set_limit(RLIMIT_NOFILE, 1024, 1024) != 0) {
        fail(RAISE_BACK);
    }
    fds[0] = fds[1] = -1;
    if (syscall1(SYS_PIPE, (long)fds) != 0 || syscall1(SYS_PIPE, (long)release) != 0) {
        fail(PIPES);
    }
    report_fd = fds[1];
    release_r = release[0];
    release_w = release[1];
    family = syscall1(SYS_FORK, 0);
    if (family < 0) {
        fail(FAMILY_FORK);
    }
    if (family == 0) {
        sys_exit(body_nproc());
    }
    /* A family that dies unheard is then end of input on the read below. */
    syscall1(SYS_CLOSE, fds[1]);
    if (syscall3(SYS_READ, fds[0], (long)&byte, 1) != 1 || byte != 's') {
        fail(NPROC_FOUR);
    }
    child_must(body_others, 0, OTHERS);
    child_must(body_setuid, 0, SETUID_COUNTS);
    syscall1(SYS_CLOSE, release_w);
    others = wait_status(family);
    if (others != 0) {
        fail(FAMILY_EXIT);
    }
    report_at(CHECKS, 0);
    puts("LIMITPROBE OK");
    sys_exit(0);
}
