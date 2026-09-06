/**
 * The resource limits, proved from user space.
 *
 * `prlimit64` keeps three limits in the process record: the processes of a
 * real user (`RLIMIT_NPROC`), a process's open descriptors (`RLIMIT_NOFILE`)
 * and its address space in bytes (`RLIMIT_AS`), each a soft value enforced
 * and a hard value the soft may be raised to. The probe runs as root and
 * proves, one child at a time: the limits read back as the table's; a
 * descriptor limit of eight refuses the ninth open with `EMFILE` and a
 * `dup2` onto a number past it with `EBADF`, and an
 * address-space limit of eight megabytes refuses a larger mapping with
 * `ENOMEM` and leaves the break where it was; a hard limit raised by a
 * user is refused with `EPERM` while a lowering and root's raising are
 * allowed; the limits are seen after a fork and after an exec, which is
 * `limitprobe show NOFILE` reporting the number it inherited; one user
 * with four processes allowed is refused its fifth fork with `EAGAIN`
 * while another user forks; and a process that took that user's identity
 * with `setuid` counts against that user's four.
 *
 * Prints "LIMITPROBE OK" or "LIMITPROBE FAIL: reason". Freestanding;
 * int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT      1
#define SYS_FORK      2
#define SYS_READ      3
#define SYS_WRITE     4
#define SYS_OPEN      5
#define SYS_CLOSE     6
#define SYS_WAITPID   7
#define SYS_DUP2      63
#define SYS_EXECVE    11
#define SYS_BRK       45
#define SYS_PIPE      42
#define SYS_NANOSLEEP 162
#define SYS_MMAP2     192
#define SYS_SETUID32  213
#define SYS_PRLIMIT64 340

#define RLIMIT_NPROC  6
#define RLIMIT_NOFILE 7
#define RLIMIT_AS     9

#define EPERM  1
#define EAGAIN 11
#define ENOMEM 12
#define EINVAL 22
#define EMFILE 24
#define EBADF  9

#define O_RDONLY 0

struct rlimit64 {
    unsigned cur_lo, cur_hi, max_lo, max_hi;
};

static int syscall1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}

static int syscall2(int n, int a, int b)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static int syscall4(int n, int a, int b, int c, int d)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d) : "memory");
    return r;
}

static unsigned mmap_anon(unsigned len)
{
    unsigned result;
    __asm__ __volatile__("push %%ebp\n\t"
                         "xor %%ebp, %%ebp\n\t"
                         "int $0x80\n\t"
                         "pop %%ebp\n\t"
                         : "=a"(result)
                         : "0"(SYS_MMAP2), "b"(0), "c"(len), "d"(3), "S"(0x22), "D"(-1)
                         : "memory");
    return result;
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

static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (int)(size_t)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (int)(size_t)"\n", 1);
}

static void fail(const char *why)
{
    syscall3(SYS_WRITE, 1, (int)(size_t)"LIMITPROBE FAIL: ", 17);
    puts(why);
    sys_exit(1);
}

static int get_limit(int resource, struct rlimit64 *rl)
{
    return syscall4(SYS_PRLIMIT64, 0, resource, 0, (int)(size_t)rl);
}

static int set_limit(int resource, unsigned cur, unsigned max)
{
    struct rlimit64 rl;

    rl.cur_lo = cur;
    rl.cur_hi = 0;
    rl.max_lo = max;
    rl.max_hi = 0;
    return syscall4(SYS_PRLIMIT64, 0, resource, (int)(size_t)&rl, 0);
}

static int wait_status(int pid)
{
    int status = -1;

    if (syscall3(SYS_WAITPID, pid, (int)(size_t)&status, 0) != pid) {
        return -1;
    }
    return (status >> 8) & 0xff;
}

static void sleep_ms(int ms)
{
    int ts[2];

    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000;
    syscall2(SYS_NANOSLEEP, (int)(size_t)ts, 0);
}

/* A child that runs `body` and exits with its answer; the parent requires
   `want`. */
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

/* Eight descriptors: the opens past the three of stdio stop at the ninth,
   with EMFILE, and the limit is seen across an exec. */
static int body_nofile(void)
{
    int i, fd = 0;
    char *argv[3];

    if (set_limit(RLIMIT_NOFILE, 8, 8) != 0) {
        return 2;
    }
    for (i = 0; i < 8; i++) {
        fd = syscall3(SYS_OPEN, (int)(size_t)"/hello", O_RDONLY, 0);
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
    syscall3(SYS_EXECVE, (int)(size_t)argv[0], (int)(size_t)argv, 0);
    return 4;
}

/* Eight megabytes of address space: a mapping of sixteen is refused, one of
   one is not, and the break does not move past the limit. */
static int body_as(void)
{
    unsigned r;
    int brk0, brk1;

    if (set_limit(RLIMIT_AS, 8u << 20, 8u << 20) != 0) {
        return 2;
    }
    r = mmap_anon(16u << 20);
    if (r != (unsigned)-ENOMEM) {
        return 3;
    }
    r = mmap_anon(1u << 20);
    if (r >= 0xFFFFF000u) {
        return 4;
    }
    brk0 = syscall1(SYS_BRK, 0);
    brk1 = syscall1(SYS_BRK, brk0 + (16 << 20));
    if (brk1 != brk0) {
        return 5;
    }
    return 0;
}

/* The hard limit: a user may lower it and not raise it; root raises it. */
static int body_hard(void)
{
    if (set_limit(RLIMIT_NOFILE, 512, 512) != 0) {
        return 2; /* root lowering */
    }
    if (set_limit(RLIMIT_NOFILE, 512, 1024) != 0) {
        return 3; /* root raising it back */
    }
    if (set_limit(RLIMIT_NOFILE, 512, 512) != 0 || syscall1(SYS_SETUID32, 1000) != 0) {
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

static int report_fd = -1;

/* Uid 1000 with four processes allowed: three forks succeed and the fourth
   is EAGAIN; the parent is told when the family stands. */
static int body_nproc(void)
{
    int i, pid;

    if (syscall1(SYS_SETUID32, 1000) != 0 || set_limit(RLIMIT_NPROC, 4, 4) != 0) {
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
    syscall3(SYS_WRITE, report_fd, (int)(size_t)"s", 1);
    for (i = 0; i < 3; i++) {
        syscall3(SYS_WAITPID, -1, 0, 0);
    }
    return 0;
}

/* Uid 1001 beside it forks; a root process that becomes uid 1000 with two
   allowed finds the four already there and is refused. */
static int body_others(void)
{
    int pid;

    if (syscall1(SYS_SETUID32, 1001) != 0) {
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

static int body_setuid(void)
{
    if (set_limit(RLIMIT_NPROC, 2, 2) != 0 || syscall1(SYS_SETUID32, 1000) != 0) {
        return 2;
    }
    return syscall1(SYS_FORK, 0) == -EAGAIN ? 0 : 3;
}

static void run_show(void)
{
    struct rlimit64 rl;

    if (get_limit(RLIMIT_NOFILE, &rl) != 0 || rl.cur_lo != 8 || rl.max_lo != 8) {
        sys_exit(9);
    }
    sys_exit(0);
}

void probe_main(unsigned int *sp);

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(unsigned int *sp)
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
    fds[0] = fds[1] = -1;
    if (syscall1(SYS_PIPE, (int)(size_t)fds) != 0) {
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
    if (syscall3(SYS_READ, fds[0], (int)(size_t)&byte, 1) != 1 || byte != 's') {
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
