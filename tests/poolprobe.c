/**
 * poolprobe: many pipes, ptys, sockets and processes live at once, and the
 * kernel heap back where it started after a thousand of each object.
 *
 * In each of four rounds, eight children each open and hold six pipes, or
 * five pty masters, or six datagram sockets, or fork four children of their
 * own, and tell the parent through one shared pipe that they have them; each
 * then waits for the parent's word before letting go, and the parent gives
 * it only after all eight have reported. So forty-eight pipes, forty ptys,
 * forty-eight sockets and, in the last round, forty processes besides the
 * parent and the shell are live at once. Then the parent opens and closes a
 * pipe, a pty and a socket a thousand times each, and the heap's `HeapUsed`
 * in `/proc/meminfo` must be what it was before, with `HeapPages` within two
 * pages of it, so nothing leaked.
 *
 * Prints "POOLPROBE OK" and exits 0, or "POOLPROBE FAIL: " and the reason
 * and exits 1.
 */

#include "probe.h"

#define AF_INET    2
#define SOCK_DGRAM 2
#define O_RDONLY   0
#define O_RDWR     2

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

/** Print "POOLPROBE FAIL: " and `why`, and exit 1. */
static void fail(const char *why)
{
    syscall3(SYS_WRITE, 1, (long)"POOLPROBE FAIL: ", 16);
    puts(why);
    sys_exit(1);
}

/** Sleep for `ms` milliseconds. */
static void sleep_ms(int ms)
{
    long ts[2];

    ts[0] = ms / 1000;
    ts[1] = (ms % 1000) * 1000000;
    syscall2(SYS_NANOSLEEP, (long)ts, 0);
}

/** A UDP socket: its descriptor, or a negated errno. */
static int open_socket(void)
{
    unsigned long args[3];

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    return sock_call(SC_SOCKET, args);
}

/** The value of the field `key`, with its colon, of /proc/meminfo, in its
 * own units, or 0xFFFFFFFF when the file or the field is absent. */
static unsigned meminfo(const char *key)
{
    static char buf[1024];
    int fd = syscall3(SYS_OPEN, (long)"/proc/meminfo", O_RDONLY, 0);
    int n, i;

    if (fd < 0) {
        return 0xFFFFFFFFu;
    }
    n = syscall3(SYS_READ, fd, (long)buf, (int)sizeof(buf) - 1);
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

static int ack_fd = -1; /* the parent's word that every holder has reported */

/** Block until the parent writes its word, a byte, to `ack_fd`. */
static void wait_for_word(void)
{
    char byte;

    syscall3(SYS_READ, ack_fd, (long)&byte, 1);
}

/**
 * A holder's half: open `count` objects of `kind` (0 a pipe, 1 a pty master,
 * 2 a datagram socket, 3 a child that waits for the word), write "h" to
 * `report`, wait for the word, reap any children, and exit 0. It exits 2
 * when a pipe fails and 3 when another object or a fork fails. `count` is
 * at most 8 for pipes and 16 for the others.
 */
static void hold(int kind, int count, int report)
{
    int fds[16];
    int i;

    for (i = 0; i < count; i++) {
        int fd;

        if (kind == 0) {
            int pair[2];

            pair[0] = pair[1] = -1;
            if (syscall1(SYS_PIPE, (long)pair) != 0) {
                sys_exit(2);
            }
            fd = pair[0];
            fds[i * 2 + 1] = pair[1];
        } else if (kind == 1) {
            fd = syscall3(SYS_OPEN, (long)"/dev/ptmx", O_RDWR, 0);
        } else if (kind == 2) {
            fd = open_socket();
        } else {
            int pid = syscall1(SYS_FORK, 0);

            if (pid < 0) {
                sys_exit(3);
            }
            if (pid == 0) {
                wait_for_word();
                sys_exit(0);
            }
            fd = pid;
        }
        if (fd < 0) {
            sys_exit(3);
        }
        fds[kind == 0 ? i * 2 : i] = fd;
    }
    syscall3(SYS_WRITE, report, (long)"h", 1);
    wait_for_word();
    if (kind == 3) {
        for (i = 0; i < count; i++) {
            int status;

            syscall3(SYS_WAITPID, fds[i], (long)&status, 0);
        }
    }
    sys_exit(0);
}

/** One round: eight holders of `per_child` objects of `kind` each, all live
 * at once; fail with `what` unless every holder reports and exits 0. */
static void crossed(int kind, int per_child, const char *what)
{
    int report[2];
    int ack[2];
    int pids[8];
    int i;
    char byte;

    report[0] = report[1] = -1;
    ack[0] = ack[1] = -1;
    if (syscall1(SYS_PIPE, (long)report) != 0 || syscall1(SYS_PIPE, (long)ack) != 0) {
        fail("the report pipe");
    }
    ack_fd = ack[0];
    for (i = 0; i < 8; i++) {
        pids[i] = syscall1(SYS_FORK, 0);
        if (pids[i] < 0) {
            fail("fork of a holder");
        }
        if (pids[i] == 0) {
            syscall1(SYS_CLOSE, report[0]);
            syscall1(SYS_CLOSE, ack[1]);
            hold(kind, per_child, report[1]);
        }
    }
    syscall1(SYS_CLOSE, report[1]); /* a holder that dies unheard is end of input */
    for (i = 0; i < 8; i++) {
        if (syscall3(SYS_READ, report[0], (long)&byte, 1) != 1 || byte != 'h') {
            fail(what);
        }
    }
    /* One byte of the word for each waiter, holders and their children. */
    for (i = 0; i < 8 * (1 + (kind == 3 ? per_child : 0)); i++) {
        syscall3(SYS_WRITE, ack[1], (long)"g", 1);
    }
    for (i = 0; i < 8; i++) {
        int status = -1;

        if (syscall3(SYS_WAITPID, pids[i], (long)&status, 0) != pids[i] || status != 0) {
            fail(what);
        }
    }
    syscall1(SYS_CLOSE, report[0]);
    syscall1(SYS_CLOSE, ack[0]);
    syscall1(SYS_CLOSE, ack[1]);
    ack_fd = -1;
}

/** Make and close a thousand objects of `kind` (0 a pipe, 1 a pty master, 2
 * a socket), and fail with `what` unless HeapUsed is unchanged and
 * HeapPages within two pages of where it was. */
static void cycle(int kind, const char *what)
{
    unsigned used0 = meminfo("HeapUsed:"), pages0 = meminfo("HeapPages:");
    unsigned used1, pages1;
    int i;

    for (i = 0; i < 1000; i++) {
        if (kind == 0) {
            int pair[2];

            pair[0] = pair[1] = -1;
            if (syscall1(SYS_PIPE, (long)pair) != 0) {
                fail(what);
            }
            syscall1(SYS_CLOSE, pair[0]);
            syscall1(SYS_CLOSE, pair[1]);
        } else {
            int fd = kind == 1 ? syscall3(SYS_OPEN, (long)"/dev/ptmx", O_RDWR, 0) : open_socket();

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

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe: the four rounds, then the three cycles. */
void probe_main(unsigned long *sp)
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
