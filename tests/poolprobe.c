/**
 * The former pools, crossed: pipes, ptys and sockets past the sixteen each
 * used to have, and the heap where it started after a thousand of each.
 *
 * Eight children each open six pipes, which is twelve of a process's
 * sixteen descriptors, then five pty masters, then six datagram sockets,
 * and hold them while they tell the parent through one shared pipe that
 * they have them; the parent reads the eight bytes before any child lets
 * go, so forty-eight pipes, forty ptys and forty-eight sockets are live at
 * once, three times the old sixteen; and each child forks four children of
 * its own that sleep, forty processes with the parent and the shell, past
 * the sixteen the process table held. Then the parent opens and
 * closes a pipe, a pty and a socket a thousand times each, and the heap's
 * `HeapUsed` in `/proc/meminfo` must be what it was before, with
 * `HeapPages` within two pages of it, so nothing leaked.
 *
 * Prints "POOLPROBE OK" or "POOLPROBE FAIL: reason". Freestanding; int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT       1
#define SYS_FORK       2
#define SYS_READ       3
#define SYS_WRITE      4
#define SYS_OPEN       5
#define SYS_CLOSE      6
#define SYS_WAITPID    7
#define SYS_PIPE       42
#define SYS_SOCKETCALL 102
#define SYS_NANOSLEEP  162

#define SC_SOCKET  1
#define AF_INET    2
#define SOCK_DGRAM 2
#define O_RDONLY   0
#define O_RDWR     2

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
    syscall3(SYS_WRITE, 1, (int)(size_t)"POOLPROBE FAIL: ", 16);
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

static int open_socket(void)
{
    unsigned args[3];

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    return syscall2(SYS_SOCKETCALL, SC_SOCKET, (int)(size_t)args);
}

/* A field of /proc/meminfo, in its own units, or 0xFFFFFFFF when absent. */
static unsigned meminfo(const char *key)
{
    static char buf[1024];
    int fd = syscall3(SYS_OPEN, (int)(size_t)"/proc/meminfo", O_RDONLY, 0);
    int n, i;

    if (fd < 0) {
        return 0xFFFFFFFFu;
    }
    n = syscall3(SYS_READ, fd, (int)(size_t)buf, (int)sizeof(buf) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        return 0xFFFFFFFFu;
    }
    buf[n] = 0;
    for (i = 0; i < n; i++) {
        int k;

        for (k = 0; key[k] && buf[i + k] == key[k]; k++) {
        }
        if (!key[k]) {
            unsigned v = 0;

            i += k;
            while (buf[i] == ' ') {
                i++;
            }
            while (buf[i] >= '0' && buf[i] <= '9') {
                v = v * 10 + (unsigned)(buf[i] - '0');
                i++;
            }
            return v;
        }
    }
    return 0xFFFFFFFFu;
}

/* A child: `kind` objects opened and held while the parent counts. */
static void hold(int kind, int count, int report)
{
    int fds[16];
    int i;

    for (i = 0; i < count; i++) {
        int fd;

        if (kind == 0) {
            int pair[2];

            pair[0] = pair[1] = -1;
            if (syscall1(SYS_PIPE, (int)(size_t)pair) != 0) {
                sys_exit(2);
            }
            fd = pair[0];
            fds[i * 2 + 1] = pair[1];
        } else if (kind == 1) {
            fd = syscall3(SYS_OPEN, (int)(size_t)"/dev/ptmx", O_RDWR, 0);
        } else if (kind == 2) {
            fd = open_socket();
        } else {
            int pid = syscall1(SYS_FORK, 0);

            if (pid < 0) {
                sys_exit(3);
            }
            if (pid == 0) {
                sleep_ms(800);
                sys_exit(0);
            }
            fd = pid;
        }
        if (fd < 0) {
            sys_exit(3);
        }
        fds[kind == 0 ? i * 2 : i] = fd;
    }
    syscall3(SYS_WRITE, report, (int)(size_t)"h", 1);
    sleep_ms(800);
    if (kind == 3) {
        for (i = 0; i < count; i++) {
            int status;

            syscall3(SYS_WAITPID, fds[i], (int)(size_t)&status, 0);
        }
    }
    sys_exit(0);
}

static void crossed(int kind, int per_child, const char *what)
{
    int report[2];
    int pids[8];
    int i;
    char byte;

    report[0] = report[1] = -1;
    if (syscall1(SYS_PIPE, (int)(size_t)report) != 0) {
        fail("the report pipe");
    }
    for (i = 0; i < 8; i++) {
        pids[i] = syscall1(SYS_FORK, 0);
        if (pids[i] < 0) {
            fail("fork of a holder");
        }
        if (pids[i] == 0) {
            syscall1(SYS_CLOSE, report[0]);
            hold(kind, per_child, report[1]);
        }
    }
    for (i = 0; i < 8; i++) {
        if (syscall3(SYS_READ, report[0], (int)(size_t)&byte, 1) != 1 || byte != 'h') {
            fail(what);
        }
    }
    for (i = 0; i < 8; i++) {
        int status = -1;

        if (syscall3(SYS_WAITPID, pids[i], (int)(size_t)&status, 0) != pids[i] || status != 0) {
            fail(what);
        }
    }
    syscall1(SYS_CLOSE, report[0]);
    syscall1(SYS_CLOSE, report[1]);
}

static void cycle(int kind, const char *what)
{
    unsigned used0 = meminfo("HeapUsed:"), pages0 = meminfo("HeapPages:");
    unsigned used1, pages1;
    int i;

    for (i = 0; i < 1000; i++) {
        if (kind == 0) {
            int pair[2];

            pair[0] = pair[1] = -1;
            if (syscall1(SYS_PIPE, (int)(size_t)pair) != 0) {
                fail(what);
            }
            syscall1(SYS_CLOSE, pair[0]);
            syscall1(SYS_CLOSE, pair[1]);
        } else {
            int fd =
                kind == 1 ? syscall3(SYS_OPEN, (int)(size_t)"/dev/ptmx", O_RDWR, 0) : open_socket();

            if (fd < 0) {
                fail(what);
            }
            syscall1(SYS_CLOSE, fd);
        }
    }
    used1 = meminfo("HeapUsed:");
    pages1 = meminfo("HeapPages:");
    if (used1 != used0 || pages1 + 2 < pages0 || pages1 > pages0 + 2) {
        fail(what);
    }
}

void probe_main(unsigned int *sp);

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(unsigned int *sp)
{
    (void)sp;
    if (meminfo("HeapUsed:") == 0xFFFFFFFFu) {
        fail("HeapUsed in /proc/meminfo");
    }
    crossed(0, 6, "forty-eight pipes at once");
    crossed(1, 5, "forty ptys at once");
    crossed(2, 6, "forty-eight sockets at once");
    crossed(3, 4, "forty processes at once");
    cycle(0, "a thousand pipes made and closed left the heap changed");
    cycle(1, "a thousand ptys made and closed left the heap changed");
    cycle(2, "a thousand sockets made and closed left the heap changed");
    puts("POOLPROBE OK");
    sys_exit(0);
}
