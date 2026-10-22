/**
 * conloadprobe: the console's receive path under load, where every typed
 * line arrives.
 *
 * The harness types numbered lines, `L NNNN payload SUM`, where SUM is the
 * Adler-32 of the text before it in eight hex digits, while five children
 * spin and one writes and syncs a 256 KiB file on the mounted volume over
 * and over. The probe reads the lines in four stages and requires each one
 * in order with its sum:
 *
 *   (A) 200 lines typed back to back while the reader waits in poll, at
 *       least one of whose bytes the port's interrupt must have moved, which
 *       `irq-bytes` counts. The port is drained into the console's ring by
 *       its interrupt and by the tick alike, and the reader takes from the
 *       ring, so how many bytes the interrupt moves depends on the machine,
 *       a fast reader leaving it few, and a required share would fail on
 *       some machine; a drain by the tick alone moves none. The stage's time
 *       is printed and not judged.
 *   (B) 60 lines typed while the probe does not read, until /proc/console's
 *       `uart-bytes` has counted every byte of them taken from the port:
 *       type-ahead into a session nobody is reading.
 *   (C) 200 lines typed while a child in its own group has the terminal and
 *       nobody reads, more than the discipline holds, then an interrupt
 *       character behind them, which must still kill the child and discards
 *       what came before it; then 60 lines read by the parent after it takes
 *       the terminal back.
 *   (D) 40 lines typed before the probe executes itself, read by the new
 *       image. The exec waits until `uart-bytes` has counted every byte
 *       of them, as stage B's reading does.
 *
 * How long the harness takes to type a stage decides nothing: the probe
 * waits for the counted bytes, and the suite's wait for its token ends a
 * run in which they never come. An overrun the port reports ends the wait
 * as well, since the bytes it lost cannot be counted, and the stage's reading
 * then finds the lines that are missing.
 *
 * Each stage is announced with `CONLOAD X` and closed with `CONLOAD X OK`;
 * the harness types after the announcement. After each loaded stage the
 * probe prints how many files the writer completed in it. The end prints
 * the `/proc/console` counters and `CONLOAD OK lines=N`, or
 * `CONLOAD FAIL: reason`.
 */

#include "probe.h"

#define O_RDONLY  0
#define O_WRONLY  1
#define O_CREAT   0100
#define O_TRUNC   01000
#define SIGINT    2
#define SIGKILL   9
#define TIOCSPGRP 0x5410
#define POLLIN    0x001

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define LINES_A    200
#define LINES_B    60
#define LINES_C    60
#define LINES_D    40
#define LINES_ALL  (LINES_A + LINES_B + LINES_C + LINES_D)
/* `L NNNN `, 20 bytes of payload, a space, eight hex digits and a newline. */
#define LINE_BYTES 37

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

/** Write `s` to standard output. */
static void print(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
}

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    print(s);
    print("\n");
}

/** Write `v` in decimal to standard output. */
static void print_num(unsigned v)
{
    char tmp[12];
    int n = 0;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        syscall3(SYS_WRITE, 1, (long)&tmp[--n], 1);
    }
}

static int pids[6];
static int npids;
static int progress[2]; /* the writer reports each file written, one byte */
static int load_died;   /* a load process that ended on its own */

/** Kill and reap the load processes, and set `load_died` for any that a
 * signal other than SIGKILL, or its own exit, ended. */
static void stop_load(void)
{
    int i;

    for (i = 0; i < npids; i++) {
        syscall2(SYS_KILL, pids[i], SIGKILL);
    }
    for (i = 0; i < npids; i++) {
        int status = 0;
        syscall3(SYS_WAITPID, pids[i], (long)&status, 0);
        if ((status & 0x7f) != SIGKILL) {
            load_died = 1;
        }
    }
    npids = 0;
}

/** Kill the child `pid` and reap it. */
static void end_child(int pid)
{
    int status;

    syscall2(SYS_KILL, pid, SIGKILL);
    syscall3(SYS_WAITPID, pid, (long)&status, 0);
}

/** Stop the load, print "CONLOAD FAIL: " and `why`, and exit 1. */
static void fail(const char *why)
{
    stop_load();
    print("CONLOAD FAIL: ");
    puts(why);
    sys_exit(1);
}

/** The time of day in milliseconds, modulo 2^32. */
static unsigned now_ms(void)
{
    long tv[2];

    tv[0] = tv[1] = 0;
    syscall2(SYS_GETTIMEOFDAY, (long)tv, 0);
    return (unsigned)tv[0] * 1000u + (unsigned)tv[1] / 1000u;
}

static unsigned char block[4096];

/** The writer: write the file `path` whole, 64 blocks of 4 KiB, close it,
 * sync, and report it with a byte on `progress`, again and again. It exits 2
 * when the open fails and 3 when a write is short. */
static void writer(const char *path)
{
    unsigned i;

    for (i = 0; i < sizeof(block); i++) {
        block[i] = (unsigned char)i;
    }
    for (;;) {
        int fd = syscall3(SYS_OPEN, (long)path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if (fd < 0) {
            sys_exit(2);
        }
        for (i = 0; i < 64; i++) {
            if (syscall3(SYS_WRITE, fd, (long)block, (int)sizeof(block)) != (int)sizeof(block)) {
                sys_exit(3);
            }
        }
        syscall1(SYS_CLOSE, fd);
        syscall1(SYS_SYNC, 0);
        syscall3(SYS_WRITE, progress[1], (long)"w", 1);
    }
}

/**
 * Print how many files the writer completed since the preceding loaded
 * stage.
 *
 * The count is taken when the stage's console work ends and is printed, not
 * judged. How many 256 KiB files a writer finishes inside a stage depends on
 * the stage's length and the machine's speed, and a short stage can finish
 * none. The scheduler's bound for a woken writer is the number of dispatches
 * it waits, which `tests/host/sched_hosttest.c` checks where no other load
 * can change it.
 */
static void writer_progressed(const char *stage)
{
    struct pollfd pfd;
    char b;
    int n = 0;

    pfd.fd = progress[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    while (syscall3(SYS_POLL, (long)&pfd, 1, 0) > 0 &&
           syscall3(SYS_READ, progress[0], (long)&b, 1) == 1) {
        n++;
    }
    print("CONLOAD writer files in stage ");
    print(stage);
    print(": ");
    print_num(n);
    print("\n");
}

/** Start the load: five spinners and the writer, with the progress pipe. */
static void start_load(void)
{
    int i;

    if (syscall1(SYS_PIPE, (long)progress) != 0) {
        fail("the progress pipe");
    }
    for (i = 0; i < 6; i++) {
        pids[i] = syscall1(SYS_FORK, 0);
        if (pids[i] < 0) {
            fail("fork of a load process");
        }
        if (pids[i] == 0) {
            if (i < 5) {
                for (;;) {
                }
            }
            writer("/lo/conload.bin");
        }
        npids = i + 1;
    }
}

/** The Adler-32 checksum of the `n` bytes at `s`. */
static unsigned adler32(const char *s, int n)
{
    unsigned a = 1, b = 0;
    int i;

    for (i = 0; i < n; i++) {
        a = (a + (unsigned char)s[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

/** The value of the hex digit `c`, or -1. */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static char line[512];
static int line_len;
static int bad_seq;
static int bad_sum;
/* The number of the first bad line, or -1 while none has been bad. */
static int first_bad = -1;

/**
 * Read one line from the console into `line`, without its newline: 1, or 0
 * at end of file. A line longer than `line` is cut short.
 *
 * Before each byte the reader waits in poll, with no timeout, so that the
 * port's interrupt has a waiter on the console to wake. A byte that never
 * comes leaves the reader waiting, and the case's watchdog ends the run.
 */
static int read_line(void)
{
    line_len = 0;
    for (;;) {
        char c;
        int r;
        struct pollfd pfd;

        pfd.fd = 0;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (syscall3(SYS_POLL, (long)&pfd, 1, -1) <= 0) {
            line[line_len] = 0;
            print("CONLOAD FAIL: the poll of the console failed; so far: ");
            puts(line);
            stop_load();
            sys_exit(1);
        }
        r = syscall3(SYS_READ, 0, (long)&c, 1);
        if (r <= 0) {
            return 0;
        }
        if (c == '\n') {
            line[line_len] = 0;
            return 1;
        }
        if (line_len < (int)sizeof(line) - 1) {
            line[line_len++] = c;
        }
    }
}

/** Non-zero when `line` is line `expected` with a right sum; a wrong number
 * counts in `bad_seq`, a wrong sum in `bad_sum`, and a line of another form
 * in neither. */
static int check_line(int expected)
{
    int seq = 0, i, sp = -1;
    unsigned sum = 0;

    if (line_len < 15 || line[0] != 'L' || line[1] != ' ') {
        return 0;
    }
    for (i = 2; i < 6; i++) {
        if (line[i] < '0' || line[i] > '9') {
            return 0;
        }
        seq = seq * 10 + (line[i] - '0');
    }
    for (i = line_len - 1; i > 0; i--) {
        if (line[i] == ' ') {
            sp = i;
            break;
        }
    }
    if (sp < 0 || line_len - sp - 1 != 8) {
        return 0;
    }
    for (i = sp + 1; i < line_len; i++) {
        int h = hexval(line[i]);
        if (h < 0) {
            return 0;
        }
        sum = (sum << 4) | (unsigned)h;
    }
    if (seq != expected) {
        bad_seq++;
        return 0;
    }
    if (sum != adler32(line, sp)) {
        bad_sum++;
        return 0;
    }
    return 1;
}

/** Read `count` lines numbered from `first`; the number of good ones. The
 * first bad line's number goes to `first_bad`. */
static int read_lines(int first, int count)
{
    int good = 0, n;

    for (n = 0; n < count; n++) {
        if (!read_line()) {
            fail("end of file on the console");
        }
        if (check_line(first + n)) {
            good++;
        } else if (first_bad < 0) {
            first_bad = first + n;
        }
    }
    return good;
}

/** Print the counters of /proc/console, or that there are none. */
static void print_counters(void)
{
    static char text[2048];
    int fd = syscall3(SYS_OPEN, (long)"/proc/console", O_RDONLY, 0);
    int n;

    if (fd < 0) {
        puts("CONLOAD counters: none");
        return;
    }
    n = syscall3(SYS_READ, fd, (long)text, (int)sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n > 0) {
        text[n] = 0;
        print("CONLOAD counters:\n");
        print(text);
    }
}

/** The value of the /proc/console counter `key`, given with the space that
 * ends its name, or 0xFFFFFFFF when the file or the counter is absent. */
static unsigned counter(const char *key)
{
    static char text[2048];
    int fd = syscall3(SYS_OPEN, (long)"/proc/console", O_RDONLY, 0);
    int n, i;

    if (fd < 0) {
        return 0xFFFFFFFFu;
    }
    n = syscall3(SYS_READ, fd, (long)text, (int)sizeof(text) - 1);
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
            while (text[i] >= '0' && text[i] <= '9') {
                v = v * 10 + (unsigned)(text[i] - '0');
                i++;
            }
            return v;
        }
    }
    return 0xFFFFFFFFu;
}

/**
 * Wait, without reading the console, until `uart-bytes` has counted `lines`
 * whole lines taken from the port since it read `base`, yielding between
 * looks of /proc/console. A rise of `uart-overruns` above `overruns` ends the
 * wait too, since a byte the port dropped is never counted; the reading that
 * follows then fails on the lines that are short. Exits 1 when the counters
 * cannot be read.
 */
static void await_typed(unsigned base, unsigned overruns, int lines)
{
    unsigned want = (unsigned)lines * LINE_BYTES, now;

    if (base == 0xFFFFFFFFu || overruns == 0xFFFFFFFFu) {
        fail("no uart-bytes and uart-overruns in /proc/console");
    }
    for (;;) {
        now = counter("uart-bytes ");
        if (now == 0xFFFFFFFFu) {
            fail("no uart-bytes in /proc/console");
        }
        if (now - base >= want || counter("uart-overruns ") != overruns) {
            return;
        }
        syscall0(SYS_SCHED_YIELD);
    }
}

/** Return when `good` is `count`; otherwise print the stage's failure with
 * the counts and the counters, stop the load and exit 1. */
static void require(int good, int count, const char *stage)
{
    if (good == count) {
        return;
    }
    print("CONLOAD FAIL: stage ");
    print(stage);
    print(": ");
    print_num((unsigned)good);
    print(" of ");
    print_num((unsigned)count);
    print(" lines, first bad ");
    print_num((unsigned)first_bad);
    print(", wrong number ");
    print_num((unsigned)bad_seq);
    print(", wrong sum ");
    print_num((unsigned)bad_sum);
    print("\n");
    print_counters();
    stop_load();
    sys_exit(1);
}

/** Stage C's first half: fork a child that spins in a group of its own,
 * give it the terminal, print "CONLOAD C", and require that the interrupt
 * character the suite then types kills it; then take the terminal back. A
 * failure kills and reaps the child before it exits. */
static void interrupt_child(void)
{
    int child, status, pgid;

    child = syscall1(SYS_FORK, 0);
    if (child < 0) {
        fail("fork of the foreground child");
    }
    if (child == 0) {
        syscall2(SYS_SETPGID, 0, 0);
        for (;;) {
        }
    }
    syscall2(SYS_SETPGID, child, child);
    if (syscall3(SYS_IOCTL, 0, TIOCSPGRP, (long)&child) != 0) {
        end_child(child);
        fail("the terminal to the child's group");
    }
    puts("CONLOAD C");
    status = 0;
    if (syscall3(SYS_WAITPID, child, (long)&status, 0) != child) {
        end_child(child);
        fail("the wait for the foreground child");
    }
    if ((status & 0x7f) != SIGINT) {
        fail("the foreground child did not die of the interrupt character");
    }
    pgid = syscall1(SYS_GETPID, 0);
    if (syscall3(SYS_IOCTL, 0, TIOCSPGRP, (long)&pgid) != 0) {
        fail("the terminal back to the parent");
    }
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe, entered with `sp` at argc: stage D's half after the exec when
 * the first argument begins with 'D', and otherwise the stages in order. */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    int good;
    unsigned start, elapsed, irq_before, typed, overruns;

    if (argc >= 2 && argv[1][0] == 'D') {
        /* The image after the exec: the lines typed before it are read. */
        good = read_lines(LINES_A + LINES_B + LINES_C, LINES_D);
        require(good, LINES_D, "D");
        puts("CONLOAD D OK");
        print_counters();
        print("CONLOAD OK lines=");
        print_num(LINES_ALL);
        print("\n");
        sys_exit(0);
    }

    if (syscall2(SYS_MKDIR, (long)"/lo", 0755) != 0 ||
        syscall5(SYS_MOUNT, (long)"/dev/disk0p1", (long)"/lo", (long)"vfat", 0, 0) != 0) {
        fail("the mount of the volume");
    }
    start_load();

    /* A: the reader waits in poll while the lines arrive. */
    irq_before = counter("irq-bytes ");
    puts("CONLOAD A");
    if (!read_line()) {
        fail("end of file on the console");
    }
    start = now_ms();
    good = check_line(0) ? 1 : 0;
    if (!good) {
        first_bad = 0;
    }
    good += read_lines(1, LINES_A - 1);
    require(good, LINES_A, "A");
    elapsed = now_ms() - start;
    print_counters();
    /* irq-drains would also count a service that took no byte. */
    if (irq_before == 0xFFFFFFFFu || counter("irq-bytes ") == irq_before) {
        print_counters();
        fail("the port's interrupt served no bytes during stage A");
    }
    writer_progressed("A");
    print("CONLOAD A OK ms=");
    print_num(elapsed);
    print("\n");

    /* B: type-ahead into a session that is not reading. */
    typed = counter("uart-bytes ");
    overruns = counter("uart-overruns ");
    puts("CONLOAD B");
    await_typed(typed, overruns, LINES_B);
    good = read_lines(LINES_A, LINES_B);
    require(good, LINES_B, "B");
    writer_progressed("B");
    puts("CONLOAD B OK");

    /* C: an interrupt character to a foreground child, then the lines. */
    interrupt_child();
    good = read_lines(LINES_A + LINES_B, LINES_C);
    require(good, LINES_C, "C");
    writer_progressed("C");
    puts("CONLOAD C OK");

    /* D: the lines typed before an exec are read after it. */
    stop_load();
    if (load_died) {
        fail("a load process ended on its own");
    }
    typed = counter("uart-bytes ");
    overruns = counter("uart-overruns ");
    puts("CONLOAD D");
    await_typed(typed, overruns, LINES_D);
    {
        char *args[3];
        char *envp[1];

        args[0] = (char *)"conloadprobe";
        args[1] = (char *)"D";
        args[2] = 0;
        envp[0] = 0;
        syscall3(SYS_EXECVE, (long)"/conloadprobe", (long)args, (long)envp);
    }
    fail("the exec did not happen");
}
