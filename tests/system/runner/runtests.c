/**
 * runtests: boot the test image once and run every probe of `PROBES`,
 * requiring each to exit 0.
 *
 * Usage: runtests --kernel K --initrd I [--cpu CPU] [--device DEV]
 *                 [--append LINE] [--log FILE] [--timeout S] [--steptmo S]
 *                 [--boottmo S]
 *        runtests --selftest
 *
 * It boots the architecture's QEMU, RACH2_QEMU or else qemu-system-i386,
 * headless with the kernel and the test RAM disk, on QEMU's user network
 * with the card DEV, an rtl8139 unless given, and with `-cpu max` unless CPU
 * is given, since rngprobe fails without RDSEED and RDRAND. It waits for the
 * kernel shell, `exec`s each probe in turn and scores it from the line that
 * hands the shell back: `sys_exit: init process N exited with status M` or
 * `terminate: init process N killed by signal S`. A probe passes when that
 * line has status 0. The record `process: exited with status N` does not
 * decide it, since the kernel prints one for every process that exits, a
 * forked child's included; see `score_window`.
 *
 * The test image has no /sbin/init, so the kernel falls to its debug shell,
 * which runs each probe as a transient init and is returned to when the
 * probe exits. The kernel prints the line only where the debug shell is the
 * supervisor; an image with /sbin/init never reaches the shell, and a run of
 * it ends at the boot limit with the shell never seen.
 *
 * Besides the probes, the run scores the kernel's `timing:` record and
 * compares the epoch clockmusl prints with the host's clock. --timeout
 * bounds the run, 180 seconds unless given, --steptmo each probe, 20, and
 * --boottmo the wait for the shell, 60, each multiplied by the set's
 * deadline factor. A probe that does not return within its limit ends the
 * run, and each probe after it counts as a failure. --log keeps the console
 * in FILE. The exit status is 0 when every probe and both checks pass, 1
 * otherwise, 2 for a usage error or a run that could not start, and 3 when
 * the kernel shell never appeared. `--selftest` checks the scoring on fixed
 * text.
 *
 * Every probe that reports its result as an exit status runs here; the rest
 * have a suite of their own, since each needs something this shared boot
 * does not set up. `tests/run console` types at the console for stdintest
 * and ttyprobe, which block until something is typed. `tests/run fault`
 * runs vmfault, vmro and winprobe, which are killed on purpose and so have
 * no status to report. `tests/run socket` starts a host echo server for
 * udptest and tcptest, and `tests/run ping` answers pingtest.
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
   have a suite of their own. tools/verify/probecheck.sh reads the names from
   this table's lines that begin with a quoted name. */
static const char *const PROBES[] = {
    "hello", "filetest", "forktest", "shelltest", "dirtest", "stattest", "touchtest", "rmtest",
    "mvtest", "lstest", "dup2test", "auxvprobe", "tlsprobe", "fsbaseprobe", "errnoprobe", "vmprobe",
    "forkro", "cowprobe", "cowregrant", "usercopyfault", "forkleak", "fdstale", "dirgrow",
    "dirprobe", "unlinkdirprobe", "pathcanon", "vfsprobe", "timeprobe", "sigprobe", "credprobe",
    "ownprobe", "permprobe", "sockettest",
    /* The YMM registers across switches, signals, an exec and ptrace, with
       XSAVE's frame and register set; without AVX its rows say they did
       not run. */
    "xsaveprobe",
    /* The four limits: the process table, a process's descriptors, the
       socket pool and the frames. Each is filled, the refusal the kernel
       documents is required, and then the resource is required back. */
    "fdfull", "oomprobe",
    /* The limits prlimit64 keeps: processes per user, descriptors and the
       address space, each refused by name. */
    "limitprobe",
    /* Without `--live`, which this does not pass, dnstest runs the
       resolver's helpers that use no network, on fixed wire data. */
    "dnstest", "trapprobe", "ofdprobe", "waitprobe", "pipeprobe", "pollprobe", "devprobe",
    "jobprobe", "miscprobe", "ptyprobe", "rngprobe", "orphanprobe", "loopprobe", "zombieprobe",
    "exitprobe", "survivorprobe", "procprobe", "suidprobe", "sudoprobe", "modeprobe",
    /* The identity and permission calls and their refusals, made through
       musl, which chooses the system calls itself. */
    "musl/authmusl", "musl/permmusl", "musl/suidmusl",
    /* After those three, since it rewrites files they read and puts them
       back. It is the slowest probe here: it waits out the credential
       lock's retry limit twice and sleeps once inside a held
       transaction. */
    "musl/pwauthmusl",
    /* After it, for the same reason: it runs the image's setuid passwd,
       which takes the same lock and rewrites the same files, and it
       restores all three databases before it exits. */
    "musl/passwdmusl", "musl/addusermusl", "musl/loginmusl", "rebootprobe", "musl/readvmusl",
    "musl/timemusl", "musl/clockmusl", "musl/statmusl", "musl/stampmusl", "musl/execargmusl",
    "musl/spawnmusl", "musl/sigmusl",
    /* clone in glibc's fork form, set_tid_address and futex. */
    "musl/clonemusl",
    /* Faults to handlers, SA_RESTART, the alternate stack, int3, the
       pending set, sigsuspend's mask and SIGCHLD's siginfo. */
    "musl/sigframemusl",
    /* How many mappings a process may hold. */
    "musl/vmamusl",
    /* The file calls GNU libc makes: paths of PATH_MAX bytes, the
       descriptor forms of the calls that take a path, the namespace
       calls through a directory descriptor, positioned reads and writes,
       truncation by path, O_PATH descriptors, lchown, the entry types
       getdents64 reports, and the file system fstatfs names for each kind
       of descriptor. */
    "musl/pathmaxmusl", "musl/fdcallmusl", "musl/atcallmusl", "musl/preadmusl", "musl/truncmusl",
    "musl/opathmusl", "musl/lchownmusl", "musl/direntmusl", "musl/statfsmusl",
    /* The process calls static GNU programs make, each group through the
       libc that has them: the CPU time the kernel charges, the clocks, how a
       process is named and scheduled, waitid, prctl, the stack limit, and the
       flags a socket is made with. */
    "musl/cputimemusl", "musl/clockidmusl", "musl/schedmusl", "musl/waitidmusl", "musl/prctlmusl",
    "musl/stackmusl", "musl/sockflagmusl",
    /* Capabilities, which Chromium's zygote gives up in each process it
       forks. */
    "musl/capmusl",
    /* eventfd, and epoll's level-triggered, edge-triggered and one-shot
       reports; timerfd's expirations, alone and through epoll. */
    "musl/epollmusl", "musl/timerfdmusl",
    /* The netlink route socket's dumps and refusals, the interface ioctls
       on an AF_UNIX socket, and a uevent socket that hears nothing. */
    "musl/netlinkmusl",
    /* inotify's events of a directory and a file, and its refusals. */
    "musl/inotifymusl",
    /* AF_UNIX: pairs, descriptors and credentials in messages, and names
       in the file system and the abstract namespace. */
    "musl/unixmusl",
    /* File mappings, shared memory, mremap and madvise. */
    "musl/mapmusl",
    /* System V shared memory, semaphores and message queues, and POSIX
       message queues. */
    "musl/ipcmusl",
    /* seccomp's filters: errno, trap, kill, TSYNC, exec, strict mode. */
    "musl/seccompmusl",
    /* Namespaces: user, PID, mount, network, UTS and IPC, by unshare,
       setns and clone. */
    "musl/nsmusl",
    /* FIFO files made by mkfifo and mknodat: their opens, waits and
       bytes. */
    "musl/fifomusl",
    /* statx, which both libcs call first for every stat, beside
       fstatat64. */
    "musl/statxmusl",
    /* Record, open file description and flock locks between processes:
       conflicts, splits, waits, EDEADLK and the closes that drop them. */
    "musl/lockmusl",
    /* tmpfs mounted by a program and at /dev/shm. */
    "musl/tmpfsmusl",
    /* mknod's kinds on KRFFS and tmpfs: their types, a device node's
       number and open, and the refusals in Linux's order. */
    "musl/mknodmusl",
    /* memfd_create, its descriptor and name, and each seal. */
    "musl/memfdmusl",
    /* chroot on KRFFS and tmpfs: paths, links and ".." inside the root,
       fork and exec keeping it, and the rmdir and unmount it refuses. */
    "musl/chrootmusl",
    /* vfork and clone with CLONE_VM|CLONE_VFORK, as posix_spawn, system
       and popen start their children. */
    "musl/vforkmusl",
    /* ptrace as GDB drives it: memory, registers, a step, a breakpoint,
       syscall stops, the exec event, a debug register, attach and detach. */
    "musl/ptracemusl",
    /* sigtimedwait, the memory lock calls, mincore, getcpu, personality,
       sendmmsg and recvmmsg, clock_gettime64 and close_range. */
    "musl/callsmusl",
    /* Core files: their notes and memory, the limit, the dumpable flag,
       and a link at the name. */
    "musl/coremusl",
    /* Threads: their ids, locks and signals, the descriptors, directories
       and umask they share, /proc/self/task, and exec and exit from one. */
    "musl/threadmusl",
    /* The shell's one-line form, which `su -c` depends on. It runs
       /musl/statushelper, a fixture and not a probe, which is why that one
       is staged but not listed here. */
    "musl/ishmusl",
    /* Last, since it changes the modes of a staged tree and makes files
       outside it. It puts every one back, but a failure part way through
       would leave them changed for the probes after it. */
    "searchprobe",
    /* Beside it for the same reason: it makes a tree of its own inside the
       two staged directories of its sticky pair, and two more directories
       at the image root, and removes every one of them again. */
    "stickyprobe"};
#define N_PROBES (int)(sizeof(PROBES) / sizeof(PROBES[0]))

/* The probes of an interface only i386 has, which tests/Makefile builds for
   i686 alone: on a boot of another architecture each is reported as not run
   and counts as no failure. */
static const char *const I686_ONLY[] = {"tlsprobe"};
#define N_I686_ONLY (int)(sizeof(I686_ONLY) / sizeof(I686_ONLY[0]))

/** The emulator: RACH2_QEMU, which the suites set to the architecture's
 * (mk/arch.mk), or qemu-system-i386. */
static const char *qemu_binary(void)
{
    const char *q = getenv("RACH2_QEMU");

    return q != NULL && q[0] != '\0' ? q : "qemu-system-i386";
}

/** Whether `probe` is one this boot's architecture has no build of: the
 * i686-only probes, when RACH2_ARCH names another architecture. */
static int not_built_here(const char *probe)
{
    const char *arch = getenv("RACH2_ARCH");
    int i;

    if (arch == NULL || arch[0] == '\0' || strcmp(arch, "i686") == 0)
        return 0;
    for (i = 0; i < N_I686_ONLY; i++)
        if (strcmp(probe, I686_ONLY[i]) == 0)
            return 1;
    return 0;
}

/* The two markers that hand the debug shell back: sys_exit's, with the init
   process's exit status, and terminate's, with the signal that killed it.
   `score_window` matches a marker whole, with its component and pid, at the
   start of a line: a probe writes to the same serial console as the kernel,
   and one that printed the marker's phrase itself must not be credited with
   a pass. */
#define EXIT_HEAD "sys_exit: init process "
#define EXIT_MID  " exited with status "
#define EXIT_TAIL ", returning to kernel shell"
#define KILL_HEAD "terminate: init process "
#define KILL_MID  " killed by signal "
#define KILL_TAIL ", returning to kernel shell"

/**
 * Match the marker `head`, a pid, `mid`, a number and `tail` at offset `at`
 * of the `len` bytes of `text`, the number with an optional minus sign when
 * `signed_value` is set. Returns the offset past the marker, with the number
 * in `*value`, or -1 when there is no marker there or its number does not
 * fit a long.
 */
static long marker_at(const unsigned char *text, size_t len, size_t at, const char *head,
                      const char *mid, const char *tail, int signed_value, long *value)
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
    return (long)(i + tlen);
}

/**
 * The offset past a record's prefix, `<N>[SECONDS] `, at `at`, or `at` when
 * there is none. The kernel writes both markers as records, so the prefix
 * begins the line and the marker follows it.
 */
static size_t past_prefix(const unsigned char *text, size_t len, size_t at)
{
    size_t i = at;

    if (i + 4 > len || text[i] != '<' || text[i + 2] != '>')
        return at;
    i += 3;
    if (text[i] != '[')
        return at;
    i++;
    while (i < len && ((text[i] >= '0' && text[i] <= '9') || text[i] == '.'))
        i++;
    if (i + 2 > len || text[i] != ']' || text[i + 1] != ' ')
        return at;
    return i + 2;
}

/**
 * Find the first marker at or after `from`. Returns the offset past it, with
 * 'k' for a kill or 'e' for an exit in `*which` and its number in `*value`,
 * or -1. With `line_start` set the marker must begin a line or follow the
 * record prefix that does, which is `score_window`'s rule; without it a
 * marker anywhere counts, which is `expect_marker`'s.
 */
static long find_marker(const unsigned char *text, size_t len, size_t from, int line_start,
                        char *which, long *value)
{
    size_t at;

    for (at = from; at < len; at++) {
        long end;
        size_t start = at;

        if (line_start) {
            if (at != 0 && text[at - 1] != '\n')
                continue;
            start = past_prefix(text, len, at);
        }
        end = marker_at(text, len, start, KILL_HEAD, KILL_MID, KILL_TAIL, 0, value);
        if (end >= 0) {
            *which = 'k';
            return end;
        }
        end = marker_at(text, len, start, EXIT_HEAD, EXIT_MID, EXIT_TAIL, 1, value);
        if (end >= 0) {
            *which = 'e';
            return end;
        }
    }
    return -1;
}

/**
 * Score one probe from the `len` bytes of serial output in its window.
 * Returns 1 when the first exit marker at the start of a line has status 0
 * and no kill marker starts a line, and 0 otherwise, with the status, the
 * signal or "no exit reported" in `detail`.
 *
 * A kill marker decides the window wherever it is, before any exit marker.
 * Only the marker is read, never a record `process: exited with status N`:
 * the kernel prints one for every process that exits, a forked child's
 * included, so a child that outlives its parent prints after it, and a
 * probe that reaps a clean child and then dies by a signal prints none of
 * its own.
 */
static int score_window(const unsigned char *window, size_t len, char *detail, size_t detail_len)
{
    size_t at;
    long value;

    for (at = 0; at < len; at++) {
        if (at != 0 && window[at - 1] != '\n')
            continue;
        if (marker_at(window, len, past_prefix(window, len, at), KILL_HEAD, KILL_MID, KILL_TAIL, 0,
                      &value) >= 0) {
            snprintf(detail, detail_len, "killed by signal %ld", value);
            return 0;
        }
    }
    for (at = 0; at < len; at++) {
        if (at != 0 && window[at - 1] != '\n')
            continue;
        if (marker_at(window, len, past_prefix(window, len, at), EXIT_HEAD, EXIT_MID, EXIT_TAIL, 1,
                      &value) >= 0) {
            snprintf(detail, detail_len, "status=%ld", value);
            return value == 0;
        }
    }
    snprintf(detail, detail_len, "no exit reported");
    return 0;
}

/**
 * The offset of the first line at or after `from` that begins with `head`
 * and has `tail` later on it, or -1; a null or empty `tail` asks for `head`
 * alone. A record is matched in two parts, since the time between them is
 * not fixed text, and only at the start of a line, since a probe that
 * printed a record's text in its own output must not count as one.
 */
static long find_text(const unsigned char *hay, size_t len, size_t from, const char *head,
                      const char *tail)
{
    size_t hlen = strlen(head), at;

    for (at = from; at + hlen <= len; at++) {
        if (at != 0 && hay[at - 1] != '\n')
            continue;
        if (memcmp(hay + at, head, hlen) != 0)
            continue;
        if (tail == (const char *)0 || tail[0] == '\0')
            return (long)at;
        {
            size_t tlen = strlen(tail), p = at + hlen;

            while (p + tlen <= len && hay[p] != '\n') {
                if (memcmp(hay + p, tail, tlen) == 0)
                    return (long)at;
                p++;
            }
        }
    }
    return -1;
}

/**
 * Score what the kernel says about time: 1 when it reported its measurements,
 * with them in `detail`, 0 with the reason otherwise.
 *
 * The kernel prints one record at every boot, `timing: tick N Hz, ...`, at
 * level 6 when its clock's period accounting passes its check on fixed
 * readings and at level 3 when it does not, so the record's level is the
 * verdict and the rest of the line is the evidence; the delay, the wait and
 * the walk against the tick are measured against time the host owns and are
 * printed for a person, not judged.
 */
static int score_kernel_timing(const unsigned char *buf, size_t buf_len, char *detail, size_t size)
{
    static const char *const MARK = "] timing: ";
    long said = find_text(buf, buf_len, 0, "<6>[", MARK);
    int ok = 1;

    if (said < 0) {
        said = find_text(buf, buf_len, 0, "<3>[", MARK);
        ok = 0;
    }
    if (said < 0) {
        snprintf(detail, size, "the kernel said nothing about time");
        return 0;
    }
    {
        const char *from = strstr((const char *)buf + said, MARK) + strlen(MARK);
        size_t n = 0;

        while (from[n] != '\0' && from[n] != '\n' && from[n] != '\r' && n + 1 < size) {
            n++;
        }
        snprintf(detail, size, "%.*s", (int)n, from);
    }
    return ok;
}

/* ---- the selftest ---- */

/**
 * Check the scoring on fixed text, printing each case that fails. Returns 0
 * when every case passes and 1 otherwise.
 *
 * The windows check `score_window`. The scans check the two rules of
 * `find_marker`, which read the same text differently on purpose: the end
 * of a window is found wherever the kernel printed its marker, since the
 * kernel writes it with no newline before it and a probe that dies after a
 * partial line leaves it inside that line, where a window that did not end
 * would read as a hang; the score requires the start of a line, so a probe
 * cannot print itself a pass. A number too long for a long is a marker
 * under neither rule. The records check `score_kernel_timing`.
 */
static int selftest(void)
{
    static const struct {
        const char *name;
        const char *window;
        const char *want_detail;
        int want_ok;
    } cases[] = {{"clean",
                  "<6>[1.20] process: exited with status 0\n"
                  "<6>[1.21] sys_exit: init process 4 exited with status 0, "
                  "returning to kernel shell\n",
                  "status=0", 1},
                 /* The markers without a record's prefix. */
                 {"clean, unprefixed",
                  "\nProcess exited with status 0\n"
                  "sys_exit: init process 4 exited with status 0, "
                  "returning to kernel shell\n",
                  "status=0", 1},
                 {"a probe forges the prefix as well",
                  "<6>[1.20] probe: sys_exit: init process 4 exited with status 0, "
                  "returning to kernel shell\n",
                  "no exit reported", 0},
                 {"clean child then signal death",
                  "\nProcess exited with status 0\n"
                  "terminate: init process 4 killed by signal 11, "
                  "returning to kernel shell\n",
                  "killed by signal 11", 0},
                 {"orphan exits after the parent",
                  "\nProcess exited with status 3\n"
                  "sys_exit: init process 4 exited with status 3, "
                  "returning to kernel shell\n"
                  "\nProcess exited with status 0\n",
                  "status=3", 0},
                 {"no status at all", "some unrelated serial chatter\n", "no exit reported", 0},
                 {"nonzero probe status",
                  "sys_exit: init process 4 exited with status 7, "
                  "returning to kernel shell\n",
                  "status=7", 0},
                 /* A probe prints the marker's phrase itself and then dies. */
                 {"probe forges the marker text, then is killed",
                  "probe: exited with status 0, returning to kernel shell\n"
                  "terminate: init process 4 killed by signal 11, "
                  "returning to kernel shell\n",
                  "killed by signal 11", 0},
                 {"probe forges the marker text and nothing else happens",
                  "probe: exited with status 0, returning to kernel shell\n", "no exit reported",
                  0}};
    int bad = 0, i;
    int n = (int)(sizeof(cases) / sizeof(cases[0]));

    for (i = 0; i < n; i++) {
        char detail[128];
        int ok = score_window((const unsigned char *)cases[i].window, strlen(cases[i].window),
                              detail, sizeof(detail));

        if (ok != cases[i].want_ok || strcmp(detail, cases[i].want_detail) != 0) {
            printf("  FAIL: %s: expected ('%s', %s), got ('%s', %s)\n", cases[i].name,
                   cases[i].want_detail, cases[i].want_ok ? "True" : "False", detail,
                   ok ? "True" : "False");
            bad++;
        }
    }
    {
        static const struct {
            const char *name;
            const char *text;
            int at_line_start;
            int want_found;
        } scans[] = {{"the kernel's marker on its own line",
                      "boot\nsys_exit: init process 4 exited with status 0, "
                      "returning to kernel shell\n",
                      1, 1},
                     {"a marker after a probe's partial line, bounding the window",
                      "probe wrote thisterminate: init process 4 killed by signal "
                      "11, returning to kernel shell\n",
                      0, 1},
                     {"that same marker under the scoring rule",
                      "probe wrote thisterminate: init process 4 killed by signal "
                      "11, returning to kernel shell\n",
                      1, 0},
                     {"a pid of more digits than a long holds",
                      "\nsys_exit: init process 4 exited with status "
                      "999999999999999999999999, returning to kernel shell\n",
                      1, 0}};
        int k, scan_n = (int)(sizeof(scans) / sizeof(scans[0]));

        for (k = 0; k < scan_n; k++) {
            char which;
            long value;
            long end = find_marker((const unsigned char *)scans[k].text, strlen(scans[k].text), 0,
                                   scans[k].at_line_start, &which, &value);

            if ((end >= 0) != scans[k].want_found) {
                printf("  FAIL: %s: expected %s, got %s\n", scans[k].name,
                       scans[k].want_found ? "a marker" : "none", end >= 0 ? "a marker" : "none");
                bad++;
            }
        }
        n += scan_n;
    }
    {
        static const struct {
            const char *name;
            const char *log;
            const char *want_detail;
            int want_ok;
        } groups[] = {
            {"the machine agrees with itself about time",
             "<6>[0.51] timing: tick 100 Hz, 200 ms delay measured 200 ms, "
             "2000 us wait measured 2001 us\n",
             "tick 100 Hz, 200 ms delay measured 200 ms, 2000 us wait measured 2001 us", 1},
            {"a measurement that does not agree is an error record",
             "<3>[0.51] timing: tick 100 Hz, 200 ms delay measured 200 ms, "
             "2000 us wait measured 1361 us\n",
             "tick 100 Hz, 200 ms delay measured 200 ms, 2000 us wait measured 1361 us", 0},
            {"the record is taken from this boot and not from a probe's text",
             "<6>[1.20] probe: timing: tick 1 Hz, 200 ms delay measured 9 ms, "
             "2000 us wait measured 9 us\n"
             "<6>[1.21] timing: tick 100 Hz, 200 ms delay measured 200 ms, "
             "2000 us wait measured 2001 us\n",
             "tick 100 Hz, 200 ms delay measured 200 ms, 2000 us wait measured 2001 us", 1},
            {"nothing said at all", "boot\n", "the kernel said nothing about time", 0}};
        int k, group_n = (int)(sizeof(groups) / sizeof(groups[0]));

        for (k = 0; k < group_n; k++) {
            char detail[128];
            int ok = score_kernel_timing((const unsigned char *)groups[k].log,
                                         strlen(groups[k].log), detail, sizeof(detail));

            if (ok != groups[k].want_ok || strcmp(detail, groups[k].want_detail) != 0) {
                printf("  FAIL: %s: expected ('%s', %s), got ('%s', %s)\n", groups[k].name,
                       groups[k].want_detail, groups[k].want_ok ? "True" : "False", detail,
                       ok ? "True" : "False");
                bad++;
            }
        }
        n += group_n;
    }
    if (bad) {
        printf("runtests: selftest: %d of %d cases failed\n", bad, n);
        return 1;
    }
    printf("runtests: selftest: %d cases PASS\n", n);
    return 0;
}

/* ---- the run ---- */

/* The serial output read so far, NUL-terminated, and the offset the waits
   search from. */
static unsigned char *buf;
static size_t buf_len, buf_cap, consumed;
/* QEMU's process, the ends of its serial pipes, and, once it is reaped, its
   exit status, 128 + N after signal N. */
static pid_t child_pid = -1;
static int child_out = -1, child_in = -1;
static int child_reaped, child_status;
/* The transcript --log asks for, or NULL. */
static FILE *logf;
/* The end of the run, on the calendar clock. */
static double deadline;

/** Seconds on the calendar clock. */
static double now(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

/* The set's deadline factor, which multiplies every completion bound. */
static double deadline_factor = 1.0;

/**
 * Read the deadline factor. tests/run passes its `RACH2_TIMEOUT_FACTOR` to
 * every suite as `RACH2_DEADLINES=scaled` with `RACH2_DEADLINE_FACTOR=F`. A
 * process told nothing, told the mode without a factor, or given a factor
 * outside 1 to 64 keeps the factor 1.
 */
static void deadline_factor_read(void)
{
    const char *mode = getenv("RACH2_DEADLINES");
    const char *factor = getenv("RACH2_DEADLINE_FACTOR");
    double f;

    if (mode == NULL || strcmp(mode, "scaled") != 0 || factor == NULL)
        return;
    f = atof(factor);
    if (f >= 1.0 && f <= 64.0)
        deadline_factor = f;
}

/** A completion bound of `seconds` multiplied by the deadline factor. */
static double scaled(double seconds)
{
    return seconds * deadline_factor;
}

/** Append `n` bytes to `buf`, keeping it NUL-terminated; exits with status
 * 2 when there is no memory. */
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

/** QEMU's exit status, 128 + N after signal N, or -1 while it runs. */
static int child_poll(void)
{
    pid_t got;

    if (child_reaped)
        return child_status;
    got = waitpid(child_pid, &child_status, WNOHANG);
    if (got == child_pid) {
        child_reaped = 1;
        child_status =
            WIFEXITED(child_status) ? WEXITSTATUS(child_status) : 128 + WTERMSIG(child_status);
        return child_status;
    }
    return -1;
}

/** Read QEMU's output into `buf` and the transcript until `until`, the
 * run's deadline, QEMU's exit or the end of the pipe. */
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
                buf_add(chunk, (size_t)n);
                if (logf != NULL) {
                    fwrite(chunk, 1, (size_t)n, logf);
                    fflush(logf);
                }
            } else if (n == 0) {
                break;
            }
        }
    }
}

/**
 * Wait `window` seconds, and not past the deadline, for the text `what`
 * after `consumed`. Returns the offset past it, which becomes `consumed`,
 * or -1 when the time runs out or QEMU exits.
 */
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
                return (long)consumed;
            }
        if (now() >= stop || child_poll() >= 0)
            return -1;
        pump(now() + 0.3);
    }
}

/**
 * Wait `window` seconds, and not past the deadline, for either marker after
 * `consumed`. Returns the offset past it, which becomes `consumed`, or -1
 * when the time runs out or QEMU exits.
 *
 * A marker is found anywhere, not only at the start of a line as the score
 * requires: the kernel writes it with no newline before it, so a probe that
 * dies by a signal after a partial line leaves it inside that line, and a
 * window that did not end there would read as a hang and stop the rest of
 * the run. A probe that prints the marker's text itself ends its own window
 * early and then scores as no exit reported, a failure either way.
 */
static long expect_marker(double window)
{
    double stop = now() + window;

    if (stop > deadline)
        stop = deadline;
    for (;;) {
        char which;
        long value;
        long end = find_marker(buf, buf_len, consumed, 0, &which, &value);

        if (end >= 0) {
            consumed = (size_t)end;
            return end;
        }
        if (now() >= stop || child_poll() >= 0)
            return -1;
        pump(now() + 0.3);
    }
}

/** Write `text` and a newline to QEMU's serial input. A write that fails,
 * since the machine has gone, is left for the wait after it to find. */
static void send_line(const char *text)
{
    if (child_in < 0)
        return;
    if (write(child_in, text, strlen(text)) < 0 || write(child_in, "\n", 1) < 0) {
        /* A closed pipe means the guest is gone; the caller notices. */
    }
}

/**
 * Boot the test image, run the probes in order, and print each probe's
 * result, the timing and clock checks and the count of failures.
 *
 * A probe's window starts where its output starts, since what was read
 * before it came from the shell and a marker in it would end the window
 * before it began, and ends at the marker of its return to the shell. The
 * marker is on the serial line, so it is read even after a probe that left
 * the console terminal in raw mode, as ttyprobe can. The host's time is
 * taken when clockmusl returns, a moment after it printed its epoch, and
 * the two must agree to 30 seconds: that catches a date wrong by hours, as
 * from a CMOS clock kept in local time and read as UTC, which clockmusl
 * cannot see from inside. tests/host/rtc_hosttest.c checks the arithmetic
 * of the kernel's clock exactly. QEMU is given `-rtc base=utc`, its
 * default, since the kernel reads the CMOS clock as UTC.
 */
int main(int argc, char **argv)
{
    const char *kernel = NULL, *initrd = NULL, *cpu = "max";
    const char *log = NULL, *append = NULL;
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
    int selftest_ok = 0;
    char selftest_detail[128];

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

    qemu[nq++] = (char *)qemu_binary();
    qemu[nq++] = (char *)"-cpu";
    qemu[nq++] = (char *)cpu;
    qemu[nq++] = (char *)"-kernel";
    qemu[nq++] = (char *)kernel;
    qemu[nq++] = (char *)"-initrd";
    qemu[nq++] = (char *)initrd;
    qemu[nq++] = (char *)"-serial";
    qemu[nq++] = (char *)"stdio";
    qemu[nq++] = (char *)"-display";
    qemu[nq++] = (char *)"none";
    qemu[nq++] = (char *)"-rtc";
    qemu[nq++] = (char *)"base=utc";
    qemu[nq++] = (char *)"-no-reboot";
    qemu[nq++] = (char *)"-netdev";
    qemu[nq++] = (char *)"user,id=net0";
    qemu[nq++] = (char *)"-device";
    qemu[nq++] = (char *)device;
    if (append != NULL) {
        qemu[nq++] = (char *)"-append";
        qemu[nq++] = (char *)append;
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
            fprintf(stderr,
                    "runtests: cannot write %s: %s; the run "
                    "continues without a transcript\n",
                    log, strerror(errno));
    }
    deadline_factor_read();
    timeout = scaled(timeout);
    steptmo = scaled(steptmo);
    boottmo = scaled(boottmo);
    deadline = now() + timeout;
    buf_add((const unsigned char *)"", 0);

    if (expect_text("Type 'help'", boottmo) < 0) {
        fprintf(stderr, "runtests: kernel shell never appeared\n");
        return 3;
    }

    for (i = 0; i < N_PROBES; i++) {
        size_t start = buf_len;
        char line[256];
        long end;

        if (not_built_here(PROBES[i])) {
            results[n_results].name = PROBES[i];
            snprintf(results[n_results].detail, sizeof(results[n_results].detail),
                     "i686 only, not run");
            results[n_results].ok = 1;
            n_results++;
            continue;
        }
        snprintf(line, sizeof(line), "exec %s", PROBES[i]);
        consumed = start;
        send_line(line);
        end = expect_marker(steptmo);
        if (end >= 0 && (size_t)end < start) {
            fprintf(stderr,
                    "runtests: the marker for %s ends before its "
                    "window begins; the scan is out of step\n",
                    PROBES[i]);
            return 2;
        }
        if (end < 0) {
            results[n_results].name = PROBES[i];
            snprintf(results[n_results].detail, sizeof(results[n_results].detail),
                     "no shell return (hang?)");
            results[n_results].ok = 0;
            n_results++;
            break;
        }
        results[n_results].name = PROBES[i];
        results[n_results].ok =
            score_window(buf + start, (size_t)end - start, results[n_results].detail,
                         sizeof(results[n_results].detail));
        n_results++;
        if (strcmp(PROBES[i], "musl/clockmusl") == 0) {
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

    snprintf(clock_detail, sizeof(clock_detail), "the probe printed no epoch");
    {
        const char *want = "CLOCKMUSL EPOCH ";
        size_t wlen = strlen(want), at;

        for (at = 0; at + wlen < buf_len; at++) {
            if (memcmp(buf + at, want, wlen) == 0) {
                const char *p = (const char *)buf + at + wlen;

                if (*p >= '0' && *p <= '9' && clock_seen_at != 0) {
                    long guest = strtol(p, NULL, 10);
                    double skew = clock_seen_at - (double)guest;

                    snprintf(clock_detail, sizeof(clock_detail), "%+d s vs the host", (int)skew);
                    clock_ok = skew >= -30 && skew <= 30;
                }
                break;
            }
        }
    }

    selftest_ok = score_kernel_timing(buf, buf_len, selftest_detail, sizeof(selftest_detail));

    for (i = 0; i < n_results; i++) {
        if (!results[i].ok)
            fails++;
        printf("  %-12s %-18s %s\n", results[i].name, results[i].detail,
               results[i].ok ? "PASS" : "FAIL");
    }

    /* Each probe a hang left unrun is a failure, and is named. */
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
                want = snprintf(names + used, sizeof(names) - used, "%s%s", used ? " " : "",
                                PROBES[i]);
                if (want > 0 && (size_t)want < sizeof(names) - used)
                    used += (size_t)want;
                else
                    used = sizeof(names) - 1;
            }
        }
        if (missing > 0) {
            fails += missing;
            printf("runtests: %d probe(s) never ran: %s\n", missing, names);
        }
    }

    /* The two checks count as failures but not as probes. */
    printf("  %-12s %-18s %s\n", "timing", selftest_detail, selftest_ok ? "PASS" : "FAIL");
    if (!selftest_ok)
        fails++;
    printf("  %-12s %-18s %s\n", "clock", clock_detail, clock_ok ? "PASS" : "FAIL");
    if (!clock_ok)
        fails++;

    printf("runtests: %d of %d probes, %d failures\n", n_results, N_PROBES, fails);
    return fails == 0 ? 0 : 1;
}
