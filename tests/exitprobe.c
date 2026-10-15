/**
 * exitprobe: reap children that end by exit, by a signal, and by exit after
 * an exec that failed once the program's header had been accepted.
 *
 * Each victim sees its parent asleep in waitpid before it dies. The parent
 * checks the encoded status, the end of input on a pipe the victim held, and
 * that the process record is gone. A separate supervisor bounds the whole
 * with poll, so that a lost zombie gives a named failure. The rows run on the
 * kernel's own process records, scheduler and reaping, with nothing put in
 * their place.
 *
 * `exitprobe [normal|signal|exec [MS]]` runs one row, or all three, with a
 * bound of MS milliseconds, 5000 by default. Prints "exitprobe: waiting:
 * ROW" when the victim sees its parent waiting, then "EXITPROBE OK ROW: ..."
 * or "EXITPROBE FAIL ROW: ...", and exits 0 when every row passed.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

/* The name of the row running, for the result lines. */
static const char *row;

/** Make the system call `n` with three arguments. */
static int call(int n, long a, long b, long c)
{
    return syscall3(n, a, b, c);
}

/** Write the string `s` to standard output, the serial console. */
static void put(const char *s)
{
    int n = 0;
    while (s[n]) {
        n++;
    }
    call(SYS_WRITE, 1, (long)s, n);
}

/** Print `kind`, the row and `detail` as one line. */
static void result(const char *kind, const char *detail)
{
    put(kind);
    put(row);
    put(detail);
    put("\n");
}

/** Exit without returning to the caller. */
static void die(int status)
{
    call(SYS_EXIT, status, 0, 0);
    for (;;) {
    }
}

/** Write "/proc/`pid`/stat" into `path`. */
static void proc_path(char *path, int pid)
{
    const char *prefix = "/proc/";
    const char *suffix = "/stat";
    char digits[12];
    int n = 0;
    while (*prefix) {
        *path++ = *prefix++;
    }
    do {
        digits[n++] = (char)('0' + pid % 10);
        pid /= 10;
    } while (pid);
    while (n) {
        *path++ = digits[--n];
    }
    while (*suffix) {
        *path++ = *suffix++;
    }
    *path = 0;
}

/**
 * Wait, a millisecond at a time, until /proc shows the process `parent`
 * asleep, which after its fork can only be in waitpid, and print
 * "exitprobe: waiting: ROW". The supervisor's poll bounds this wait and the
 * reap after it.
 */
static void wait_parent(int parent)
{
    char path[40], buf[256];
    int fd, n, i;
    long nap[2] = {0, 1000000};
    proc_path(path, parent);
    for (;;) {
        fd = call(SYS_OPEN, (long)path, 0, 0);
        n = call(SYS_READ, fd, (long)buf, sizeof(buf));
        call(SYS_CLOSE, fd, 0, 0);
        for (i = 0; i + 3 < n; i++) {
            if (buf[i] == ')' && buf[i + 1] == ' ' && buf[i + 2] == 'S' && buf[i + 3] == ' ') {
                result("exitprobe: waiting: ", "");
                return;
            }
        }
        call(SYS_NANOSLEEP, (long)nap, 0, 0);
    }
}

/**
 * The victim of row `mode`: hold the write end of the reaper's pipe, an
 * unlinked file and a working directory, wait for the reaper `parent` to
 * wait, and end: by exit 37, by SIGTERM, or by exit 38 after an exec of the
 * truncated ELF. That file passes the header check but lacks the program
 * header it names, so execve fails while it loads the image, which it builds
 * before it gives up the old one, and returns ENOEXEC. Exits 90 when the
 * setup fails and 91 when the row's end did not happen. `report` and
 * `reader` are the descriptors the victim closes.
 */
static void victim(int mode, int parent, int report, int reader)
{
    char *args[2] = {"/exit-bad", 0};
    int fd;
    call(SYS_CLOSE, report, 0, 0);
    call(SYS_CLOSE, reader, 0, 0);
    fd = call(SYS_OPEN, (long)"/exit-file", 0102, 0600);
    if (fd < 0 || call(SYS_WRITE, fd, (long)"held", 4) != 4 ||
        call(SYS_UNLINK, (long)"/exit-file", 0, 0) != 0 ||
        call(SYS_CHDIR, (long)"/exit-cwd", 0, 0) != 0) {
        die(90);
    }
    wait_parent(parent);
    if (mode == 0) {
        die(37);
    } else if (mode == 1) {
        call(SYS_KILL, call(SYS_GETPID, 0, 0, 0), 15, 0);
    } else if (call(SYS_EXECVE, (long)args[0], (long)args, 0) == -8) {
        die(38);
    }
    die(91);
}

/**
 * The reaper of row `mode`: fork the victim, wait for it, and check its wait
 * status, the end of input on the pipe it held, ECHILD for a second wait and
 * no /proc record. Writes 1 or 0 to `report` and exits 0, or 93 on a
 * failure, or 92 when the pipe cannot be made.
 */
static void reaper(int mode, int report)
{
    int fds[2], parent, pid, status = -1, ok, fd;
    char byte = 0, path[40];
    int expected = mode == 0 ? 37 << 8 : (mode == 1 ? 15 : 38 << 8);
    if (call(SYS_PIPE, (long)fds, 0, 0) != 0 || call(SYS_FCNTL, fds[0], 4, 04000) != 0) {
        die(92);
    }
    parent = call(SYS_GETPID, 0, 0, 0);
    pid = call(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        victim(mode, parent, report, fds[0]);
    }
    call(SYS_CLOSE, fds[1], 0, 0);
    ok = pid > 0 && call(SYS_WAITPID, pid, (long)&status, 0) == pid && status == expected;
    ok = ok && call(SYS_READ, fds[0], (long)&byte, 1) == 0;
    ok = ok && call(SYS_WAITPID, pid, (long)&status, 1) == -10;
    proc_path(path, pid);
    fd = call(SYS_OPEN, (long)path, 0, 0);
    ok = ok && fd == -2;
    if (fd >= 0) {
        call(SYS_CLOSE, fd, 0, 0);
    }
    byte = (char)ok;
    call(SYS_WRITE, report, (long)&byte, 1);
    die(ok ? 0 : 93);
}

/** Run row `mode` in a reaper child and bound it, from outside, with a poll
 * of `timeout` milliseconds on the reaper's report, so that the reaper's own
 * blocking wait and signal state are untouched. Prints the row's result line
 * and returns 1 when it passed. */
static int run_row(int mode, int timeout)
{
    struct {
        int fd;
        short events, revents;
    } pollfd;
    int report[2], pid, status = -1, ok;
    char byte = 0;
    row = mode == 0 ? "normal" : (mode == 1 ? "signal" : "exec");
    if (call(SYS_PIPE, (long)report, 0, 0) != 0) {
        return 0;
    }
    pid = call(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        call(SYS_CLOSE, report[0], 0, 0);
        reaper(mode, report[1]);
    }
    call(SYS_CLOSE, report[1], 0, 0);
    pollfd.fd = report[0];
    pollfd.events = 1;
    pollfd.revents = 0;
    ok = pid > 0 && call(SYS_POLL, (long)&pollfd, 1, timeout) == 1;
    ok = ok && call(SYS_READ, report[0], (long)&byte, 1) == 1 && byte == 1;
    call(SYS_CLOSE, report[0], 0, 0);
    if (!ok) {
        result("EXITPROBE FAIL ", ": reap or release");
        return 0;
    }
    ok = call(SYS_WAITPID, pid, (long)&status, 0) == pid && status == 0;
    result(ok ? "EXITPROBE OK " : "EXITPROBE FAIL ", ": status, eof, echild, proc absent");
    return ok;
}

/**
 * Make /exit-cwd and write /exit-bad: a complete ELF header of the
 * architecture's class with no program header bytes after it, ELF32 i386
 * with entry 0x08048000 or ELF64 x86-64 with entry 0x400000, each naming one
 * program header at the header's end. Returns 1 when the file was written.
 */
static int fixture(void)
{
#if defined __x86_64__
    static const unsigned char header[64] = {
        0x7f, 'E', 'L', 'F', 2,    1, 1, 0, 0,  0, 0,  0, 0, 0, 0, 0, 2, 0, 62, 0, 1, 0,
        0,    0,   0,   0,   0x40, 0, 0, 0, 0,  0, 64, 0, 0, 0, 0, 0, 0, 0, 0,  0, 0, 0,
        0,    0,   0,   0,   0,    0, 0, 0, 64, 0, 56, 0, 1, 0, 0, 0, 0, 0, 0,  0};
#else
    static const unsigned char header[52] = {
        0x7f, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0,  0, 2,  0, 3, 0, 1, 0, 0, 0, 0, 0x80,
        4,    8,   52,  0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 52, 0, 32, 0, 1, 0, 0, 0, 0, 0, 0, 0};
#endif
    int fd, ok;
    call(SYS_MKDIR, (long)"/exit-cwd", 0700, 0);
    fd = call(SYS_OPEN, (long)"/exit-bad", 01101, 0700);
    ok = fd >= 0 && call(SYS_WRITE, fd, (long)header, sizeof(header)) == sizeof(header);
    call(SYS_CLOSE, fd, 0, 0);
    return ok;
}

/** The probe's entry, given the initial stack `sp`: run all rows or the one
 * the first argument names, with the bound in milliseconds the second gives;
 * a bound that is not 1 to 320000 exits 94. */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0], first = 0, last = 2, timeout = 5000, i;
    char **argv = (char **)(sp + 1);
    if (argc > 1) {
        first = argv[1][0] == 'n' ? 0 : (argv[1][0] == 's' ? 1 : 2);
        last = first;
    }
    if (argc > 2) {
        timeout = 0;
        for (i = 0; argv[2][i] >= '0' && argv[2][i] <= '9'; i++) {
            if (timeout > 32000) {
                die(94);
            }
            timeout = timeout * 10 + argv[2][i] - '0';
        }
        if (timeout < 1 || timeout > 320000 || argv[2][i]) {
            die(94);
        }
    }
    row = "fixture";
    if (!fixture()) {
        result("EXITPROBE FAIL ", ": create");
        die(1);
    }
    for (i = first; i <= last; i++) {
        if (!run_row(i, timeout)) {
            die(1);
        }
    }
    die(0);
}

/** Pass the initial stack before a C prologue changes the stack pointer. */
PROBE_SP_ENTRY;
