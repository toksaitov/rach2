/**
 * Drives reboot(2) at every answer that is not a shutdown, and leaves the
 * machine running.
 *
 * This is the one probe in the tree whose misbehaviour destroys every probe
 * after it: all of them share a single boot, and a syntactically valid
 * `RB_POWER_OFF` issued by a caller who is still root stops the machine and
 * takes the rest of the run with it. So the unprivileged case asserts its own
 * precondition before it asks for anything: the child requires `setuid` to
 * return 0 and `geteuid` to read back the new uid, and exits with a distinct
 * status WITHOUT issuing any command if either fails.
 *
 * The cases: a correct call with `RB_DISABLE_CAD` returns 0, and so does each
 * of the three alternate second magics, which is the only way to pin that they
 * are accepted, not rejected for some other reason; `RB_ENABLE_CAD`,
 * `RB_KEXEC`, a zero first magic and a zero second magic each return -EINVAL;
 * and an unprivileged caller asking for a real power-off gets -EPERM.
 *
 * Prints "REBOOTPROBE OK" at the end, which is also the proof that nothing
 * here stopped the machine.
 *
 * Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;
typedef unsigned int uint32_t;

#define STDOUT_FILENO 1

#define SYS_EXIT      1
#define SYS_FORK      2
#define SYS_WRITE     4
#define SYS_WAITPID   7
#define SYS_REBOOT    88
#define SYS_GETEUID32 201
#define SYS_SETUID32  213

/* The magics musl passes for a caller of reboot(2), and the four commands this
 * kernel answers without stopping. */
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

static int sys_write(int fd, const void *buf, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count)
                         : "memory");
    return r;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
    __builtin_unreachable();
}

static int sys_fork(void)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_FORK));
    return r;
}

static int sys_waitpid(int pid, int *status, int options)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_WAITPID), "b"(pid), "c"(status), "d"(options)
                         : "memory");
    return r;
}

static int sys_setuid(int uid)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_SETUID32), "b"(uid));
    return r;
}

static int sys_geteuid(void)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_GETEUID32));
    return r;
}

static int sys_reboot(uint32_t magic1, uint32_t magic2, uint32_t cmd)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_REBOOT), "b"(magic1), "c"(magic2), "d"(cmd));
    return r;
}

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}
static void puts(const char *s)
{
    print(s);
    sys_write(STDOUT_FILENO, "\n", 1);
}

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

void _start(void)
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

    /* The unprivileged refusal, in a child that proves it is unprivileged
     * before it asks for a power-off. Exit 2 means the drop failed and nothing
     * was asked; exit 3 means the answer was wrong. */
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
