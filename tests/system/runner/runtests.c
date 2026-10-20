/**
 * runtests: boot the test image once and run every probe of `PROBES`,
 * requiring each to exit 0.
 *
 * Usage: runtests --kernel K --initrd I [--cpu CPU] [--device DEV]
 *                 [--append LINE] [--log FILE] [--timeout S] [--steptmo S]
 *                 [--boottmo S] [--probe NAME]...
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
 * requires clockmusl to compare its date with a host UDP reference. --timeout
 * bounds the run, 180 seconds unless given, --steptmo each probe, 20, and
 * --boottmo the wait for the shell, 60, each multiplied by the set's
 * deadline factor. A probe that does not return within its limit ends the
 * run, and each probe after it counts as a failure. --log keeps the console
 * in FILE. Each --probe runs only the probes it names, in the order of
 * `PROBES`, as a mutation proof runs the probe its fault reaches; the clock
 * check then counts only when musl/clockmusl is named. The exit status is 0
 * when every probe and both checks pass, 1
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
#include <arpa/inet.h>
#include <sys/socket.h>
#include <time.h>
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
#include "../../clockref.h"

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
    /* execve of /proc/self/fd/N, which is glibc's fexecve without
       execveat. */
    "musl/fdexecmusl",
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
    /* pselect6's descriptor sets, and the signal mask it waits under. */
    "musl/pselectmusl",
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
    /* Paths that go on past /proc/PID/fd/N, cwd and root, and /dev/fd/N,
       from the file or directory each refers to. */
    "musl/proclinkmusl",
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

/* The probes --probe names, or none for every probe. */
static const char *chosen[N_PROBES];
static int n_chosen;

/** Say whether probe `name` runs: every probe does when none is chosen. */
static int runs(const char *name)
{
    int i;

    for (i = 0; i < n_chosen; i++)
        if (strcmp(chosen[i], name) == 0)
            return 1;
    return n_chosen == 0;
}

/** Choose probe `name` for --probe; 0, or -1 for a name `PROBES` lacks. */
static int choose(const char *name)
{
    int i, j;

    for (i = 0; i < N_PROBES; i++)
        if (strcmp(PROBES[i], name) == 0) {
            for (j = 0; j < n_chosen; j++)
                if (chosen[j] == PROBES[i])
                    return 0;
            chosen[n_chosen++] = PROBES[i];
            return 0;
        }
    return -1;
}

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

/** Require a complete reference result line inside the probe's own window. */
static int score_clock_reference(const unsigned char *window, size_t length)
{
    static const char result[] = "CLOCKMUSL REFERENCE OK";
    size_t at;

    for (at = 0; at + sizeof(result) <= length; at++) {
        size_t end;

        if (at != 0 && window[at - 1] != '\n')
            continue;
        if (memcmp(window + at, result, sizeof(result) - 1) != 0)
            continue;
        end = at + sizeof(result) - 1;
        if (end < length && window[end] == '\r')
            end++;
        if (end < length && window[end] == '\n')
            return 1;
    }
    return 0;
}

/* ---- the selftest ---- */

/** Check clock error intervals and complete reference records. */
static int selftest_clock(int *count)
{
    int bad = 0;
    static const struct {
        long long low, high, bound;
        int verdict;
    } intervals[] = {{-10, 10, 30, 1}, {40, 60, 30, 0},    {-60, -40, 30, 0},
                     {20, 40, 30, -1}, {-40, -20, 30, -1}, {-1000, 1000, 30, -1},
                     {5, -5, 30, -1},  {-30, 30, 30, 1}};
    static const struct {
        const char *text;
        int verdict;
    } references[] = {{"CLOCKMUSL REFERENCE OK\n", 1},
                      {"boot\nCLOCKMUSL REFERENCE OK\r\nexit\n", 1},
                      {"probe: CLOCKMUSL REFERENCE OK\n", 0},
                      {"CLOCKMUSL REFERENCE OK extra\n", 0},
                      {"CLOCKMUSL REFERENCE OK", 0}};
    size_t k;

    for (k = 0; k < sizeof(intervals) / sizeof(intervals[0]); k++) {
        if (clockref_judge(intervals[k].low, intervals[k].high, intervals[k].bound) !=
            intervals[k].verdict) {
            printf("runtests: selftest: clock interval %lu failed\n", (unsigned long)k);
            bad++;
        }
        (*count)++;
    }
    for (k = 0; k < sizeof(references) / sizeof(references[0]); k++) {
        if (score_clock_reference((const unsigned char *)references[k].text,
                                  strlen(references[k].text)) != references[k].verdict) {
            printf("runtests: selftest: clock record %lu failed\n", (unsigned long)k);
            bad++;
        }
        (*count)++;
    }
    return bad;
}

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
    int n = (int)(sizeof(cases) / sizeof(cases[0])), bad = 0, i;

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
    bad += selftest_clock(&n);
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
/* The private UDP reference socket, serviced while serial output is read. */
static int reference_fd = -1;
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

/**
 * Stop QEMU if it still runs: SIGTERM, then SIGKILL when it has not ended
 * 0.3 seconds later. `main` registers it with `atexit` once QEMU has
 * started, so no return from `main` and no `exit` leaves the machine behind.
 */
static void child_stop(void)
{
    if (child_pid <= 0 || child_poll() >= 0)
        return;
    kill(child_pid, SIGTERM);
    usleep(300000);
    if (child_poll() < 0) {
        kill(child_pid, SIGKILL);
        usleep(300000);
        child_poll();
    }
}

/** Answer one reference request with host monotonic and calendar time. */
static void reference_reply(void)
{
    struct sockaddr_in peer;
    socklen_t size = sizeof(peer);
    char packet[160], tail;
    unsigned sequence;
    struct timespec mono, real;
    ssize_t length =
        recvfrom(reference_fd, packet, sizeof(packet) - 1, 0, (struct sockaddr *)&peer, &size);

    if (length <= 0)
        return;
    packet[length] = '\0';
    if (sscanf(packet, "CLOCKREF %u %c", &sequence, &tail) != 1)
        return;
    if (clock_gettime(CLOCK_MONOTONIC, &mono) || clock_gettime(CLOCK_REALTIME, &real))
        return;
    length = snprintf(packet, sizeof(packet), "CLOCKREF %u %lld %lld", sequence,
                      (long long)mono.tv_sec * 1000000000LL + mono.tv_nsec,
                      (long long)real.tv_sec * 1000000000LL + real.tv_nsec);
    sendto(reference_fd, packet, (size_t)length, 0, (struct sockaddr *)&peer, size);
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
        FD_SET(reference_fd, &r);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        if (select((child_out > reference_fd ? child_out : reference_fd) + 1, &r, NULL, NULL, &tv) >
            0) {
            if (FD_ISSET(reference_fd, &r))
                reference_reply();
            if (!FD_ISSET(child_out, &r))
                continue;
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
 * supplied through a private UDP socket while serial output is read.
 * clockmusl brackets that independent timestamp with its clock reads and
 * judges the whole error interval. QEMU reads the CMOS clock as UTC.
 */
/* A probe's outcome in the run. */
struct result {
    const char *name;
    char detail[128];
    int ok;
};

/**
 * Name each probe that was to run and has no result in `results`, the
 * probes a hang left unrun, and give their count, which are failures.
 */
static int count_unrun(const struct result *results, int n_results)
{
    int missing = 0, i;
    char names[4096];
    size_t used = 0;

    names[0] = '\0';
    for (i = 0; i < N_PROBES; i++) {
        int ran = !runs(PROBES[i]), j, want;

        for (j = 0; j < n_results; j++)
            if (strcmp(results[j].name, PROBES[i]) == 0)
                ran = 1;
        if (ran)
            continue;
        missing++;
        want = snprintf(names + used, sizeof(names) - used, "%s%s", used ? " " : "", PROBES[i]);
        if (want > 0 && (size_t)want < sizeof(names) - used)
            used += (size_t)want;
        else
            used = sizeof(names) - 1;
    }
    if (missing > 0)
        printf("runtests: %d probe(s) never ran: %s\n", missing, names);
    return missing;
}

int main(int argc, char **argv)
{
    const char *kernel = NULL, *initrd = NULL, *cpu = "max";
    const char *log = NULL, *append = NULL;
    const char *device = "rtl8139,netdev=net0";
    double timeout = 180.0, steptmo = 20.0, boottmo = 60.0;
    char *qemu[32];
    int nq = 0, i;
    int out_pipe[2], in_pipe[2];
    struct result results[N_PROBES];
    int n_results = 0, fails = 0;
    int reference_port;
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
        else if (strcmp(argv[i], "--probe") == 0 && i + 1 < argc) {
            if (choose(argv[++i]) != 0) {
                fprintf(stderr, "runtests: no probe %s\n", argv[i]);
                return 2;
            }
        } else {
            fprintf(stderr, "runtests: unknown option %s\n", argv[i]);
            return 2;
        }
    }
    if (kernel == NULL || initrd == NULL) {
        fprintf(stderr, "runtests: --kernel and --initrd are required\n");
        return 2;
    }

    {
        struct sockaddr_in address;
        socklen_t size = sizeof(address);

        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        reference_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (reference_fd < 0 || bind(reference_fd, (struct sockaddr *)&address, size) ||
            getsockname(reference_fd, (struct sockaddr *)&address, &size)) {
            fprintf(stderr, "runtests: cannot bind the clock reference\n");
            return 2;
        }
        fcntl(reference_fd, F_SETFD, FD_CLOEXEC);
        reference_port = ntohs(address.sin_port);
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
    atexit(child_stop);
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

        if (!runs(PROBES[i]))
            continue;
        if (not_built_here(PROBES[i])) {
            results[n_results].name = PROBES[i];
            snprintf(results[n_results].detail, sizeof(results[n_results].detail),
                     "i686 only, not run");
            results[n_results].ok = 1;
            n_results++;
            continue;
        }
        if (strcmp(PROBES[i], "musl/clockmusl") == 0)
            snprintf(line, sizeof(line), "exec %s %d", PROBES[i], reference_port);
        else
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
            clock_ok = results[n_results - 1].ok &&
                       score_clock_reference(buf + start, (size_t)end - start);
        }
    }

    pump(now() + 1.0);
    child_stop();
    if (logf != NULL)
        fclose(logf);

    close(reference_fd);
    snprintf(clock_detail, sizeof(clock_detail), "%s",
             clock_ok ? "host reference" : "no valid reference result");

    selftest_ok = score_kernel_timing(buf, buf_len, selftest_detail, sizeof(selftest_detail));

    for (i = 0; i < n_results; i++) {
        if (!results[i].ok)
            fails++;
        printf("  %-12s %-18s %s\n", results[i].name, results[i].detail,
               results[i].ok ? "PASS" : "FAIL");
    }

    fails += count_unrun(results, n_results);

    /* The two checks count as failures but not as probes. */
    printf("  %-12s %-18s %s\n", "timing", selftest_detail, selftest_ok ? "PASS" : "FAIL");
    if (!selftest_ok)
        fails++;
    if (runs("musl/clockmusl")) {
        printf("  %-12s %-18s %s\n", "clock", clock_detail, clock_ok ? "PASS" : "FAIL");
        if (!clock_ok)
            fails++;
    }

    printf("runtests: %d of %d probes, %d failures\n", n_results,
           n_chosen != 0 ? n_chosen : N_PROBES, fails);
    return fails == 0 ? 0 : 1;
}
