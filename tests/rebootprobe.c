/**
 * Drives reboot(2) at every answer that is not a shutdown, and leaves the
 * machine running.
 *
 * A fault in this probe can end every probe after it: they share one boot
 * (runtests), and a well-formed `RB_POWER_OFF` from a caller who is still
 * root stops the machine and the rest of the run with it. So the
 * unprivileged case checks its own precondition before it makes the call:
 * the child requires `setuid` to return 0 and `geteuid` to read back the new
 * uid, and exits with a status of its own, having made no call, if either
 * fails.
 *
 * The cases: a correct call with `RB_DISABLE_CAD` returns 0, and so does each
 * of the three alternate second magics, the one way to show that the kernel
 * accepts them rather than refusing them for some other reason;
 * `RB_ENABLE_CAD`, `RB_KEXEC`, a zero first magic, a zero second magic and a
 * wrong second magic each return -EINVAL; and an unprivileged caller asking
 * for a power-off gets -EPERM.
 *
 * Prints "REBOOTPROBE OK" at the end, which also shows that nothing here
 * stopped the machine.
 *
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

typedef unsigned int uint32_t;

#define STDOUT_FILENO 1

/* The magics musl passes for a caller of reboot(2), and the four commands
 * this probe makes, none of which stops the machine as the probe makes it. */
#define MAGIC1        0xfee1deadu
#define MAGIC2        672274793u
#define MAGIC2A       85072278u
#define MAGIC2B       369367448u
#define MAGIC2C       537993216u
#define CMD_CAD_OFF   0x00000000u
#define CMD_CAD_ON    0x89abcdefu
#define CMD_KEXEC     0x45584543u
#define CMD_POWER_OFF 0x4321fedcu

#define PROBE_UID 1000

#define WANT_EPERM  (-1)
#define WANT_EINVAL (-22)

/** write(2): the bytes written, or a negated errno. */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** exit(2) with `status`; does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** fork(2): the child's pid, 0 in the child, or a negated errno. */
static int sys_fork(void)
{
    return syscall0(SYS_FORK);
}

/** waitpid(2): the pid reaped, or a negated errno. */
static int sys_waitpid(int pid, int *status, int options)
{
    return syscall3(SYS_WAITPID, (long)pid, (long)status, (long)options);
}

/** setuid(2): 0, or a negated errno. */
static int sys_setuid(int uid)
{
    return syscall1(SYS_SETUID, (long)uid);
}

/** geteuid(2): the effective uid. */
static int sys_geteuid(void)
{
    return syscall0(SYS_GETEUID);
}

/** reboot(2) with the two magics and `cmd`: 0 or a negated errno, and no
 * return when the call stops the machine. */
static int sys_reboot(uint32_t magic1, uint32_t magic2, uint32_t cmd)
{
    return syscall3(SYS_REBOOT, (long)magic1, (long)magic2, (long)cmd);
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

/** Write `s` to standard output. */
static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    print(s);
    sys_write(STDOUT_FILENO, "\n", 1);
}

/** Write `n` in decimal. */
static void print_num(int n)
{
    char buf[12];
    int i = 0, neg = 0;
    if (n < 0) {
        neg = 1;
        n = -n;
    }
    if (n == 0)
        buf[i++] = '0';
    while (n > 0) {
        buf[i++] = (char)('0' + n % 10);
        n /= 10;
    }
    if (neg)
        sys_write(STDOUT_FILENO, "-", 1);
    while (i > 0)
        sys_write(STDOUT_FILENO, &buf[--i], 1);
}

static int failures;

/** Require `got` to equal `want`, naming the case. */
static void expect(const char *what, int got, int want)
{
    if (got == want) {
        return;
    }
    print("REBOOTPROBE FAIL: ");
    print(what);
    print(": got ");
    print_num(got);
    print(", wanted ");
    print_num(want);
    puts("");
    failures++;
}

/**
 * Make each call of the case list and count the answers that differ. The
 * child of the unprivileged case exits 2 when it could not drop root, having
 * made no call, 3 when its power-off was not refused with -EPERM, and 0 when
 * it was. Exits 0 when every case held, else 1.
 */
PROBE_START void _start(void)
{
    int child, status = 0;

    puts("reboot(2) probe starting...");

    expect("disable-cad with the usual magic", sys_reboot(MAGIC1, MAGIC2, CMD_CAD_OFF), 0);
    expect("disable-cad with alternate magic a", sys_reboot(MAGIC1, MAGIC2A, CMD_CAD_OFF), 0);
    expect("disable-cad with alternate magic b", sys_reboot(MAGIC1, MAGIC2B, CMD_CAD_OFF), 0);
    expect("disable-cad with alternate magic c", sys_reboot(MAGIC1, MAGIC2C, CMD_CAD_OFF), 0);

    expect("enable-cad", sys_reboot(MAGIC1, MAGIC2, CMD_CAD_ON), WANT_EINVAL);
    expect("kexec", sys_reboot(MAGIC1, MAGIC2, CMD_KEXEC), WANT_EINVAL);
    expect("no first magic", sys_reboot(0, MAGIC2, CMD_CAD_OFF), WANT_EINVAL);
    expect("no second magic", sys_reboot(MAGIC1, 0, CMD_CAD_OFF), WANT_EINVAL);
    expect("wrong second magic", sys_reboot(MAGIC1, MAGIC2 + 1, CMD_CAD_OFF), WANT_EINVAL);

    child = sys_fork();
    if (child == 0) {
        if (sys_setuid(PROBE_UID) != 0 || sys_geteuid() != PROBE_UID) {
            sys_exit(2);
        }
        if (sys_reboot(MAGIC1, MAGIC2, CMD_POWER_OFF) != WANT_EPERM) {
            sys_exit(3);
        }
        sys_exit(0);
    }
    if (child < 0) {
        puts("REBOOTPROBE FAIL: fork");
        failures++;
    } else if (sys_waitpid(child, &status, 0) != child) {
        puts("REBOOTPROBE FAIL: waitpid");
        failures++;
    } else if ((status & 0x7f) != 0) {
        puts("REBOOTPROBE FAIL: the unprivileged child did not exit normally");
        failures++;
    } else if (((status >> 8) & 0xff) == 2) {
        puts("REBOOTPROBE FAIL: the child could not drop privilege, so it asked nothing");
        failures++;
    } else if (((status >> 8) & 0xff) != 0) {
        puts("REBOOTPROBE FAIL: an unprivileged power-off was not refused with EPERM");
        failures++;
    }

    if (failures == 0) {
        puts("REBOOTPROBE OK");
        sys_exit(0);
    }
    sys_exit(1);
}
