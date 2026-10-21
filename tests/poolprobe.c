/**
 * poolprobe: many pipes, ptys, sockets and processes live at once, and the
 * kernel heap back where it started after a thousand each of pipes, ptys
 * and sockets.
 *
 * `poolprobe live` runs four rounds. In each, eight children each open and
 * hold six pipes, or five pty masters, or six datagram sockets, or fork four
 * children of their own, and tell the parent through one shared pipe that
 * they have them; each then waits for the parent's word before letting go,
 * and the parent gives it only after all eight have reported. So forty-eight
 * pipes, forty ptys, forty-eight sockets and, in the last round, forty
 * processes besides the parent are live at once.
 *
 * `poolprobe heap` opens and closes a pipe, a pty and a socket a thousand
 * times each. After a thousand, the heap's `HeapUsed` in `/proc/meminfo`
 * must be what it was before, and `HeapPages` within two pages of it, so
 * nothing leaked. One object of each kind is made and closed before the
 * first reading, since the first of a kind may set up state the kernel keeps
 * for every later one, which is no leak. The heap is the whole kernel's, so
 * the probe writes nothing between the first reading and the last, and each
 * kind has three tries: the process that reads the probe's output still
 * wakes on its timer, and can hold a block of the heap at one reading and
 * not at the other, while a leak of each object shows in every try.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, guest.pools.live or guest.pools.heap without it, and beside them
 * "POOLPROBE OK" with exit status 0, or "POOLPROBE FAIL: " and the reason
 * with exit status 1. Any other mode is refused with status 2.
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

#define AF_INET    2
#define SOCK_DGRAM 2
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

/** Whether the strings `a` and `b` are equal; NULL equals nothing. */
static int same(const char *a, const char *b)
{
    if (a == 0 || b == 0) {
        return 0;
    }
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

/** Report the planned assertion that failed with `why`, print
 * "POOLPROBE FAIL: " and `why`, and exit 1. */
static void fail(const char *why)
{
    report_stop(why);
    syscall3(SYS_WRITE, 1, (long)"POOLPROBE FAIL: ", 16);
    puts(why);
    sys_exit(1);
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
    /* A holder that dies unheard is then end of input on the reads below. */
    syscall1(SYS_CLOSE, report[1]);
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
    report_held();
}

/** Make and close `count` objects of `kind` (0 a pipe, 1 a pty master, 2 a
 * socket); 0, or -1 when one could not be made. */
static int make_close(int kind, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        if (kind == 0) {
            int pair[2];

            pair[0] = pair[1] = -1;
            if (syscall1(SYS_PIPE, (long)pair) != 0) {
                return -1;
            }
            syscall1(SYS_CLOSE, pair[0]);
            syscall1(SYS_CLOSE, pair[1]);
        } else {
            int fd = kind == 1 ? syscall3(SYS_OPEN, (long)"/dev/ptmx", O_RDWR, 0) : open_socket();

            if (fd < 0) {
                return -1;
            }
            syscall1(SYS_CLOSE, fd);
        }
    }
    return 0;
}

/* The last try of `cycle`: whether it made every object, and HeapUsed and
 * HeapPages before and after it. */
static int made_all;
static unsigned used_before, used_after, pages_before, pages_after;

/** Make and close a thousand objects of `kind`, up to three times; 1 when
 * after one of the tries HeapUsed is what it was before it and HeapPages is
 * within two pages, and 0 when none is or an object could not be made. */
static int cycle(int kind)
{
    int tries;

    for (tries = 0; tries < 3; tries++) {
        used_before = probe_meminfo("HeapUsed:");
        pages_before = probe_meminfo("HeapPages:");
        made_all = make_close(kind, 1000) == 0;
        if (!made_all) {
            return 0;
        }
        used_after = probe_meminfo("HeapUsed:");
        pages_after = probe_meminfo("HeapPages:");
        if (used_after == used_before && pages_after + 2 >= pages_before &&
            pages_after <= pages_before + 2) {
            return 1;
        }
    }
    return 0;
}

/** The reason a cycle of `what` failed, with the readings of its last try,
 * in a buffer that the next call reuses. */
static const char *changed(const char *what)
{
    static char text[ETUDE_REPORT_LINE_MAX];
    char number[12];
    int n = 0;

    report_put(text, &n, "a thousand ");
    report_put(text, &n, what);
    if (!made_all) {
        report_put(text, &n, " could not all be made");
        text[n] = '\0';
        return text;
    }
    report_put(text, &n,
               " made and closed changed the heap in three tries; the last moved HeapUsed from ");
    report_put(text, &n, report_decimal(number, (int)used_before));
    report_put(text, &n, " to ");
    report_put(text, &n, report_decimal(number, (int)used_after));
    report_put(text, &n, " and HeapPages from ");
    report_put(text, &n, report_decimal(number, (int)pages_before));
    report_put(text, &n, " to ");
    report_put(text, &n, report_decimal(number, (int)pages_after));
    text[n] = '\0';
    return text;
}

/** The four rounds, each reported as it ends. */
static void live(void)
{
    report_plan("pipes");
    report_plan("ptys");
    report_plan("sockets");
    report_plan("processes");
    crossed(0, 6, "forty-eight pipes at once");
    crossed(1, 5, "forty ptys at once");
    crossed(2, 6, "forty-eight sockets at once");
    crossed(3, 4, "forty processes at once");
}

/** The three cycles, measured before anything is reported. */
static void heap(const char *case_id)
{
    static const char *const kinds[] = {"pipes", "ptys", "sockets"};
    int meminfo = probe_meminfo("HeapUsed:") != 0xFFFFFFFFu;
    int warm = meminfo && make_close(0, 1) == 0 && make_close(1, 1) == 0 && make_close(2, 1) == 0;
    int held = 0, kind;

    while (warm && held < 3 && cycle(held)) {
        held++;
    }
    report_begin(case_id);
    report_plan("meminfo");
    report_plan("pipe-heap");
    report_plan("pty-heap");
    report_plan("socket-heap");
    if (!meminfo) {
        fail("HeapUsed in /proc/meminfo");
    }
    report_held();
    if (!warm) {
        fail("a pipe, a pty or a socket made before the first reading");
    }
    for (kind = 0; kind < held; kind++) {
        report_held();
    }
    if (held < 3) {
        fail(changed(kinds[held]));
    }
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe, entered with `sp` at argc: the mode its argument names. */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    char **envp = argv + argc + 1;
    const char *mode;

    probe_args(argc, argv);
    mode = probe_word(1);
    if (same(mode, "live")) {
        report_begin(report_case(envp, "guest.pools.live"));
        live();
    } else if (same(mode, "heap")) {
        heap(report_case(envp, "guest.pools.heap"));
    } else {
        puts("POOLPROBE FAIL: usage: poolprobe live | heap");
        sys_exit(2);
    }
    report_end();
    puts("POOLPROBE OK");
    sys_exit(0);
}
