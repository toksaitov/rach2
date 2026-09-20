/**
 * Reap children through all three production death paths.
 *
 * Each victim observes its parent blocked in waitpid before dying. The parent
 * checks the encoded status, pipe EOF and removal of the process record. A
 * separate supervisor bounds completion with poll, so a lost zombie produces
 * a named failure. No resource, scheduler or reaper is substituted.
 */
#define SYS_EXIT      1
#define SYS_FORK      2
#define SYS_READ      3
#define SYS_WRITE     4
#define SYS_OPEN      5
#define SYS_CLOSE     6
#define SYS_WAITPID   7
#define SYS_UNLINK    10
#define SYS_EXECVE    11
#define SYS_CHDIR     12
#define SYS_GETPID    20
#define SYS_KILL      37
#define SYS_MKDIR     39
#define SYS_PIPE      42
#define SYS_FCNTL     221
#define SYS_NANOSLEEP 162
#define SYS_POLL      168

static const char *row;

/** Make a direct i386 system call. */
static int call(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

/** Write one string to the serial console. */
static void put(const char *s)
{
    int n = 0;
    while (s[n]) {
        n++;
    }
    call(SYS_WRITE, 1, (int)s, n);
}

/** Print the row and its result as one complete line. */
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

/** Form the procfs path of one process. */
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
 * Wait for the parent to enter waitpid. It has no other blocking call after
 * fork. The supervisor's poll bounds this handshake and the subsequent reap.
 */
static void wait_parent(int parent)
{
    char path[40], buf[256];
    int fd, n, i;
    int nap[2] = {0, 1000000};
    proc_path(path, parent);
    for (;;) {
        fd = call(SYS_OPEN, (int)path, 0, 0);
        n = call(SYS_READ, fd, (int)buf, sizeof(buf));
        call(SYS_CLOSE, fd, 0, 0);
        for (i = 0; i + 3 < n; i++) {
            if (buf[i] == ')' && buf[i + 1] == ' ' && buf[i + 2] == 'S' && buf[i + 3] == ' ') {
                result("exitprobe: waiting: ", "");
                return;
            }
        }
        call(SYS_NANOSLEEP, (int)nap, 0, 0);
    }
}

/**
 * Keep a pipe writer, an unlinked file and a working directory until death.
 * The truncated ELF passes the initial header check but fails its program
 * header read after execve has destroyed the old image.
 */
static void victim(int mode, int parent, int report, int reader)
{
    char *args[2] = {"/exit-bad", 0};
    int fd;
    call(SYS_CLOSE, report, 0, 0);
    call(SYS_CLOSE, reader, 0, 0);
    fd = call(SYS_OPEN, (int)"/exit-file", 0102, 0600);
    if (fd < 0 || call(SYS_WRITE, fd, (int)"held", 4) != 4 ||
        call(SYS_UNLINK, (int)"/exit-file", 0, 0) != 0 ||
        call(SYS_CHDIR, (int)"/exit-cwd", 0, 0) != 0) {
        die(90);
    }
    wait_parent(parent);
    if (mode == 0) {
        die(37);
    } else if (mode == 1) {
        call(SYS_KILL, call(SYS_GETPID, 0, 0, 0), 15, 0);
    } else {
        call(SYS_EXECVE, (int)args[0], (int)args, 0);
    }
    die(91);
}

/** Reap the victim and check released descriptors and the removed record. */
static void reaper(int mode, int report)
{
    int fds[2], parent, pid, status = -1, ok, fd;
    char byte = 0, path[40];
    int expected = mode == 0 ? 37 << 8 : (mode == 1 ? 15 : 9);
    if (call(SYS_PIPE, (int)fds, 0, 0) != 0 || call(SYS_FCNTL, fds[0], 4, 04000) != 0) {
        die(92);
    }
    parent = call(SYS_GETPID, 0, 0, 0);
    pid = call(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        victim(mode, parent, report, fds[0]);
    }
    call(SYS_CLOSE, fds[1], 0, 0);
    ok = pid > 0 && call(SYS_WAITPID, pid, (int)&status, 0) == pid && status == expected;
    ok = ok && call(SYS_READ, fds[0], (int)&byte, 1) == 0;
    ok = ok && call(SYS_WAITPID, pid, (int)&status, 1) == -10;
    proc_path(path, pid);
    fd = call(SYS_OPEN, (int)path, 0, 0);
    ok = ok && fd == -2;
    if (fd >= 0) {
        call(SYS_CLOSE, fd, 0, 0);
    }
    byte = (char)ok;
    call(SYS_WRITE, report, (int)&byte, 1);
    die(ok ? 0 : 93);
}

/** Bound the real parent's blocking wait without changing its signal state. */
static int run_row(int mode, int timeout)
{
    struct {
        int fd;
        short events, revents;
    } pollfd;
    int report[2], pid, status = -1, ok;
    char byte = 0;
    row = mode == 0 ? "normal" : (mode == 1 ? "signal" : "exec");
    if (call(SYS_PIPE, (int)report, 0, 0) != 0) {
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
    ok = pid > 0 && call(SYS_POLL, (int)&pollfd, 1, timeout) == 1;
    ok = ok && call(SYS_READ, report[0], (int)&byte, 1) == 1 && byte == 1;
    call(SYS_CLOSE, report[0], 0, 0);
    if (!ok) {
        result("EXITPROBE FAIL ", ": reap or release");
        return 0;
    }
    ok = call(SYS_WAITPID, pid, (int)&status, 0) == pid && status == 0;
    result(ok ? "EXITPROBE OK " : "EXITPROBE FAIL ", ": status, eof, echild, proc absent");
    return ok;
}

/** Write a complete ELF32 header with no program header bytes after it. */
static int fixture(void)
{
    static const unsigned char header[52] = {
        0x7f, 'E', 'L', 'F', 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0,  0, 2,  0, 3, 0, 1, 0, 0, 0, 0, 0x80,
        4,    8,   52,  0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 52, 0, 32, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    int fd, ok;
    call(SYS_MKDIR, (int)"/exit-cwd", 0700, 0);
    fd = call(SYS_OPEN, (int)"/exit-bad", 01101, 0700);
    ok = fd >= 0 && call(SYS_WRITE, fd, (int)header, sizeof(header)) == sizeof(header);
    call(SYS_CLOSE, fd, 0, 0);
    return ok;
}

/** Run all rows or one selected row, with a bound in milliseconds. */
void probe_main(unsigned int *sp)
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

/** Pass the initial i386 stack before a C prologue changes ESP. */
__asm__(".global _start\n"
        "_start:\n"
        "    pushl %esp\n"
        "    call probe_main\n");
