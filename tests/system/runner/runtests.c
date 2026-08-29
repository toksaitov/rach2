/**
 * runtests: boot the test image once and run every probe, checking each
 * exits 0.
 *
 * Boots qemu-system-i386 headless with the kernel and the test RAM disk,
 * waits for the kernel shell, then `exec`s each probe in turn and scores
 * it from the line that hands the shell back: `sys_exit: init process N
 * exited with status M` or `terminate: init process N killed by signal S`.
 * A probe passes when that line has status 0.
 *
 * Not the bare "Process exited with status N", which the kernel prints for
 * every process exit including a forked child's, so neither the first nor
 * the last of them is reliably the probe's own. See score_window below.
 * That line is printed only where the debug shell is the supervisor, which
 * every image this runner boots is; an image running /sbin/init prints
 * none at all.
 *
 * This depends on the test image holding no /sbin/init: without it the
 * kernel falls to the debug shell, which `exec`s each probe as a transient
 * init that returns here on exit. A real image instead boots /sbin/init
 * and never reaches this shell. Do not stage a /sbin/init in the test
 * image, or the harness would hang waiting for a prompt that never comes.
 *
 * `-cpu max` is always passed: the entropy-dependent probe (rngprobe)
 * fails closed without RDSEED and RDRAND. Exit code: 0 if every probe
 * exited 0, else 1.
 *
 * This is the automated end-to-end pass over the test image. Every probe
 * that reports its result as an exit status runs here; the rest have a
 * suite of their own, because each needs something this shared boot does
 * not set up. `tests/run console` types at the console for stdintest and
 * ttyprobe, which block until something is typed. `tests/run fault` runs
 * vmfault, vmro and winprobe, which are killed on purpose and so have no
 * status to report. `tests/run socket` starts a host echo server for
 * udptest and tcptest, and `tests/run ping` answers pingtest.
 *
 * Usage: runtests --kernel K --initrd I [--cpu max] [--timeout S]
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

/* One boot runs all of these, in order. See the header for the probes that
   have a suite of their own. */
static const char *const PROBES[] = {
    "hello", "filetest", "forktest", "shelltest", "dirtest", "stattest",
    "touchtest",
    "rmtest", "mvtest", "lstest", "dup2test", "auxvprobe", "tlsprobe",
    "errnoprobe", "vmprobe", "forkro", "cowprobe", "cowregrant",
    "usercopyfault",
    "forkleak", "fdstale", "dirgrow", "dirprobe", "unlinkdirprobe",
    "pathcanon", "timeprobe",
    "sigprobe", "credprobe", "ownprobe", "permprobe", "sockettest",
    /* Hermetic without `--live`, which this does not pass: the resolver's
       pure helpers and the crafted wire data, not the real internet. */
    "dnstest",
    "trapprobe", "ofdprobe", "waitprobe", "pipeprobe", "pollprobe",
    "devprobe", "jobprobe", "miscprobe", "ptyprobe", "rngprobe",
    "orphanprobe", "loopprobe", "procprobe",
    "suidprobe", "sudoprobe", "modeprobe",
    /* The musl-linked identity and permission walks: prove the id family
       and the deny paths through the real libc syscall selection, not just
       raw int 0x80. */
    "musl/authmusl",
    "musl/permmusl",
    "musl/suidmusl",
    /* After those three, because it rewrites files they read and puts them
       back. It is also the slowest probe here by a wide margin, and
       deliberately so: it waits out the credential lock's real retry
       budget twice and sleeps inside a held transaction once. */
    "musl/pwauthmusl",
    /* After it, and for the same reasons doubled: it drives the shipped
       setuid passwd, which takes the same lock and rewrites the same
       files, and it restores all three databases before it exits. */
    "musl/passwdmusl",
    "musl/addusermusl",
    "musl/loginmusl",
    "rebootprobe",
    "musl/readvmusl",
    "musl/timemusl",
    "musl/clockmusl",
    "musl/statmusl",
    "musl/stampmusl",
    "musl/execargmusl",
    "musl/spawnmusl",
    /* How many mappings a process may hold, which the compiler found too
       few. */
    "musl/vmamusl",
    /* The shell's one-line form, which `su -c` depends on. It runs
       /musl/statushelper, a fixture and not a probe, which is why that one
       is staged but not listed here. */
    "musl/ishmusl",
    /* Last: it flips the modes of a staged tree and mints fixtures outside
       it. It puts every one back, but a probe that mutates shared state
       belongs where a failure part-way through cannot be another probe's
       premise. */
    "searchprobe",
    /* Beside it for the same reason: it mints a tree of its own inside the
       two staged directories of its sticky pair, and two more directories
       at the image root, and removes every one of them again. */
    "stickyprobe"
};
#define N_PROBES (int) (sizeof(PROBES) / sizeof(PROBES[0]))

/* The two markers that hand the debug shell back. Both kernel paths carry
   their own outcome on this line: sys_exit its exit status,
   terminate_current_process the signal that killed the process.
   score_window matches them at a line start, not on the trailing phrase
   alone: a probe writes to the same serial console the kernel does, so a
   pattern accepting the suffix anywhere lets a probe print "exited with
   status 0, returning to kernel shell" itself and be credited before it
   crashes or hangs. Requiring the component prefix and the pid keeps the
   verdict the kernel's to give. */
#define EXIT_HEAD "sys_exit: init process "
#define EXIT_MID " exited with status "
#define EXIT_TAIL ", returning to kernel shell"
#define KILL_HEAD "terminate: init process "
#define KILL_MID " killed by signal "
#define KILL_TAIL ", returning to kernel shell"

/* Match one marker at `at`. Fills `value` with the status or signal and
   returns the end offset, or -1. */
static long marker_at(const unsigned char *text, size_t len, size_t at,
                      const char *head, const char *mid, const char *tail,
                      int signed_value, long *value)
{
    size_t i = at;
    size_t hlen = strlen(head), mlen = strlen(mid), tlen = strlen(tail);
    int digits = 0;
    long sign = 1, n = 0;

    if (at + hlen > len || memcmp(text + at, head, hlen) != 0)
        return -1;
    i += hlen;
    while (i < len && text[i] >= '0' && text[i] <= '9') {
        i++;
        digits++;
    }
    if (digits == 0 || i + mlen > len || memcmp(text + i, mid, mlen) != 0)
        return -1;
    i += mlen;
    if (signed_value && i < len && text[i] == '-') {
        sign = -1;
        i++;
    }
    digits = 0;
    while (i < len && text[i] >= '0' && text[i] <= '9') {
        if (n > (LONG_MAX - 9) / 10)
            return -1;
        n = n * 10 + (text[i] - '0');
        i++;
        digits++;
    }
    if (digits == 0 || i + tlen > len || memcmp(text + i, tail, tlen) != 0)
        return -1;
    *value = sign * n;
    return (long) (i + tlen);
}

/* The first marker at or after `from`. `line_start` requires it to begin a
   line, which is what makes the verdict the kernel's. Returns the end
   offset, or -1; `which` is 'e' for an exit and 'k' for a kill. */
static long find_marker(const unsigned char *text, size_t len, size_t from,
                        int line_start, char *which, long *value)
{
    size_t at;

    for (at = from; at < len; at++) {
        long end;

        if (line_start && at != 0 && text[at - 1] != '\n')
            continue;
        end = marker_at(text, len, at, KILL_HEAD, KILL_MID, KILL_TAIL, 0,
                        value);
        if (end >= 0) {
            *which = 'k';
            return end;
        }
        end = marker_at(text, len, at, EXIT_HEAD, EXIT_MID, EXIT_TAIL, 1,
                        value);
        if (end >= 0) {
            *which = 'e';
            return end;
        }
    }
    return -1;
}

/* Decide one probe's result from the serial bytes its marker bounds.
   Scores the status the marker line itself has, never a bare "Process
   exited with status N": the kernel prints that for every process exit, a
   forked child's included, so neither the first nor the last of them is
   reliably the probe's own. A child that outlives its parent prints after
   it, and a probe that reaps a clean child and then dies on a signal
   prints no status of its own at all, which is how a crash used to score
   as a pass. Fills `detail` and returns nonzero when the probe passed. */
static int score_window(const unsigned char *window, size_t len, char *detail,
                        size_t detail_len)
{
    size_t at;
    long value;

    /* A kill anywhere in the window decides it, the way the Python
       original searched for the kill first. */
    for (at = 0; at < len; at++) {
        if (at != 0 && window[at - 1] != '\n')
            continue;
        if (marker_at(window, len, at, KILL_HEAD, KILL_MID, KILL_TAIL, 0,
                      &value) >= 0) {
            snprintf(detail, detail_len, "killed by signal %ld", value);
            return 0;
        }
    }
    for (at = 0; at < len; at++) {
        if (at != 0 && window[at - 1] != '\n')
            continue;
        if (marker_at(window, len, at, EXIT_HEAD, EXIT_MID, EXIT_TAIL, 1,
                      &value) >= 0) {
            snprintf(detail, detail_len, "status=%ld", value);
            return value == 0;
        }
    }
    snprintf(detail, detail_len, "no exit reported");
    return 0;
}

/* ---- the selftest ---- */

static int selftest(void)
{
    static const struct {
        const char *name;
        const char *window;
        const char *want_detail;
        int want_ok;
    } cases[] = {
        { "clean",
          "\nProcess exited with status 0\n"
          "sys_exit: init process 4 exited with status 0, "
          "returning to kernel shell\n", "status=0", 1 },
        { "clean child then signal death",
          "\nProcess exited with status 0\n"
          "terminate: init process 4 killed by signal 11, "
          "returning to kernel shell\n", "killed by signal 11", 0 },
        { "orphan exits after the parent",
          "\nProcess exited with status 3\n"
          "sys_exit: init process 4 exited with status 3, "
          "returning to kernel shell\n"
          "\nProcess exited with status 0\n", "status=3", 0 },
        { "no status at all", "some unrelated serial chatter\n",
          "no exit reported", 0 },
        { "nonzero probe status",
          "sys_exit: init process 4 exited with status 7, "
          "returning to kernel shell\n", "status=7", 0 },
        /* A probe shares the serial console with the kernel, so the phrase
           alone must not be a verdict: this one prints it and then dies. */
        { "probe forges the marker text, then is killed",
          "probe: exited with status 0, returning to kernel shell\n"
          "terminate: init process 4 killed by signal 11, "
          "returning to kernel shell\n", "killed by signal 11", 0 },
        { "probe forges the marker text and nothing else happens",
          "probe: exited with status 0, returning to kernel shell\n",
          "no exit reported", 0 }
    };
    int bad = 0, i;
    int n = (int) (sizeof(cases) / sizeof(cases[0]));

    for (i = 0; i < n; i++) {
        char detail[128];
        int ok = score_window((const unsigned char *) cases[i].window,
                              strlen(cases[i].window), detail,
                              sizeof(detail));

        if (ok != cases[i].want_ok ||
            strcmp(detail, cases[i].want_detail) != 0) {
            printf("  FAIL: %s: expected ('%s', %s), got ('%s', %s)\n",
                   cases[i].name, cases[i].want_detail,
                   cases[i].want_ok ? "True" : "False", detail,
                   ok ? "True" : "False");
            bad++;
        }
    }
    /* The window's end and the window's score read the same text by two
       rules, and the difference is deliberate. The end must be found
       wherever the kernel printed it, because the kernel writes the marker
       with no leading newline and a probe that dies after a partial line
       leaves it mid-line; an unbounded window there reads as a hang. The
       score requires a line start, so a probe cannot print itself a pass.
       An absurd digit run is not a marker under either rule. */
    {
        static const struct {
            const char *name;
            const char *text;
            int at_line_start;
            int want_found;
        } scans[] = {
            { "the kernel's marker on its own line",
              "boot\nsys_exit: init process 4 exited with status 0, "
              "returning to kernel shell\n", 1, 1 },
            { "a marker after a probe's partial line, bounding the window",
              "probe wrote thisterminate: init process 4 killed by signal "
              "11, returning to kernel shell\n", 0, 1 },
            { "that same marker under the scoring rule",
              "probe wrote thisterminate: init process 4 killed by signal "
              "11, returning to kernel shell\n", 1, 0 },
            { "a pid of more digits than a long holds",
              "\nsys_exit: init process 4 exited with status "
              "999999999999999999999999, returning to kernel shell\n", 1, 0 }
        };
        int k, scan_n = (int) (sizeof(scans) / sizeof(scans[0]));

        for (k = 0; k < scan_n; k++) {
            char which;
            long value;
            long end = find_marker((const unsigned char *) scans[k].text,
                                   strlen(scans[k].text), 0,
                                   scans[k].at_line_start, &which, &value);

            if ((end >= 0) != scans[k].want_found) {
                printf("  FAIL: %s: expected %s, got %s\n", scans[k].name,
                       scans[k].want_found ? "a marker" : "none",
                       end >= 0 ? "a marker" : "none");
                bad++;
            }
        }
        n += scan_n;
    }
    if (bad) {
        printf("runtests: selftest: %d of %d cases failed\n", bad, n);
        return 1;
    }
    printf("runtests: selftest: %d cases PASS\n", n);
    return 0;
}

/* ---- the run ---- */

static unsigned char *buf;
static size_t buf_len, buf_cap, consumed;
static pid_t child_pid = -1;
static int child_out = -1, child_in = -1;
static int child_reaped, child_status;
static FILE *logf;
static double deadline;

static double now(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (double) tv.tv_sec + (double) tv.tv_usec / 1e6;
}

static void buf_add(const unsigned char *data, size_t n)
{
    if (buf_len + n + 1 > buf_cap) {
        buf_cap = (buf_len + n + 1) * 2;
        buf = realloc(buf, buf_cap);
        if (buf == NULL) {
            fprintf(stderr, "runtests: out of memory\n");
            exit(2);
        }
    }
    memcpy(buf + buf_len, data, n);
    buf_len += n;
    buf[buf_len] = '\0';
}

static int child_poll(void)
{
    pid_t got;

    if (child_reaped)
        return child_status;
    got = waitpid(child_pid, &child_status, WNOHANG);
    if (got == child_pid) {
        child_reaped = 1;
        child_status = WIFEXITED(child_status) ? WEXITSTATUS(child_status)
                                               : 128 + WTERMSIG(child_status);
        return child_status;
    }
    return -1;
}

static void pump(double until)
{
    while (now() < until && now() < deadline && child_poll() < 0) {
        fd_set r;
        struct timeval tv;
        unsigned char chunk[1 << 16];
        ssize_t n;

        FD_ZERO(&r);
        FD_SET(child_out, &r);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        if (select(child_out + 1, &r, NULL, NULL, &tv) > 0) {
            n = read(child_out, chunk, sizeof(chunk));
            if (n > 0) {
                buf_add(chunk, (size_t) n);
                if (logf != NULL) {
                    fwrite(chunk, 1, (size_t) n, logf);
                    fflush(logf);
                }
            } else if (n == 0) {
                break;
            }
        }
    }
}

/* Wait for the literal text `what` after `consumed`; returns its end
   offset or -1. Used only for the shell prompt. */
static long expect_text(const char *what, double window)
{
    double stop = now() + window;
    size_t wlen = strlen(what);

    if (stop > deadline)
        stop = deadline;
    for (;;) {
        size_t at;

        for (at = consumed; at + wlen <= buf_len; at++)
            if (memcmp(buf + at, what, wlen) == 0) {
                consumed = at + wlen;
                return (long) consumed;
            }
        if (now() >= stop || child_poll() >= 0)
            return -1;
        pump(now() + 0.3);
    }
}

/* Wait for either shell-return marker after `consumed`. The search does
   not require a line start, the way the Python runner's own wait did not;
   the scoring that follows does. */
static long expect_marker(double window)
{
    double stop = now() + window;

    if (stop > deadline)
        stop = deadline;
    for (;;) {
        char which;
        long value;
        /* Not at a line start, unlike the scoring rule. The kernel writes
           this marker straight to serial with no leading newline, so a
           probe that dies by signal after printing a partial line leaves
           the marker in the middle of one. Requiring a line start here
           would leave that window unbounded, and the probe would read as
           a hang and stop the rest of the suite. A probe that prints the
           text itself ends its own window early and then scores as no
           exit reported, which is a failure either way. */
        long end = find_marker(buf, buf_len, consumed, 0, &which, &value);

        if (end >= 0) {
            consumed = (size_t) end;
            return end;
        }
        if (now() >= stop || child_poll() >= 0)
            return -1;
        pump(now() + 0.3);
    }
}

static void send_line(const char *text)
{
    if (child_in < 0)
        return;
    if (write(child_in, text, strlen(text)) < 0 ||
        write(child_in, "\n", 1) < 0) {
        /* A closed pipe means the guest is gone; the caller notices. */
    }
}

int main(int argc, char **argv)
{
    const char *kernel = NULL, *initrd = NULL, *cpu = "max";
    const char *log = NULL, *append = NULL;
    /* The card to attach. The RTL8139 is what this suite has always booted
       against; naming another is how the same probes are run on another
       driver. */
    const char *device = "rtl8139,netdev=net0";
    double timeout = 180.0, steptmo = 20.0, boottmo = 60.0;
    char *qemu[32];
    int nq = 0, i;
    int out_pipe[2], in_pipe[2];
    struct {
        const char *name;
        char detail[128];
        int ok;
    } results[N_PROBES];
    int n_results = 0, fails = 0;
    double clock_seen_at = 0;
    int clock_ok = 0;
    char clock_detail[128];

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--selftest") == 0)
            return selftest();
        else if (strcmp(argv[i], "--kernel") == 0 && i + 1 < argc)
            kernel = argv[++i];
        else if (strcmp(argv[i], "--initrd") == 0 && i + 1 < argc)
            initrd = argv[++i];
        else if (strcmp(argv[i], "--cpu") == 0 && i + 1 < argc)
            cpu = argv[++i];
        else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc)
            log = argv[++i];
        else if (strcmp(argv[i], "--append") == 0 && i + 1 < argc)
            append = argv[++i];
        else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc)
            device = argv[++i];
        else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc)
            timeout = atof(argv[++i]);
        else if (strcmp(argv[i], "--steptmo") == 0 && i + 1 < argc)
            steptmo = atof(argv[++i]);
        else if (strcmp(argv[i], "--boottmo") == 0 && i + 1 < argc)
            boottmo = atof(argv[++i]);
        else {
            fprintf(stderr, "runtests: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (kernel == NULL || initrd == NULL) {
        fprintf(stderr, "runtests: --kernel and --initrd are required\n");
        return 2;
    }

    /* -rtc base=utc is QEMU's default and is passed anyway: this kernel
       reads the CMOS as UTC (there is no timezone in those registers to
       read), so a host configured for a local-time RTC would hand the
       guest a date wrong by its offset, and the clock check below would
       report it as our bug. */
    qemu[nq++] = (char *) "qemu-system-i386";
    qemu[nq++] = (char *) "-cpu";
    qemu[nq++] = (char *) cpu;
    qemu[nq++] = (char *) "-kernel";
    qemu[nq++] = (char *) kernel;
    qemu[nq++] = (char *) "-initrd";
    qemu[nq++] = (char *) initrd;
    qemu[nq++] = (char *) "-serial";
    qemu[nq++] = (char *) "stdio";
    qemu[nq++] = (char *) "-display";
    qemu[nq++] = (char *) "none";
    qemu[nq++] = (char *) "-rtc";
    qemu[nq++] = (char *) "base=utc";
    qemu[nq++] = (char *) "-no-reboot";
    qemu[nq++] = (char *) "-netdev";
    qemu[nq++] = (char *) "user,id=net0";
    qemu[nq++] = (char *) "-device";
    qemu[nq++] = (char *) device;
    if (append != NULL) {
        qemu[nq++] = (char *) "-append";
        qemu[nq++] = (char *) append;
    }
    qemu[nq] = NULL;

    if (pipe(out_pipe) != 0 || pipe(in_pipe) != 0) {
        fprintf(stderr, "runtests: cannot make a pipe\n");
        return 2;
    }
    child_pid = fork();
    if (child_pid < 0) {
        fprintf(stderr, "qemu launch failed: %s\n", strerror(errno));
        return 2;
    }
    if (child_pid == 0) {
        dup2(in_pipe[0], 0);
        dup2(out_pipe[1], 1);
        dup2(out_pipe[1], 2);
        close(in_pipe[1]);
        close(out_pipe[0]);
        execvp(qemu[0], qemu);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    child_out = out_pipe[0];
    child_in = in_pipe[1];
    fcntl(child_out, F_SETFL, O_NONBLOCK);
    signal(SIGPIPE, SIG_IGN);
    if (log != NULL) {
        logf = fopen(log, "wb");
        if (logf == NULL)
            fprintf(stderr, "runtests: cannot write %s: %s; the run "
                    "continues without a transcript\n", log,
                    strerror(errno));
    }
    deadline = now() + timeout;
    buf_add((const unsigned char *) "", 0);

    if (expect_text("Type 'help'", boottmo) < 0) {
        fprintf(stderr, "runtests: kernel shell never appeared\n");
        return 3;
    }

    for (i = 0; i < N_PROBES; i++) {
        size_t start = buf_len;
        char line[256];
        long end;

        snprintf(line, sizeof(line), "exec %s", PROBES[i]);
        /* The window starts where this probe's output will start. Anything
           already buffered came from the shell, not from the probe, and a
           marker left in it would end the window before it began. */
        consumed = start;
        send_line(line);
        /* The probe runs AS the init process, and only its own exit
           returns the kernel shell, so that marker bounds the window. The
           marker is on serial, so it survives a probe that left the
           console tty in raw mode (ttyprobe). */
        end = expect_marker(steptmo);
        if (end >= 0 && (size_t) end < start) {
            fprintf(stderr, "runtests: the marker for %s ends before its "
                    "window begins; the scan is out of step\n", PROBES[i]);
            return 2;
        }
        if (end < 0) {
            results[n_results].name = PROBES[i];
            snprintf(results[n_results].detail,
                     sizeof(results[n_results].detail),
                     "no shell return (hang?)");
            results[n_results].ok = 0;
            n_results++;
            break;
        }
        results[n_results].name = PROBES[i];
        results[n_results].ok =
            score_window(buf + start, (size_t) end - start,
                         results[n_results].detail,
                         sizeof(results[n_results].detail));
        n_results++;
        if (strcmp(PROBES[i], "musl/clockmusl") == 0) {
            /* Host time AT the probe, not at the end of the suite: the
               guest printed its epoch a moment ago, so the two are
               comparable to the second. Sampling after the remaining
               probes would leave a window minutes wide, and a clock
               minutes wrong would pass. */
            clock_seen_at = now();
        }
    }

    pump(now() + 1.0);
    if (child_poll() < 0) {
        kill(child_pid, SIGTERM);
        usleep(300000);
        if (child_poll() < 0) {
            kill(child_pid, SIGKILL);
            usleep(300000);
            child_poll();
        }
    }
    if (logf != NULL)
        fclose(logf);

    /* The clock, end to end. clockmusl checks that its own date is real
       and advancing; only this side can check it against the clock QEMU
       handed the machine, which is what catches a date wrong by hours, not
       by years (a local-time CMOS read as UTC, say). The anchor's own
       arithmetic is pinned exactly in tests/host/rtc_hosttest.c and not
       here: that error is under a second and no tolerance would see it. */
    snprintf(clock_detail, sizeof(clock_detail), "the probe printed no epoch");
    {
        const char *want = "CLOCKMUSL EPOCH ";
        size_t wlen = strlen(want), at;

        for (at = 0; at + wlen < buf_len; at++) {
            if (memcmp(buf + at, want, wlen) == 0) {
                const char *p = (const char *) buf + at + wlen;

                if (*p >= '0' && *p <= '9' && clock_seen_at != 0) {
                    long guest = strtol(p, NULL, 10);
                    double skew = clock_seen_at - (double) guest;

                    snprintf(clock_detail, sizeof(clock_detail),
                             "%+d s vs the host", (int) skew);
                    clock_ok = skew >= -30 && skew <= 30;
                }
                break;
            }
        }
    }

    for (i = 0; i < n_results; i++) {
        if (!results[i].ok)
            fails++;
        printf("  %-12s %-18s %s\n", results[i].name, results[i].detail,
               results[i].ok ? "PASS" : "FAIL");
    }

    /* A hang breaks the loop, so a short run is a failure in its own right
       and names what never ran. Printing the smaller count alone would
       read as a complete suite that happened to be smaller today. */
    {
        int missing = 0;
        char names[4096];
        size_t used = 0;

        names[0] = '\0';
        for (i = 0; i < N_PROBES; i++) {
            int ran = 0, j;

            for (j = 0; j < n_results; j++)
                if (strcmp(results[j].name, PROBES[i]) == 0)
                    ran = 1;
            if (!ran) {
                int want;

                missing++;
                want = snprintf(names + used, sizeof(names) - used, "%s%s",
                                used ? " " : "", PROBES[i]);
                if (want > 0 && (size_t) want < sizeof(names) - used)
                    used += (size_t) want;
                else
                    used = sizeof(names) - 1;
            }
        }
        if (missing > 0) {
            fails += missing;
            printf("runtests: %d probe(s) never ran: %s\n", missing, names);
        }
    }

    /* Reported beside the probes, not as one, so the count stays the count
       of probes. */
    printf("  %-12s %-18s %s\n", "clock", clock_detail,
           clock_ok ? "PASS" : "FAIL");
    if (!clock_ok)
        fails++;

    printf("runtests: %d of %d probes, %d failures\n", n_results, N_PROBES,
           fails);
    return fails == 0 ? 0 : 1;
}
