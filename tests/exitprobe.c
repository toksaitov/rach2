/**
 * exitprobe: reap children that end by exit, by a signal, and by exit after
 * an exec that failed once the program's header had been accepted.
 *
 * Each victim sees its parent asleep in waitpid before it dies. The parent
 * checks the encoded status, the end of input on a pipe the victim held, and
 * that the process record is gone. A separate supervisor reads the parent's
 * report, so that a reaper that ends without one gives a named failure; a
 * lost zombie leaves the reaper waiting, and the case's watchdog ends it. The
 * rows run on the kernel's own process records, scheduler and reaping, with
 * nothing put in their place.
 *
 * `exitprobe [normal|signal|exec]` runs one row, or all three. Prints
 * "exitprobe: waiting: ROW" when the victim sees its parent waiting, then
 * "EXITPROBE OK ROW: ..." or "EXITPROBE FAIL ROW: ...", and exits 0 when
 * every row passed.
 *
 * The fixtures, the directory `exit-cwd`, the truncated program `exit-bad`
 * and the victim's `exit-file`, are made in the working directory, which is
 * `/` on the test image and the case's own directory in a native run, and
 * removed before the probe exits. The row is the first argument without `=`
 * (probe_args.h).
 *
 * etude's report (probe_report.h), for the case ETUDE_CASE names, has the
 * assertions `fixture`, the rows that run, `normal`, `signal` and `exec`,
 * and `cleanup`, the removal of the fixtures. A row that fails leaves the
 * rows after it not run, and `cleanup` is reported on every path after the
 * report begins. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

#define SIGKILL 9

/* The name of the row running, for the result lines, and the assertion of
 * each row. */
static const char *row;
static const char *const row_ids[3] = {"normal", "signal", "exec"};

/* Why the last row that failed did. */
static const char *row_why;
static int fixture_dir;
static int fixture_file;

/** Make the system call `n` with three arguments. */
static int call(int n, long a, long b, long c)
{
    return syscall3(n, a, b, c);
}

/** Print `kind`, the row and `detail` as one line. */
static void result(const char *kind, const char *detail)
{
    probe_line(kind, row, detail);
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
 * reap after it. Exits 95 once `parent` is no longer the caller's parent, as
 * when the supervisor has killed it at the bound.
 */
static void wait_parent(int parent)
{
    char path[40], buf[256];
    int fd, n, i;
    long nap[2] = {0, 1000000};
    proc_path(path, parent);
    for (;;) {
        if (call(SYS_GETPPID, 0, 0, 0) != parent) {
            die(95);
        }
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
    char *args[2] = {"../exit-bad", 0};
    int fd;
    call(SYS_CLOSE, report, 0, 0);
    call(SYS_CLOSE, reader, 0, 0);
    fd = call(SYS_OPEN, (long)"exit-file", 0102, 0600);
    if (fd < 0 || call(SYS_WRITE, fd, (long)"held", 4) != 4 ||
        call(SYS_UNLINK, (long)"exit-file", 0, 0) != 0 ||
        call(SYS_CHDIR, (long)"exit-cwd", 0, 0) != 0) {
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

/** Kill the reaper `pid` and reap it, so that a failed row leaves no reaper
 * blocked in its wait; its victim then exits by itself in wait_parent, or has
 * already ended. */
static void end_reaper(int pid)
{
    int status;
    call(SYS_KILL, pid, SIGKILL, 0);
    call(SYS_WAITPID, pid, (long)&status, 0);
}

/** Run row `mode` in a reaper child and read its report from outside, so that
 * the reaper's own blocking wait and signal state are untouched. Prints the
 * row's result line and returns 1 when it passed; a row that failed by its
 * report, or whose reaper ended without one, has its reaper killed and
 * reaped after the line. */
static int run_row(int mode)
{
    int report[2], pid, status = -1, ok;
    char byte = 0;
    row = mode == 0 ? "normal" : (mode == 1 ? "signal" : "exec");
    if (call(SYS_PIPE, (long)report, 0, 0) != 0) {
        row_why = "the report pipe could not be made";
        return 0;
    }
    pid = call(SYS_FORK, 0, 0, 0);
    if (pid == 0) {
        call(SYS_CLOSE, report[0], 0, 0);
        reaper(mode, report[1]);
    }
    call(SYS_CLOSE, report[1], 0, 0);
    ok = pid > 0 && call(SYS_READ, report[0], (long)&byte, 1) == 1 && byte == 1;
    call(SYS_CLOSE, report[0], 0, 0);
    if (!ok) {
        result("EXITPROBE FAIL ", ": reap or release");
        row_why = "the reaper did not report that the reap held";
        if (pid > 0) {
            end_reaper(pid);
        }
        return 0;
    }
    ok = call(SYS_WAITPID, pid, (long)&status, 0) == pid && status == 0;
    result(ok ? "EXITPROBE OK " : "EXITPROBE FAIL ", ": status, eof, echild, proc absent");
    row_why = "the reaper did not exit 0";
    return ok;
}

/**
 * Make `exit-cwd` and write `exit-bad`, which the victim executes from
 * `exit-cwd` as `../exit-bad`: a complete ELF header of the architecture's
 * class with no program header bytes after it, ELF32 i386 with entry
 * 0x08048000 or ELF64 x86-64 with entry 0x400000, each naming one program
 * header at the header's end. Returns 1 when the file was written.
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
    if (call(SYS_MKDIR, (long)"exit-cwd", 0700, 0) != 0) {
        return 0;
    }
    fixture_dir = 1;
    fd = call(SYS_OPEN, (long)"exit-bad", 01301, 0700);
    fixture_file = fd >= 0;
    ok = fd >= 0 && call(SYS_WRITE, fd, (long)header, sizeof(header)) == sizeof(header);
    call(SYS_CLOSE, fd, 0, 0);
    return ok;
}

/** Remove the supervisor's fixtures after the rows have ended, report
 * `cleanup` and end the report. */
static void finish(int status)
{
    int clean = 1;

    call(SYS_UNLINK, (long)"exit-file", 0, 0);
    if (fixture_file && call(SYS_UNLINK, (long)"exit-bad", 0, 0) != 0) {
        clean = 0;
    }
    if (fixture_dir && call(SYS_RMDIR, (long)"exit-cwd", 0, 0) != 0) {
        clean = 0;
    }
    if (clean) {
        report_pass("cleanup");
    } else {
        report_fail("cleanup", "exit-bad or exit-cwd could not be removed");
        status = 1;
    }
    report_end();
    die(status);
}

/** Report the rows `from` to `last` not run, for `why`. */
static void rows_not_run(int from, int last, const char *why)
{
    int i;

    for (i = from; i <= last; i++) {
        report_notrun(row_ids[i], why);
    }
}

/** The probe's entry, given the initial stack `sp`: run all rows or the one
 * the first argument names. */
void probe_main(unsigned long *sp)
{
    int first = 0, last = 2, i;
    const char *mode;
    probe_args((int)sp[0], (char **)(sp + 1));
    mode = probe_word(1);
    if (mode != 0) {
        first = mode[0] == 'n' ? 0 : (mode[0] == 's' ? 1 : 2);
        last = first;
    }
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.exitprobe"));
    row = "fixture";
    if (!fixture()) {
        result("EXITPROBE FAIL ", ": create");
        report_fail("fixture", "exit-cwd or exit-bad could not be made");
        rows_not_run(first, last, "the fixtures were not made");
        finish(1);
    }
    report_pass("fixture");
    for (i = first; i <= last; i++) {
        if (!run_row(i)) {
            report_fail(row_ids[i], row_why);
            rows_not_run(i + 1, last, "an earlier row failed");
            finish(1);
        }
        report_pass(row_ids[i]);
    }
    finish(0);
}

/** Pass the initial stack before a C prologue changes the stack pointer. */
PROBE_SP_ENTRY;
