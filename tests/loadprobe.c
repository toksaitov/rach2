/**
 * The machine under load, and the longest delays it saw.
 *
 * Six children spin, two write a file of 256 KiB on the mounted volume and
 * sync it over and over, one pings the gateway through the kernel's own
 * ping every fifty milliseconds, and one reads lines from the console,
 * which the harness types every two seconds, for twenty seconds. Then the
 * parent kills them all, reads `/proc/stat`, and prints the four delays the
 * kernel kept as its longest: the gap between two timer ticks, a block
 * request from its taking to its completion, a wait for the request limit,
 * and a console line from readable to read. The timer's gap must stay
 * under a fifth of a second, the disk's completion under its five-second
 * deadline, and the console's line under a second; at least three lines
 * must have been read. The numbers are the record milestone 50 keeps of
 * the machine.
 *
 * Prints "LOADPROBE timer N disk N diskwait N console N" and then
 * "LOADPROBE OK" or "LOADPROBE FAIL: reason". Freestanding; int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT       1
#define SYS_FORK       2
#define SYS_READ       3
#define SYS_WRITE      4
#define SYS_OPEN       5
#define SYS_CLOSE      6
#define SYS_WAITPID    7
#define SYS_MOUNT      21
#define SYS_SYNC       36
#define SYS_KILL       37
#define SYS_MKDIR      39
#define SYS_PIPE       42
#define SYS_SOCKETCALL 102
#define SYS_NANOSLEEP  162

#define SC_PING    0x1000
#define GATEWAY_IP 0x0A000202u

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_TRUNC  01000
#define SIGKILL  9

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

static int sys5(int n, int a, int b, int c, int d, int e)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                         : "memory");
    return r;
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

static void print(const char *s)
{
    syscall3(SYS_WRITE, 1, (int)(size_t)s, (int)strlen(s));
}

static void puts(const char *s)
{
    print(s);
    print("\n");
}

static void print_num(unsigned v)
{
    char tmp[12];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        syscall3(SYS_WRITE, 1, (int)(size_t)&tmp[--n], 1);
    }
}

static void fail(const char *why)
{
    print("LOADPROBE FAIL: ");
    puts(why);
    sys_exit(1);
}

static void sleep_ms(int ms)
{
    int ts[2];

    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000;
    syscall2(SYS_NANOSLEEP, (int)(size_t)ts, 0);
}

static unsigned char block[4096];

/* A writer: the file written whole and synced, again and again. */
static int writer_syncs = 1;

static void writer(const char *path)
{
    unsigned i;

    for (i = 0; i < sizeof(block); i++) {
        block[i] = (unsigned char)i;
    }
    for (;;) {
        int fd = syscall3(SYS_OPEN, (int)(size_t)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (fd < 0) {
            sys_exit(2);
        }
        for (i = 0; i < 64; i++) {
            if (syscall3(SYS_WRITE, fd, (int)(size_t)block, (int)sizeof(block)) !=
                (int)sizeof(block)) {
                sys_exit(3);
            }
        }
        syscall1(SYS_CLOSE, fd);
        if (writer_syncs) {
            syscall1(SYS_SYNC, 0);
        }
        /* A breath between files: a process that is always inside a system
           call is never preempted, and the others would not run; entry 6 of
           milestone 50 records that. */
        sleep_ms(10);
    }
}

static void pinger(void)
{
    unsigned args[3];

    for (;;) {
        args[0] = GATEWAY_IP;
        syscall2(SYS_SOCKETCALL, SC_PING, (int)(size_t)args);
        sleep_ms(50);
    }
}

/* The console reader: lines counted into the report pipe, one byte each. */
static void reader(int report)
{
    char c;

    for (;;) {
        if (syscall3(SYS_READ, 0, (int)(size_t)&c, 1) == 1 && c == '\n') {
            syscall3(SYS_WRITE, report, (int)(size_t)"l", 1);
        }
    }
}

/* A field of /proc/stat, or 0xFFFFFFFF when absent. */
static unsigned stat_field(const char *key)
{
    static char text[4096];
    int fd = syscall3(SYS_OPEN, (int)(size_t)"/proc/stat", O_RDONLY, 0);
    int n, i;

    if (fd < 0) {
        return 0xFFFFFFFFu;
    }
    n = syscall3(SYS_READ, fd, (int)(size_t)text, (int)sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        return 0xFFFFFFFFu;
    }
    text[n] = 0;
    for (i = 0; i < n; i++) {
        int k;

        for (k = 0; key[k] && text[i + k] == key[k]; k++) {
        }
        if (!key[k] && (i == 0 || text[i - 1] == '\n')) {
            unsigned v = 0;

            i += k;
            while (text[i] == ' ') {
                i++;
            }
            while (text[i] >= '0' && text[i] <= '9') {
                v = v * 10 + (unsigned)(text[i] - '0');
                i++;
            }
            return v;
        }
    }
    return 0xFFFFFFFFu;
}

void probe_main(unsigned int *sp);

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

/* Which of the four loads run: every one unless the argument names some
   by their first letters, `s`pinners, `w`riters, `p`inger, `r`eader. */
static int wanted(const char *arg, char kind)
{
    int i;

    if (!arg) {
        return 1;
    }
    for (i = 0; arg[i]; i++) {
        if (arg[i] == kind) {
            return 1;
        }
    }
    return 0;
}

void probe_main(unsigned int *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    const char *arg = argc >= 2 ? argv[1] : 0;
    int pids[10];
    int report[2];
    int i, lines = 0;
    unsigned timer, disk, diskwait, console;
    char byte;

    if (syscall2(SYS_MKDIR, (int)(size_t)"/lo", 0755) != 0 ||
        sys5(SYS_MOUNT, (int)(size_t)"/dev/disk0p1", (int)(size_t)"/lo",
             (int)(size_t)(wanted(arg, 'k') && arg ? "krffs" : "vfat"), 0, 0) != 0) {
        fail("the mount of the volume");
    }
    report[0] = report[1] = -1;
    if (syscall1(SYS_PIPE, (int)(size_t)report) != 0) {
        fail("the report pipe");
    }
    for (i = 0; i < 10; i++) {
        pids[i] = syscall1(SYS_FORK, 0);
        if (pids[i] < 0) {
            fail("fork of a load process");
        }
        if (pids[i] == 0) {
            syscall1(SYS_CLOSE, report[0]);
            if (i < 6) {
                if (wanted(arg, 's')) {
                    for (;;) {
                    }
                }
            } else if (i < 8) {
                writer_syncs = !wanted(arg, 'n');
                if (wanted(arg, 'w') || (i == 6 && wanted(arg, 'W'))) {
                    writer(i == 6 ? "/lo/load1.bin" : "/lo/load2.bin");
                }
            } else if (i == 8) {
                if (wanted(arg, 'p')) {
                    pinger();
                }
            } else if (wanted(arg, 'r')) {
                reader(report[1]);
            }
            sleep_ms(30000);
            sys_exit(0);
        }
    }
    syscall1(SYS_CLOSE, report[1]);
    sleep_ms(20000);
    for (i = 0; i < 10; i++) {
        syscall2(SYS_KILL, pids[i], SIGKILL);
    }
    for (i = 0; i < 10; i++) {
        int status;

        syscall3(SYS_WAITPID, pids[i], (int)(size_t)&status, 0);
    }
    while (syscall3(SYS_READ, report[0], (int)(size_t)&byte, 1) == 1) {
        lines++;
    }
    timer = stat_field("latency_timer_us ");
    disk = stat_field("latency_disk_us ");
    diskwait = stat_field("latency_diskwait_us ");
    console = stat_field("latency_console_us ");
    print("LOADPROBE timer ");
    print_num(timer);
    print(" disk ");
    print_num(disk);
    print(" diskwait ");
    print_num(diskwait);
    print(" console ");
    print_num(console);
    print(" lines ");
    print_num((unsigned)lines);
    print("\n");
    if (timer == 0xFFFFFFFFu || disk == 0xFFFFFFFFu || console == 0xFFFFFFFFu) {
        fail("/proc/stat has no latency lines");
    }
    if (lines < 3 && wanted(arg, 'r')) {
        fail("the console reader had fewer than three lines");
    }
    if (timer > 200000u) {
        fail("a timer tick was more than a fifth of a second late");
    }
    if (disk > 5000000u) {
        fail("a request took longer than its deadline");
    }
    if (console > 1000000u) {
        fail("a console line waited more than a second for its reader");
    }
    puts("LOADPROBE OK");
    sys_exit(0);
}
