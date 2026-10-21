/**
 * callsmusl: check calls that GNU libc programs make, through musl and by
 * number.
 *
 * sigtimedwait takes a blocked signal the process sent itself and returns its
 * number and sender, takes a signal sent to the thread before a lower one sent
 * to the process, waits for an alarm under a timeout of more than 2^31 ticks,
 * and with no signal pending its timeout ends it with EAGAIN. kill of a child
 * that has ended and is not yet reaped succeeds, alone and by its group, as on
 * Linux, and fails with ESRCH once it is reaped. A child that starts a session
 * and opens a free pseudo-terminal slave with O_NOCTTY does not take it, and
 * opening it again without the flag makes it the controlling terminal with the
 * child's group in the foreground, the Linux rule BusyBox's telnetd depends on.
 * getrusage reports the memory a process has touched as ru_maxrss in kilobytes,
 * and wait4 that of a child that touched more, as GNU time reads them. A UDP
 * socket connected to a local port no socket has polls POLLERR once the port
 * unreachable comes back, and its next receive is ECONNREFUSED, as glibc's
 * resolver expects, ten times in a row, since errors over the loopback are not
 * rate-limited. A POSIX timer's signal comes with SI_TIMER and its value, a
 * periodic timer's signal blocked through several periods comes once with the
 * others as its overrun, and a deleted timer's id is EINVAL. sigqueue gives its
 * value and SI_QUEUE to the signal it sends, and rt_sigqueueinfo its errno and
 * value. A writer to a raw pseudo-terminal slave whose master has not read
 * sleeps for room, as its state in /proc shows, and the master then reads
 * every byte of its 64 KiB; a non-blocking writer takes EAGAIN; two writers'
 * lines reach the master each whole, even when the first one's write waits
 * for room. A signalfd reads a blocked signal sent to the process, with its
 * sender, and a sigqueue's value, and with none pending a non-blocking read
 * is EAGAIN. A setgid directory gives a file
 * made in it its group, and a directory its group and the bit. A process
 * of another user may not open the maps or the descriptors
 * of a root process under /proc, and reads its own. Its /proc directory is
 * root's while a change of ids leaves it not dumpable, and its own once
 * PR_SET_DUMPABLE makes it so, as on Linux, and so is a descriptor open on its
 * maps. mlock, munlock, mlockall and munlockall accept a mapped range, and
 * mlock fails on a gap with ENOMEM; mincore reports a touched page resident.
 * getcpu reports a processor of the caller's affinity and node 0, and
 * personality keeps and returns its value. On x86-64 the ids 0xfffffffe read
 * back from getuid, geteuid, getgid and getegid unsigned, not as an error.
 * sendmmsg sends two datagrams in one call over an AF_UNIX pair and recvmmsg
 * receives both, each with its count; on x86-64 a count of 2^32 + 1 receives
 * one, and socket and socketpair of the domain 2^32 + 1 make AF_UNIX sockets,
 * since each takes an int. clock_gettime64 reads the monotonic clock with
 * 64-bit fields (clock_gettime on x86-64). close_range marks a range of
 * descriptors close-on-exec and closes it, reaches past the table's end, and
 * fails with EINVAL for a flag it does not take and for a backward range.
 * splice from a pipe to /dev/null, tee and sendfile answer EINVAL, which GNU
 * grep takes for a descriptor it must read instead, and EBADF for a closed one.
 * posix_fallocate gives a file in /dev/shm its pages, the size and the blocks
 * of the range, and answers EOPNOTSUPP for a file of KRFFS or of /proc open for
 * writing, after which glibc's writes the range itself, EBADF for one open only
 * for reading, EINVAL for a length of 0, ESPIPE for a pipe, and EOPNOTSUPP for
 * a mode Linux does not take before the check that the descriptor is open for
 * writing. adjtimex reads an unsynchronized clock with its 10 ms tick, and
 * clock_adjtime of CLOCK_MONOTONIC is EOPNOTSUPP. The extended attribute calls
 * find the file and answer as Linux does for a file system without attributes:
 * an empty list and EOPNOTSUPP to get one, and ENOENT for a missing file.
 * readahead and posix_fadvise accept a regular file, and posix_fadvise fails on
 * a pipe with ESPIPE. setitimer's ITIMER_VIRTUAL and ITIMER_PROF post SIGVTALRM
 * and SIGPROF as the process spins, and getitimer reports the interval; so do
 * POSIX timers on the process's and the thread's clocks of processor time.
 * /dev/mem reads the first megabyte and maps the local APIC, and fails with
 * EPERM for RAM and with EINVAL for a private mapping. klogctl gives the log
 * ring's size, its records whole, and after a clear none of those before;
 * /dev/log leads to /var/run/log. The clock's wake alarm fails with ETIME for a
 * time past and EINVAL for one two days ahead, ends a read at the alarm, and
 * wakes a suspend to idle that gives no process time meanwhile.
 *
 * It prints "CALLSMUSL OK" and exits 0 on success, or prints "CALLSMUSL FAIL
 * (detail: errno N)" and exits 1 at the first failure.
 *
 * The setgid directory's group is 4242, the other user is 4243 and the closed
 * port is 47, or the NEW of each in `ids=OLD:NEW,...` and `ports=OLD:NEW,...`
 * (tests/native/Readme.md). Given `machine=native`, it leaves out the
 * adjtimex row, since the machine's NTP client has already adjusted the clock
 * that row requires unsynchronized.
 *
 * etude's report (probe_report.h), for the case ETUDE_CASE names, has an
 * assertion for each row in order, a setup step joined to the row after it:
 * `sigtimedwait-pending`, `sigtimedwait-eagain`, `sigtimedwait-thread-first`,
 * `sigtimedwait-far`, `zombie-kill`, `zombie-esrch`, `mlock`, `mlockall`,
 * `mlock-gap`, `mincore`, `getcpu`, `personality`, `ids-unsigned`, `sendmmsg`,
 * `recvmmsg`, `recvmmsg-wide-count`, `socket-wide-domain`, `clock-gettime64`,
 * `time-call`, `close-range-cloexec`, `close-range-below`,
 * `close-range-einval-flag`, `close-range-backward`, `close-range-end`,
 * `splice-einval`, `tee-einval`, `sendfile-einval`, `splice-ebadf`,
 * `fallocate-krffs`, `fallocate-ebadf`, `fallocate-einval`, `fallocate-espipe`,
 * `fallocate-mode`, `fallocate-proc`, `fallocate-shm`, `adjtimex`,
 * `clock-adjtime`, `listxattr`, `getxattr`, `lsetxattr-enoent`,
 * `readahead-fadvise`, `fadvise-espipe`, `noctty-session`, `maxrss-self`,
 * `maxrss-child`, `udp-refused`, `setgid-file`, `setgid-dir`,
 * `privacy-other-root`, `privacy-own-maps`, `privacy-dumpable`,
 * `timer-si-timer`, `timer-overrun`, `timer-delete`, `sigqueue`,
 * `rt-sigqueueinfo`, `pty-nonblock-eagain`, `pty-writer-sleeps`, `pty-64k`,
 * `pty-whole-lines`, `signalfd-eagain`, `signalfd-read`, `itimer-interval`,
 * `itimer-signals`, `ptimer-gettime`, `ptimer-signals`, `devmem-low`,
 * `devmem-ram-eperm`, `devmem-private-einval`, `devmem-apic`, `klog-size`,
 * `klog-records`, `klog-clear`, `dev-log-link`, `rtc-alarm-refused`,
 * `rtc-alarm-readback`, `rtc-read-af`, `rtc-wakeup-enabled`,
 * `power-state-mode`, `freeze-wakes` and `freeze-holds`. `ids-unsigned`,
 * `recvmmsg-wide-count` and `socket-wide-domain` are x86-64's alone, and the
 * branch of the group `arch`, `i686` or `x86-64`, says whether they ran. A
 * native run reports `adjtimex` not run. The first failure fails its assertion
 * and leaves the rest not run.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <dirent.h>
#include <poll.h>
#include <sched.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/timex.h>
#include <sys/xattr.h>
#include <termios.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../probe_args.h"
#include "../probe_report.h"

/*
 * The calls of 64-bit fields and offsets, by their i386 names: i386's
 * clock_gettime64, call 403, and sendfile64 are x86-64's clock_gettime and
 * sendfile, whose fields and offsets are 64 bits wide.
 */
#if defined __x86_64__
#define SYS_CLOCK_GETTIME64 SYS_clock_gettime
#define SYS_sendfile64      SYS_sendfile
#else
#define SYS_CLOCK_GETTIME64 403
#endif

/* The longest wait, in seconds, for another process to be seen asleep: a
 * bound that ends a wait the kernel never satisfies, far past a correct one. */
#define ASLEEP_BOUND 20

/* The bytes of the line a child writes in `whole_writes`, its newline
 * included: more than the master ring holds, so that the write waits for
 * room part way through, and fewer than twice what the ring takes of it, so
 * that one read of the ring makes room for the rest. */
#define WHOLE_LINE 6000

/* The rounds of `whole_writes`. A timer tick between the caller's read and
 * its write may switch to the child, which then finishes its line first, and
 * such a round passes even when the kernel cuts a waiting write; the caller
 * writes first in the other rounds. */
#define WHOLE_ROUNDS 4

/* The assertions of etude's report in the order they are checked, more than
 * probe_report.h's path holds, and how many are planned and reported. */
static const char *rows[96];
static int rows_planned, rows_reported;

/** Add the assertion `id` to the end of the planned rows. */
static void plan_row(const char *id)
{
    rows[rows_planned++] = id;
}

/** Report that the next planned row held. */
static void held(void)
{
    report_pass(rows[rows_reported++]);
}

/** In the process that began the report, report that the next planned row
 * failed with `why`, that those after it were not run, and the report's
 * end; after the end, nothing. */
static void stop(const char *why)
{
    if (report_owner == 0 || report_ended || report_getpid() != report_owner) {
        return;
    }
    if (rows_reported < rows_planned) {
        report_fail(rows[rows_reported++], why);
    }
    while (rows_reported < rows_planned) {
        report_notrun(rows[rows_reported++], "an earlier check failed");
    }
    report_end();
}

/** Report the planned rows before the one at index `row` held. */
static void held_to(int row)
{
    while (rows_reported < row && rows_reported < rows_planned) {
        held();
    }
}

/** Report one failed check with errno and return 1, the failure status. */
static int fail(const char *what)
{
    char why[200];

    snprintf(why, sizeof(why), "%s: errno %d", what, errno);
    printf("CALLSMUSL FAIL (%s)\n", why);
    stop(why);
    return 1;
}

/**
 * Write a byte to each page of the `n` bytes at `p`, through a volatile
 * pointer so the compiler cannot drop stores nothing reads.
 */
static void touch(char *p, long n)
{
    volatile char *v = p;
    long i;

    for (i = 0; i < n; i += 4096) {
        v[i] = 1;
    }
}

/**
 * Check ru_maxrss, in kilobytes, of the caller and of a child it waits for.
 * Returns 0, or 1 after the failure is printed.
 */
static int peaks(void)
{
    struct rusage ru;
    char *mine = malloc(4 << 20);
    int status;
    pid_t pid;

    if (!mine) {
        return fail("4 MiB to touch");
    }
    touch(mine, 4 << 20);
    if (getrusage(RUSAGE_SELF, &ru) != 0 || ru.ru_maxrss < 4096) {
        return fail("getrusage did not report the 4 MiB touched as ru_maxrss");
    }
    held();
    pid = fork();
    if (pid == 0) {
        char *more = malloc(12 << 20);

        if (more) {
            touch(more, 12 << 20);
        }
        _exit(more ? 0 : 1);
    }
    if (pid < 0 || wait4(pid, &status, 0, &ru) != pid || ru.ru_maxrss < 12288) {
        return fail("wait4 did not report the child's 12 MiB as ru_maxrss");
    }
    held();
    free(mine);
    return 0;
}

/**
 * Check ten datagrams to a closed local port, each answered with a port
 * unreachable, since errors over the loopback are not rate-limited: each polls
 * POLLERR and its receive fails with ECONNREFUSED. Returns 0, or 1 after the
 * failure is printed.
 */
static int refusals(void)
{
    struct sockaddr_in a;
    struct pollfd p;
    char c;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int i;

    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)probe_port(47));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
        return fail("a UDP socket connected to a closed local port");
    }
    for (i = 0; i < 10; i++) {
        if (send(fd, "x", 1, 0) != 1) {
            return fail("a send to a closed local port");
        }
        p.fd = fd;
        p.events = POLLIN;
        if (poll(&p, 1, 1000) != 1 || !(p.revents & POLLERR)) {
            return fail("poll did not report the port unreachable as POLLERR");
        }
        if (recv(fd, &c, 1, 0) != -1 || errno != ECONNREFUSED) {
            return fail("the receive after a port unreachable was not ECONNREFUSED");
        }
    }
    held();
    close(fd);
    return 0;
}

/**
 * Check POSIX timers on SIGUSR2, which stays blocked and is taken by
 * sigtimedwait. Returns 0, or 1 after the failure is printed.
 */
static int timers(void)
{
    struct sigevent se;
    struct itimerspec its;
    struct timespec wait = {2, 0}, zero = {0, 0};
    sigset_t set;
    siginfo_t si;
    timer_t t;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, NULL);
    memset(&se, 0, sizeof se);
    se.sigev_notify = SIGEV_SIGNAL;
    se.sigev_signo = SIGUSR2;
    se.sigev_value.sival_int = 99;
    memset(&its, 0, sizeof its);
    its.it_value.tv_nsec = 20000000;
    if (timer_create(CLOCK_MONOTONIC, &se, &t) != 0 || timer_settime(t, 0, &its, NULL) != 0) {
        return fail("timer_create and timer_settime of a 20 ms timer");
    }
    if (sigtimedwait(&set, &si, &wait) != SIGUSR2 || si.si_code != SI_TIMER ||
        si.si_value.sival_int != 99) {
        return fail("the timer's signal did not come with SI_TIMER and its value");
    }
    held();
    its.it_interval.tv_nsec = 20000000;
    if (timer_settime(t, 0, &its, NULL) != 0) {
        return fail("timer_settime of a periodic timer");
    }
    usleep(210000);
    if (sigtimedwait(&set, &si, &wait) != SIGUSR2 || si.si_overrun < 3 || timer_getoverrun(t) < 3) {
        return fail("a blocked periodic timer's signal did not carry its overrun");
    }
    held();
    if (timer_delete(t) != 0 || syscall(SYS_timer_delete, (long)t) != -1 || errno != EINVAL) {
        return fail("timer_delete, then again");
    }
    held();
    while (sigtimedwait(&set, &si, &zero) == SIGUSR2) {
    }
    return 0;
}

/**
 * Check sigqueue and rt_sigqueueinfo to the caller's own process, taken by
 * sigtimedwait. Returns 0, or 1 after the failure is printed.
 */
static int queues(void)
{
    struct timespec wait = {2, 0};
    union sigval v;
    sigset_t set;
    siginfo_t si;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, NULL);
    v.sival_int = 4242;
    if (sigqueue(getpid(), SIGUSR2, v) != 0) {
        return fail("sigqueue to the caller's own process");
    }
    if (sigtimedwait(&set, &si, &wait) != SIGUSR2 || si.si_code != SI_QUEUE ||
        si.si_value.sival_int != 4242 || si.si_pid != getpid()) {
        return fail("sigqueue's signal did not come with SI_QUEUE, its value and its sender");
    }
    held();
    memset(&si, 0, sizeof(si));
    si.si_signo = SIGUSR2;
    si.si_errno = 77;
    si.si_code = SI_QUEUE;
    si.si_pid = getpid();
    si.si_uid = getuid();
    si.si_value.sival_int = 4343;
    if (syscall(SYS_rt_sigqueueinfo, getpid(), SIGUSR2, &si) != 0) {
        return fail("rt_sigqueueinfo to the caller's own process");
    }
    memset(&si, 0, sizeof(si));
    if (sigtimedwait(&set, &si, &wait) != SIGUSR2 || si.si_errno != 77 ||
        si.si_value.sival_int != 4343) {
        return fail("rt_sigqueueinfo's signal did not come with its errno and value");
    }
    held();
    return 0;
}

/**
 * Check a non-blocking signalfd of SIGUSR2, which stays blocked. Returns 0, or
 * 1 after the failure is printed.
 */
static int signalfds(void)
{
    struct signalfd_siginfo si;
    union sigval v;
    sigset_t set;
    int fd;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, NULL);
    fd = signalfd(-1, &set, SFD_NONBLOCK);
    if (fd < 0 || read(fd, &si, sizeof si) != -1 || errno != EAGAIN) {
        return fail("a signalfd with nothing pending did not read EAGAIN");
    }
    held();
    v.sival_int = 31;
    if (kill(getpid(), SIGUSR2) != 0 || read(fd, &si, sizeof si) != (int)sizeof si ||
        si.ssi_signo != SIGUSR2 || si.ssi_pid != (uint32_t)getpid() ||
        sigqueue(getpid(), SIGUSR2, v) != 0 || read(fd, &si, sizeof si) != (int)sizeof si ||
        si.ssi_int != 31) {
        return fail("a signalfd did not read the signal sent, its sender and sigqueue's value");
    }
    held();
    close(fd);
    return 0;
}

/**
 * Check the group of a file and a directory made in a setgid directory of
 * group 4242. Returns 0, or 1 after the failure is printed.
 */
static int setgids(void)
{
    const char *dir = "/tmp/callsmusl.sg";
    gid_t group = (gid_t)probe_id(4242);
    struct stat st;
    int fd;

    unlink("/tmp/callsmusl.sg/f");
    rmdir("/tmp/callsmusl.sg/d");
    rmdir(dir);
    if (mkdir(dir, 0775) != 0 || chown(dir, (uid_t)-1, group) != 0 || chmod(dir, 02775) != 0) {
        return fail("a setgid directory of group 4242 in /tmp");
    }
    fd = open("/tmp/callsmusl.sg/f", O_CREAT | O_WRONLY, 0644);
    if (fd < 0 || close(fd) != 0 || stat("/tmp/callsmusl.sg/f", &st) != 0 || st.st_gid != group) {
        return fail("a file made in a setgid directory did not take its group");
    }
    held();
    if (mkdir("/tmp/callsmusl.sg/d", 0755) != 0 || stat("/tmp/callsmusl.sg/d", &st) != 0 ||
        st.st_gid != group || !(st.st_mode & S_ISGID)) {
        return fail("a directory made in a setgid directory did not take its group and the bit");
    }
    held();
    unlink("/tmp/callsmusl.sg/f");
    rmdir("/tmp/callsmusl.sg/d");
    rmdir(dir);
    return 0;
}

/**
 * Run in a child of root: as user 4243, open the root process `parent`'s maps
 * and fd directory and read a link in that directory, which fail with EACCES,
 * open the caller's own maps, and check the owner of its /proc directory
 * before and after PR_SET_DUMPABLE. Returns 0, or the number of the first
 * step that went wrong.
 */
static int privacies_child(pid_t parent)
{
    char path[64], link[64];
    uid_t user = (uid_t)probe_id(4243);
    struct stat st;
    DIR *d;
    int fd;

    if (setuid(user) != 0) {
        return 1;
    }
    snprintf(path, sizeof path, "/proc/%d/maps", (int)parent);
    if (open(path, O_RDONLY) != -1 || errno != EACCES) {
        return 2;
    }
    snprintf(path, sizeof path, "/proc/%d/fd", (int)parent);
    d = opendir(path);
    if (d || errno != EACCES) {
        return 3;
    }
    snprintf(path, sizeof path, "/proc/%d/fd/1", (int)parent);
    if (readlink(path, link, sizeof link) != -1 || errno != EACCES) {
        return 3;
    }
    fd = open("/proc/self/maps", O_RDONLY);
    if (fd < 0) {
        return 4;
    }
    if (stat("/proc/self/", &st) != 0 || st.st_uid != 0 ||
        prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0 || stat("/proc/self/", &st) != 0 ||
        st.st_uid != user || stat("/proc/self/status", &st) != 0 || st.st_uid != user ||
        fstat(fd, &st) != 0 || st.st_uid != user) {
        return 5;
    }
    return 0;
}

/**
 * Run `privacies_child` in a child and judge its exit status, which also says
 * which of the three rows of etude's report failed: another user's opens, the
 * child's own maps, and its /proc directory. Returns 0, or 1 after the
 * failure is printed.
 */
static int privacies(void)
{
    int status;
    int base = rows_reported;
    pid_t pid, parent = getpid();

    pid = fork();
    if (pid == 0) {
        _exit(privacies_child(parent));
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return fail("a child of user 4243");
    }
    if (WEXITSTATUS(status) == 2 || WEXITSTATUS(status) == 3) {
        return fail("another user opened a root process's maps or fd directory, or read "
                    "one of its descriptors' links");
    }
    if (WEXITSTATUS(status) == 5) {
        held_to(base + 2);
        return fail("the /proc directory of a dumpable process of user 4243 was not its own");
    }
    if (WEXITSTATUS(status) != 0) {
        held_to(base + (WEXITSTATUS(status) == 4 ? 1 : 0));
        return fail("a child of user 4243 could not become it or read its own maps");
    }
    held_to(base + 3);
    return 0;
}

/** The state letter /proc/<pid>/stat gives process `pid`, or 0 when it has
 * no stat file. */
static int proc_state(pid_t pid)
{
    char path[64], line[256], *state;
    int fd, n;

    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    n = (int)read(fd, line, sizeof line - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    line[n] = '\0';
    state = strrchr(line, ')');
    return state && state[1] == ' ' ? state[2] : 0;
}

/**
 * Wait for process `pid` to be asleep, state S in /proc/<pid>/stat, looking
 * every 10 ms. Returns 1 once it is, and 0 when it ends first, as a zombie or
 * gone from /proc, or when ASLEEP_BOUND seconds pass.
 */
static int seen_asleep(pid_t pid)
{
    struct timespec start, now;
    int state;

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        state = proc_state(pid);
        if (state == 'S') {
            return 1;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (state == 0 || state == 'Z' || now.tv_sec - start.tv_sec >= ASLEEP_BOUND) {
            return 0;
        }
        usleep(10000);
    }
}

/**
 * One round of `whole_writes` on the slave descriptor `slave` and the master
 * `master`: a child writes `line`, WHOLE_LINE bytes, and once it is seen
 * asleep for room the caller reads what the ring holds and at once writes its
 * own line. The master must then read `want`, the child's line whole and the
 * caller's after it. Returns 0, or 1 after the failure is printed.
 */
static int whole_round(int master, int slave, const char *line, const char *want)
{
    char got[WHOLE_LINE + 10];
    int n, have = 0, status;
    pid_t pid = fork();

    if (pid == 0) {
        _exit(write(slave, line, WHOLE_LINE) == WHOLE_LINE ? 0 : 1);
    }
    if (pid < 0) {
        return fail("a child to write a line to the slave");
    }
    if (!seen_asleep(pid) || (have = (int)read(master, got, sizeof got)) <= 0 ||
        have >= WHOLE_LINE || 2 * have <= WHOLE_LINE) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return fail("a slave write of a line longer than the master ring was not seen "
                    "waiting for room with over half of it, and not all, in the ring");
    }
    if (write(slave, "bbbbbbb\n", 8) != 8) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return fail("a second writer's line to the slave was not written");
    }
    while (have < (int)sizeof got &&
           (n = (int)read(master, got + have, sizeof got - (size_t)have)) > 0) {
        have += n;
    }
    if (have != (int)sizeof got) {
        kill(pid, SIGKILL);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        have != (int)sizeof got || memcmp(got, want, sizeof got) != 0) {
        return fail("two writers' lines to one slave were mixed");
    }
    return 0;
}

/**
 * Check that a slave write that waits for room in the master ring is not cut
 * by another writer's, as Linux's terminal write lock keeps it whole.
 *
 * In each of WHOLE_ROUNDS rounds (`whole_round`) a child writes a line of
 * 'a' and a newline to the slave `name` through output processing, which
 * makes the newline two bytes. Once the child is seen asleep for room, the
 * caller reads what the ring holds, which makes room for the rest of the
 * line, and at once writes its own line of 'b' to the slave. The caller's
 * write must wait for the child's, so the master `master` reads the child's
 * line whole and the caller's after it. The caller keeps itself and its
 * children to the processor it runs on, so that a child goes on only when
 * the caller sleeps or a timer tick switches to it, and restores its
 * affinity at the end. Returns 0, or 1 after the failure is printed.
 */
static int whole_writes(int master, const char *name)
{
    char line[WHOLE_LINE], want[WHOLE_LINE + 10];
    int slave = open(name, O_RDWR | O_NOCTTY);
    int round, status = 0;
    unsigned cpu;
    cpu_set_t mask, one;
    struct termios t;

    if (slave < 0 || tcgetattr(slave, &t) != 0) {
        return fail("a pseudo-terminal slave for two writers");
    }
    if (sched_getaffinity(0, sizeof mask, &mask) != 0 ||
        syscall(SYS_getcpu, &cpu, NULL, NULL) != 0 || cpu >= CPU_SETSIZE) {
        return fail("the caller's processor and affinity");
    }
    CPU_ZERO(&one);
    CPU_SET(cpu, &one);
    if (sched_setaffinity(0, sizeof one, &one) != 0) {
        return fail("an affinity of the caller's processor alone");
    }
    t.c_oflag |= OPOST | ONLCR;
    tcsetattr(slave, TCSANOW, &t);
    memset(line, 'a', sizeof line - 1);
    line[sizeof line - 1] = '\n';
    memcpy(want, line, sizeof line - 1);
    memcpy(want + sizeof line - 1, "\r\nbbbbbbb\r\n", 11);
    for (round = 0; round < WHOLE_ROUNDS && status == 0; round++) {
        status = whole_round(master, slave, line, want);
    }
    close(slave);
    if (sched_setaffinity(0, sizeof mask, &mask) != 0 && status == 0) {
        return fail("the caller's affinity restored");
    }
    return status;
}

/**
 * Check a slave written past what the master ring holds: a non-blocking
 * write is cut and then EAGAIN, and a child's 64 KiB, written while the
 * master does not read until the child is seen asleep for room, all reach
 * the master; a line whose write waits for room reaches it whole, before
 * another writer's. Returns 0, or 1 after the failure is printed.
 */
static int flows(void)
{
    char name[32], buf[4096];
    struct termios t;
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    int slave, status, n, cut, total = 0;
    pid_t pid;

    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 ||
        ptsname_r(master, name, sizeof name) != 0 ||
        (slave = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK)) < 0 || tcgetattr(slave, &t) != 0) {
        return fail("a raw pseudo-terminal pair");
    }
    cfmakeraw(&t);
    tcsetattr(slave, TCSANOW, &t);
    memset(buf, 'x', sizeof buf);
    cut = (int)write(slave, buf, sizeof buf);
    if (cut <= 0 || write(slave, buf, sizeof buf) != -1 || errno != EAGAIN) {
        return fail("a non-blocking slave write past the master ring was not cut and then EAGAIN");
    }
    held();
    while (cut > 0 && (n = read(master, buf, sizeof buf)) > 0) {
        cut -= n;
    }
    pid = fork();
    if (pid == 0) {
        int i;

        fcntl(slave, F_SETFL, 0);
        for (i = 0; i < 16; i++) {
            if (write(slave, buf, sizeof buf) != (int)sizeof buf) {
                _exit(1);
            }
        }
        _exit(0);
    }
    close(slave);
    if (pid > 0 && !seen_asleep(pid)) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        return fail("a slave writer past the master ring was never seen asleep for room");
    }
    held();
    while (total < 16 * (int)sizeof buf && (n = read(master, buf, sizeof buf)) > 0) {
        total += n;
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0 || total != 16 * (int)sizeof buf) {
        return fail("a slave writer's 64 KiB did not all reach the master");
    }
    held();
    if (whole_writes(master, name) != 0) {
        return 1;
    }
    held();
    close(master);
    return 0;
}

/**
 * Check that a session leader does not take a free slave it opens with
 * O_NOCTTY, and takes it when it opens it again without the flag.
 * Returns 0, or 1 after the failure is printed.
 */
static int terminals(void)
{
    char name[32];
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    int status;
    pid_t pid;

    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 ||
        ptsname_r(master, name, sizeof name) != 0) {
        return fail("a pseudo-terminal pair");
    }
    pid = fork();
    if (pid == 0) {
        int a, b;

        setsid();
        a = open(name, O_RDWR | O_NOCTTY);
        if (a < 0 || tcgetpgrp(a) == getpid()) {
            _exit(1);
        }
        b = open(name, O_RDWR);
        _exit(b >= 0 && tcgetpgrp(b) == getpid() ? 0 : 2);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail("a session leader did not take the slave it opened");
    }
    held();
    close(master);
    return 0;
}

/**
 * Check that a child that has ended is a target of kill, alone and by its
 * group, until it is reaped, and ESRCH after. Returns 0, or 1 after the failure
 * is printed.
 */
static int zombies(void)
{
    siginfo_t info;
    pid_t pid = fork();

    if (pid == 0) {
        setpgid(0, 0);
        _exit(0);
    }
    if (pid < 0 || waitid(P_PID, (id_t)pid, &info, WEXITED | WNOWAIT) != 0) {
        return fail("a child that ends, waited for and left unreaped");
    }
    if (kill(pid, 0) != 0 || kill(pid, SIGTERM) != 0 || kill(-pid, SIGTERM) != 0) {
        return fail("kill of a child not yet reaped did not succeed");
    }
    held();
    if (waitpid(pid, NULL, 0) != pid || kill(pid, 0) != -1 || errno != ESRCH) {
        return fail("kill of a reaped child was not ESRCH");
    }
    held();
    return 0;
}

/**
 * Check sigtimedwait: a pending signal and its sender, EAGAIN with none, a
 * thread's signal before a lower one of the process, and a timeout past 2^31
 * ticks; then the rows of `zombies`. Returns 0, or 1 after the failure is
 * printed.
 */
static int signals(void)
{
    sigset_t set;
    siginfo_t info;
    struct timespec zero = {0, 0};
    struct timespec far = {21474836, 500000000};

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &set, NULL) != 0 || kill(getpid(), SIGUSR1) != 0) {
        return fail("blocking and sending SIGUSR1");
    }
    if (sigtimedwait(&set, &info, &zero) != SIGUSR1 || info.si_signo != SIGUSR1 ||
        info.si_pid != getpid()) {
        return fail("sigtimedwait did not take the pending signal");
    }
    held();
    errno = 0;
    if (sigtimedwait(&set, &info, &zero) != -1 || errno != EAGAIN) {
        return fail("sigtimedwait with nothing pending");
    }
    held();
    sigaddset(&set, SIGUSR2);
    if (sigprocmask(SIG_BLOCK, &set, NULL) != 0 || kill(getpid(), SIGUSR1) != 0 ||
        syscall(SYS_tkill, syscall(SYS_gettid), SIGUSR2) != 0) {
        return fail("sending SIGUSR1 to the process and SIGUSR2 to the thread");
    }
    if (sigtimedwait(&set, &info, &zero) != SIGUSR2 || info.si_code != SI_TKILL ||
        sigtimedwait(&set, &info, &zero) != SIGUSR1 || info.si_code != SI_USER) {
        return fail("sigtimedwait did not take the thread's signal before the process's");
    }
    held();
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    if (sigprocmask(SIG_BLOCK, &set, NULL) != 0) {
        return fail("blocking SIGALRM");
    }
    alarm(1);
    if (sigtimedwait(&set, &info, &far) != SIGALRM) {
        return fail("sigtimedwait under a timeout past 2^31 ticks did not wait for the alarm");
    }
    held();
    return zombies();
}

/**
 * Check mlock, munlock, mlockall and munlockall on a mapped range, mlock across
 * a gap, and mincore of a touched page. Returns 0, or 1 after the failure is
 * printed.
 */
static int memory(void)
{
    long page = sysconf(_SC_PAGESIZE);
    unsigned char vec[2];
    char *p = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (p == MAP_FAILED) {
        return fail("mmap");
    }
    if (mlock(p, 2 * page) != 0 || munlock(p, 2 * page) != 0) {
        return fail("mlock of a mapped range");
    }
    held();
    if (mlockall(MCL_CURRENT) != 0 || munlockall() != 0) {
        return fail("mlockall");
    }
    held();
    munmap(p + page, page);
    errno = 0;
    if (mlock(p, 2 * page) != -1 || errno != ENOMEM) {
        return fail("mlock across a gap");
    }
    held();
    p[0] = 1;
    if (mincore(p, page, vec) != 0 || !(vec[0] & 1)) {
        return fail("mincore of a touched page");
    }
    held();
    munmap(p, page);
    return 0;
}

/**
 * Check getcpu and personality, and on x86-64 the ids 0xfffffffe read back
 * unsigned. Returns 0, or 1 after the failure is printed.
 */
static int identity(void)
{
    unsigned cpu = 99, node = 7;
    cpu_set_t set;
    long old;

    if (sched_getaffinity(0, sizeof(set), &set) != 0 ||
        syscall(SYS_getcpu, &cpu, &node, NULL) != 0 || cpu >= CPU_SETSIZE ||
        !CPU_ISSET(cpu, &set) || node != 0) {
        return fail("getcpu");
    }
    held();
    old = syscall(SYS_personality, 0xffffffffUL);
    if (old < 0 || syscall(SYS_personality, 0x0040000UL) != old ||
        syscall(SYS_personality, 0xffffffffUL) != 0x0040000L ||
        syscall(SYS_personality, (unsigned long)old) != 0x0040000L) {
        return fail("personality");
    }
    held();
#if defined __x86_64__
    {
        int status;
        pid_t pid = fork();

        if (pid == 0) {
            _exit(setresgid(0xfffffffeU, 0xfffffffeU, 0xfffffffeU) != 0 ||
                          setresuid(0xfffffffeU, 0xfffffffeU, 0xfffffffeU) != 0 ||
                          syscall(SYS_getuid) != 0xfffffffeL ||
                          syscall(SYS_geteuid) != 0xfffffffeL ||
                          syscall(SYS_getgid) != 0xfffffffeL || syscall(SYS_getegid) != 0xfffffffeL
                      ? 1
                      : 0);
        }
        if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
            return fail("an id past 2^31 read back signed");
        }
        held();
    }
#endif
    return 0;
}

/**
 * Check sendmmsg and recvmmsg over an AF_UNIX pair, and on x86-64 a count and a
 * domain past 32 bits, of which the kernel takes the low word. Returns 0, or 1
 * after the failure is printed.
 */
static int messages(void)
{
    int sv[2];
    char a[] = "one", b[] = "second", ra[16], rb[16];
    struct iovec iov[2], riov[2];
    struct mmsghdr out[2], in[2];

    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) {
        return fail("socketpair");
    }
    memset(out, 0, sizeof(out));
    memset(in, 0, sizeof(in));
    iov[0].iov_base = a;
    iov[0].iov_len = sizeof(a);
    iov[1].iov_base = b;
    iov[1].iov_len = sizeof(b);
    riov[0].iov_base = ra;
    riov[0].iov_len = sizeof(ra);
    riov[1].iov_base = rb;
    riov[1].iov_len = sizeof(rb);
    out[0].msg_hdr.msg_iov = &iov[0];
    out[0].msg_hdr.msg_iovlen = 1;
    out[1].msg_hdr.msg_iov = &iov[1];
    out[1].msg_hdr.msg_iovlen = 1;
    in[0].msg_hdr.msg_iov = &riov[0];
    in[0].msg_hdr.msg_iovlen = 1;
    in[1].msg_hdr.msg_iov = &riov[1];
    in[1].msg_hdr.msg_iovlen = 1;
    if (sendmmsg(sv[0], out, 2, 0) != 2 || out[0].msg_len != sizeof(a) ||
        out[1].msg_len != sizeof(b)) {
        return fail("sendmmsg");
    }
    held();
    if (recvmmsg(sv[1], in, 2, 0, NULL) != 2 || in[0].msg_len != sizeof(a) ||
        in[1].msg_len != sizeof(b) || strcmp(ra, a) != 0 || strcmp(rb, b) != 0) {
        return fail("recvmmsg");
    }
    held();
#if defined __x86_64__
    {
        int pair[2];
        long fd;

        in[0].msg_len = in[1].msg_len = 0;
        if (sendmmsg(sv[0], out, 2, 0) != 2 ||
            syscall(SYS_recvmmsg, sv[1], in, 0x100000001UL, 0, NULL) != 1 ||
            in[0].msg_len != sizeof(a) || in[1].msg_len != 0 ||
            recv(sv[1], rb, sizeof(rb), 0) != sizeof(b)) {
            return fail("recvmmsg of a count past 32 bits did not take its low word");
        }
        held();
        fd = syscall(SYS_socket, 0x100000001UL, SOCK_STREAM, 0);
        if (fd < 0 || syscall(SYS_socketpair, 0x100000001UL, SOCK_STREAM, 0, pair) != 0) {
            return fail("a domain past 32 bits did not take its low word");
        }
        held();
        close((int)fd);
        close(pair[0]);
        close(pair[1]);
    }
#endif
    close(sv[0]);
    close(sv[1]);
    return 0;
}

/**
 * Check clock_gettime64 of the monotonic clock, and the raw time call, which
 * glibc makes on x86-64: it gives real time's seconds, between two readings
 * of the clock taken around it whatever the process waited, and stores them
 * as well. Returns 0, or 1 after the failure is printed.
 */
static int clocks(void)
{
    int64_t t[2] = {-1, -1};
    struct timespec before, after;
    long stored = -1, got;

    if (syscall(SYS_CLOCK_GETTIME64, CLOCK_MONOTONIC, t) != 0 || t[0] < 0 || t[1] < 0 ||
        t[1] >= 1000000000) {
        return fail("clock_gettime64");
    }
    held();
    if (clock_gettime(CLOCK_REALTIME, &before) != 0) {
        return fail("clock_gettime before time");
    }
    got = syscall(SYS_time, &stored);
    if (clock_gettime(CLOCK_REALTIME, &after) != 0) {
        return fail("clock_gettime after time");
    }
    if (got < (long)before.tv_sec || got > (long)after.tv_sec || stored != got) {
        return fail("time");
    }
    held();
    return 0;
}

/**
 * Check close_range, call 436 on both architectures: CLOSE_RANGE_CLOEXEC, an
 * unknown flag, a backward range, and a range to the end of the table.
 * Returns 0, or 1 after the failure is printed.
 */
static int ranges(void)
{
    int fd[3];
    int flags;
    int i;

    for (i = 0; i < 3; i++) {
        fd[i] = dup(0);
        if (fd[i] < 0) {
            return fail("dup");
        }
    }
    if (syscall(436, fd[0], fd[2], 4) != 0) {
        return fail("close_range with CLOSE_RANGE_CLOEXEC");
    }
    for (i = 0; i < 3; i++) {
        flags = fcntl(fd[i], F_GETFD);
        if (flags < 0 || !(flags & FD_CLOEXEC)) {
            return fail("close_range with CLOSE_RANGE_CLOEXEC left a descriptor unmarked");
        }
    }
    held();
    flags = fcntl(0, F_GETFD);
    if (flags < 0 || (flags & FD_CLOEXEC)) {
        return fail("close_range with CLOSE_RANGE_CLOEXEC marked a descriptor below the range");
    }
    held();
    errno = 0;
    if (syscall(436, fd[0], fd[0], 8) != -1 || errno != EINVAL) {
        return fail("close_range with an unknown flag");
    }
    held();
    errno = 0;
    if (syscall(436, fd[2], fd[0], 0) != -1 || errno != EINVAL) {
        return fail("close_range with first past last");
    }
    held();
    if (syscall(436, fd[0], ~0U, 0) != 0 || fcntl(fd[0], F_GETFD) != -1 ||
        fcntl(fd[2], F_GETFD) != -1 || fcntl(0, F_GETFD) < 0) {
        return fail("close_range to the end of the table");
    }
    held();
    return 0;
}

/**
 * Check that splice from a pipe to /dev/null, tee and sendfile fail with
 * EINVAL, and splice from a closed descriptor with EBADF. Returns 0, or 1 after
 * the failure is printed.
 */
static int copies(void)
{
    int p[2], q[2];
    int null = open("/dev/null", O_WRONLY);

    if (null < 0 || pipe(p) != 0 || pipe(q) != 0 || write(p[1], "abc", 3) != 3) {
        return fail("a pipe with bytes in it and /dev/null");
    }
    if (syscall(SYS_splice, p[0], NULL, null, NULL, 16, 0) != -1 || errno != EINVAL) {
        return fail("splice from a pipe to /dev/null was not EINVAL");
    }
    held();
    if (syscall(SYS_tee, p[0], q[1], 16, 0) != -1 || errno != EINVAL) {
        return fail("tee was not EINVAL");
    }
    held();
    if (syscall(SYS_sendfile64, null, p[0], NULL, 16) != -1 || errno != EINVAL) {
        return fail("sendfile was not EINVAL");
    }
    held();
    if (syscall(SYS_splice, 999, NULL, null, NULL, 16, 0) != -1 || errno != EBADF) {
        return fail("splice from a closed descriptor was not EBADF");
    }
    held();
    close(null);
    close(p[0]);
    close(p[1]);
    close(q[0]);
    close(q[1]);
    return 0;
}

/**
 * Check that adjtimex reads the clock's state as Linux's starts,
 * unsynchronized, and that clock_adjtime adjusts CLOCK_REALTIME alone.
 * Returns 0, or 1 after the failure is printed.
 */
static int adjusts(void)
{
    struct timex tx;

    memset(&tx, 0, sizeof tx);
    if (!probe_native() && (adjtimex(&tx) != TIME_ERROR || !(tx.status & STA_UNSYNC) ||
                            tx.tick != 10000 || tx.tolerance != (500L << 16))) {
        return fail("adjtimex did not read an unsynchronized clock with its 10 ms tick");
    }
    if (!probe_native()) {
        held();
    }
    memset(&tx, 0, sizeof tx);
    if (clock_adjtime(CLOCK_MONOTONIC, &tx) != -1 || errno != EOPNOTSUPP) {
        return fail("clock_adjtime of CLOCK_MONOTONIC was not EOPNOTSUPP");
    }
    held();
    return 0;
}

/**
 * Check the extended attribute calls, which no file system here stores, and
 * the advice calls, which change nothing but check their descriptor, on
 * /bin/sh, or on this program's own file when it was started by an absolute
 * path, as in a native run's root, which has no /bin/sh. Returns 0, or 1
 * after the failure is printed.
 */
static int advises(void)
{
    char v[16];
    int p[2];
    const char *file = probe_self("/bin/sh");
    int fd = open(file, O_RDONLY);

    if (fd < 0 || pipe(p) != 0) {
        return fail("a regular file and a pipe");
    }
    if (listxattr(file, v, sizeof v) != 0 || flistxattr(fd, v, sizeof v) != 0) {
        return fail("listxattr of a regular file was not an empty list");
    }
    held();
    if (getxattr(file, "user.x", v, sizeof v) != -1 || errno != EOPNOTSUPP) {
        return fail("getxattr of a regular file was not EOPNOTSUPP");
    }
    held();
    if (lsetxattr("/nonexistent", "user.x", "1", 1, 0) != -1 || errno != ENOENT) {
        return fail("lsetxattr of a missing file was not ENOENT");
    }
    held();
    if (readahead(fd, 0, 4096) != 0 || posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL) != 0) {
        return fail("readahead and posix_fadvise of a regular file did not return 0");
    }
    held();
    if (posix_fadvise(p[0], 0, 0, POSIX_FADV_NORMAL) != ESPIPE) {
        return fail("posix_fadvise of a pipe was not ESPIPE");
    }
    held();
    close(fd);
    close(p[0]);
    close(p[1]);
    return 0;
}

/**
 * Check posix_fallocate in /dev/shm, a tmpfs, which makes the range's pages.
 * Returns 0, or 1 after the failure is printed.
 */
static int reserves_shm(void)
{
    struct stat st;
    int fd = open("/dev/shm/callsmusl.fallocate", O_CREAT | O_RDWR | O_TRUNC, 0600);

    if (fd < 0 || posix_fallocate(fd, 0, 8192) != 0 || fstat(fd, &st) != 0 || st.st_size != 8192 ||
        st.st_blocks != 16) {
        return fail("posix_fallocate in /dev/shm did not make the range's pages");
    }
    held();
    close(fd);
    unlink("/dev/shm/callsmusl.fallocate");
    return 0;
}

/**
 * Check posix_fallocate and fallocate on a KRFFS file in /tmp, a pipe and a
 * /proc file, then the rows of `reserves_shm`. Returns 0, or 1 after the
 * failure is printed.
 */
static int reserves(void)
{
    int p[2];
    int fd = open("/tmp/callsmusl.fallocate", O_CREAT | O_RDWR | O_TRUNC, 0600);
    int ro = open("/tmp/callsmusl.fallocate", O_RDONLY);

    if (fd < 0 || ro < 0 || pipe(p) != 0) {
        return fail("a file in /tmp, open twice, and a pipe");
    }
    if (posix_fallocate(fd, 0, 4096) != EOPNOTSUPP) {
        return fail("posix_fallocate of a file open for writing was not EOPNOTSUPP");
    }
    held();
    if (posix_fallocate(ro, 0, 4096) != EBADF) {
        return fail("posix_fallocate of a file open for reading was not EBADF");
    }
    held();
    if (posix_fallocate(fd, 0, 0) != EINVAL) {
        return fail("posix_fallocate of no bytes was not EINVAL");
    }
    held();
    if (posix_fallocate(p[1], 0, 4096) != ESPIPE) {
        return fail("posix_fallocate of a pipe was not ESPIPE");
    }
    held();
    if (fallocate(ro, 0x100, 0, 4096) != -1 || errno != EOPNOTSUPP) {
        return fail("fallocate with a mode Linux does not take was not EOPNOTSUPP first");
    }
    held();
    close(p[1]);
    p[1] = open("/proc/self/oom_score_adj", O_WRONLY);
    if (p[1] < 0 || posix_fallocate(p[1], 0, 4096) != EOPNOTSUPP) {
        return fail("posix_fallocate of a /proc file open for writing was not EOPNOTSUPP");
    }
    held();
    unlink("/tmp/callsmusl.fallocate");
    close(fd);
    close(ro);
    close(p[0]);
    close(p[1]);
    return reserves_shm();
}

/* The counts of SIGVTALRM and SIGPROF that on_vtalrm and on_prof took. */
static volatile sig_atomic_t vtalrms, profs;

/** Count a SIGVTALRM. */
static void on_vtalrm(int signo)
{
    (void)signo;
    vtalrms++;
}

/** Count a SIGPROF. */
static void on_prof(int signo)
{
    (void)signo;
    profs++;
}

/* The counts of SIGUSR1 and SIGUSR2 that on_usr1 and on_usr2 took. */
static volatile sig_atomic_t usr1s, usr2s;

/** Count a SIGUSR1. */
static void on_usr1(int signo)
{
    (void)signo;
    usr1s++;
}

/** Count a SIGUSR2. */
static void on_usr2(int signo)
{
    (void)signo;
    usr2s++;
}

/**
 * Spend a while in user mode. ITIMER_VIRTUAL counts only user time, and a
 * loop that did nothing but call times() would spend nearly all of its time
 * in the kernel, where a slow guest's ticks all land.
 */
static void spin_user(void)
{
    volatile unsigned int n = 0;

    while (n < 100000) {
        n++;
    }
}

/**
 * Check POSIX timers on the process's and the thread's clocks of processor
 * time: each posts its signal three times while the process spins for at
 * most three seconds of processor time, and timer_gettime reports no more
 * left than its interval. Returns 0, or 1 after the failure is printed.
 */
static int cpu_ptimers(void)
{
    struct itimerspec every = {{0, 10000000}, {0, 10000000}}, cur;
    struct sigevent ev;
    struct tms t;
    timer_t proc_timer, thread_timer;
    clock_t used;
    sigset_t both, mask;

    sigemptyset(&both);
    sigaddset(&both, SIGUSR1);
    sigaddset(&both, SIGUSR2);
    signal(SIGUSR1, on_usr1);
    signal(SIGUSR2, on_usr2);
    sigprocmask(SIG_UNBLOCK, &both, &mask);
    memset(&ev, 0, sizeof ev);
    ev.sigev_notify = SIGEV_SIGNAL;
    ev.sigev_signo = SIGUSR1;
    if (timer_create(CLOCK_PROCESS_CPUTIME_ID, &ev, &proc_timer) != 0) {
        return fail("timer_create on the process's clock of processor time");
    }
    ev.sigev_signo = SIGUSR2;
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &ev, &thread_timer) != 0) {
        return fail("timer_create on the thread's clock of processor time");
    }
    times(&t);
    used = t.tms_utime + t.tms_stime;
    timer_settime(proc_timer, 0, &every, NULL);
    timer_settime(thread_timer, 0, &every, NULL);
    do {
        spin_user();
        times(&t);
    } while ((usr1s < 3 || usr2s < 3) && t.tms_utime + t.tms_stime - used < 300);
    if (timer_gettime(proc_timer, &cur) != 0 || cur.it_interval.tv_nsec != 10000000 ||
        cur.it_value.tv_sec != 0 || cur.it_value.tv_nsec > 10000000) {
        return fail("timer_gettime of a timer of processor time");
    }
    held();
    timer_delete(proc_timer);
    timer_delete(thread_timer);
    sigprocmask(SIG_SETMASK, &mask, NULL);
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
    if (usr1s < 3 || usr2s < 3) {
        return fail("the timers of processor time did not each post three signals");
    }
    held();
    return 0;
}

/**
 * Check the interval timers of processor time, which gprof's sampling uses:
 * each posts its signal three times while the process spins for at most
 * three seconds of processor time, and a value of zero disarms it. Then the
 * rows of `cpu_ptimers`. Returns 0, or 1 after the failure is printed.
 */
static int cputimers(void)
{
    struct itimerval every = {{0, 10000}, {0, 10000}}, off = {{0, 0}, {0, 0}}, cur;
    struct tms t;
    clock_t used;

    signal(SIGVTALRM, on_vtalrm);
    signal(SIGPROF, on_prof);
    times(&t);
    used = t.tms_utime + t.tms_stime;
    if (setitimer(ITIMER_VIRTUAL, &every, NULL) != 0 || setitimer(ITIMER_PROF, &every, NULL) != 0) {
        return fail("setitimer of ITIMER_VIRTUAL and ITIMER_PROF");
    }
    if (getitimer(ITIMER_PROF, &cur) != 0 || cur.it_interval.tv_usec != 10000) {
        return fail("getitimer of ITIMER_PROF did not report its interval");
    }
    held();
    do {
        spin_user();
        times(&t);
    } while ((vtalrms < 3 || profs < 3) && t.tms_utime + t.tms_stime - used < 300);
    setitimer(ITIMER_VIRTUAL, &off, NULL);
    setitimer(ITIMER_PROF, &off, NULL);
    signal(SIGVTALRM, SIG_DFL);
    signal(SIGPROF, SIG_DFL);
    if (vtalrms < 3 || profs < 3) {
        return fail("ITIMER_VIRTUAL and ITIMER_PROF did not each post three signals");
    }
    held();
    return cpu_ptimers();
}

/**
 * Check /dev/mem as Linux's STRICT_DEVMEM gives it to root: the first megabyte
 * read by pread and the local APIC's registers through a shared mapping, a
 * frame of RAM at 32 MiB, which fails both ways with EPERM, and a private
 * mapping, which fails with EINVAL. Returns 0, or 1 after the failure is
 * printed.
 */
static int devmems(void)
{
    unsigned char low[16];
    int fd = open("/dev/mem", O_RDWR);
    volatile unsigned *apic;

    if (fd < 0) {
        return fail("open /dev/mem");
    }
    if (pread(fd, low, sizeof low, 0xF0000) != (ssize_t)sizeof low) {
        return fail("pread of the first megabyte of /dev/mem");
    }
    held();
    if (pread(fd, low, sizeof low, 0x2000000) != -1 || errno != EPERM ||
        mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0x2000000) != MAP_FAILED || errno != EPERM) {
        return fail("/dev/mem gave a frame of RAM");
    }
    held();
    if (mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 0xFEE00000) != MAP_FAILED || errno != EINVAL) {
        return fail("a private mapping of /dev/mem was not EINVAL");
    }
    held();
    apic = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0xFEE00000);
    if (apic == MAP_FAILED || (apic[0x30 / 4] & 0xFF) == 0) {
        return fail("the local APIC's version through a mapping of /dev/mem");
    }
    held();
    munmap((void *)apic, 4096);
    close(fd);
    return 0;
}

/**
 * Check the kernel's log through syslog(2): the ring's size, the whole ring
 * read as records that begin with a priority, after a clear nothing of what
 * came before, and /dev/log a link to the syslog daemon's socket.
 * Returns 0, or 1 after the failure is printed.
 */
static int klogs(void)
{
    static char ring[16384];
    char link[32];
    int n;

    if (klogctl(10, NULL, 0) != 16384) {
        return fail("klogctl's size of the ring");
    }
    held();
    n = klogctl(3, ring, sizeof ring);
    if (n <= 0 || ring[0] != '<' || ring[n - 1] != '\n') {
        return fail("klogctl's read of the whole ring was not whole records");
    }
    held();
    if (klogctl(5, NULL, 0) != 0 || klogctl(3, ring, sizeof ring) != 0) {
        return fail("klogctl's read after a clear gave records from before it");
    }
    held();
    n = (int)readlink("/dev/log", link, sizeof link - 1);
    if (n != 12 || memcmp(link, "/var/run/log", 12) != 0) {
        return fail("/dev/log is not a link to /var/run/log");
    }
    held();
    return 0;
}

/* Linux's RTC ioctls, which no header of this build has, with the wake
   alarm's struct: enabled, pending and a struct rtc_time of nine ints. */
#define RTC_RD_TIME   0x80247009u
#define RTC_WKALM_SET 0x4028700Fu
#define RTC_WKALM_RD  0x80287010u

struct wkalrm {
    unsigned char enabled, pending, pad[2];
    int tm[9];
};

/**
 * Return the CPU ticks /proc/PID/stat gives `pid` in user and system mode,
 * or 0 when the file cannot be read.
 */
static long cpu_ticks(pid_t pid)
{
    char path[32], text[512];
    long u = 0, s = 0;
    int fd, n, field = 1, i;

    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    fd = open(path, O_RDONLY);
    n = fd < 0 ? 0 : (int)read(fd, text, sizeof text - 1);
    if (fd >= 0) {
        close(fd);
    }
    for (i = 0; i < n && field < 15; i++) {
        if (text[i] == ')') {
            field = 2;
        } else if (text[i] == ' ' && field >= 2) {
            field++;
            if (field == 14) {
                u = strtol(text + i + 1, NULL, 10);
            } else if (field == 15) {
                s = strtol(text + i + 1, NULL, 10);
            }
        }
    }
    return u + s;
}

/**
 * Set the wake alarm of /dev/rtc0, open as `fd`, `ahead` seconds from the
 * clock's time. Returns ioctl's result, with the alarm's date and time in
 * `*w`.
 */
static int wake_in(int fd, int ahead, struct wkalrm *w)
{
    struct tm t;
    time_t at;

    memset(w, 0, sizeof *w);
    if (ioctl(fd, RTC_RD_TIME, w->tm) != 0) {
        return -1;
    }
    memset(&t, 0, sizeof t);
    t.tm_sec = w->tm[0];
    t.tm_min = w->tm[1];
    t.tm_hour = w->tm[2];
    t.tm_mday = w->tm[3];
    t.tm_mon = w->tm[4];
    t.tm_year = w->tm[5];
    at = timegm(&t) + ahead;
    gmtime_r(&at, &t);
    w->enabled = 1;
    w->tm[0] = t.tm_sec;
    w->tm[1] = t.tm_min;
    w->tm[2] = t.tm_hour;
    w->tm[3] = t.tm_mday;
    w->tm[4] = t.tm_mon;
    w->tm[5] = t.tm_year;
    return ioctl(fd, RTC_WKALM_SET, w);
}

/** Kill the spinning child `spinner`, when there is one, reap it, and close
 * `state` when it is open; then report `why` as fail does, with the errno of
 * the failure, unless it is 0. */
static int unfreeze(pid_t spinner, int state, const char *why)
{
    int status, saved = errno;

    if (spinner > 0) {
        kill(spinner, SIGKILL);
        waitpid(spinner, &status, 0);
    }
    if (state >= 0) {
        close(state);
    }
    errno = saved;
    return why ? fail(why) : 0;
}

/** Check that freeze waits for the wake alarm while the spinner is held.
 * The caller owns the RTC descriptor `fd`; return 0, or report a failure. */
static int freezes(int fd)
{
    struct wkalrm w, back;
    struct stat st;
    int state, woke;
    long before, after;
    pid_t spinner;

    spinner = fork();
    if (spinner == 0) {
        for (;;) {
        }
    }
    usleep(50000);
    state = open("/sys/power/state", O_WRONLY);
    if (spinner < 0 || state < 0 || wake_in(fd, 2, &w) != 0) {
        return unfreeze(spinner, state, "a spinning child, /sys/power/state and the wake alarm");
    }
    if (fstat(state, &st) != 0 || (st.st_mode & 0777) != 0644) {
        return unfreeze(spinner, state, "the mode fstat gives /sys/power/state's descriptor");
    }
    held();
    before = cpu_ticks(spinner);
    if (ioctl(fd, RTC_WKALM_RD, &back) != 0 || back.pending) {
        return unfreeze(spinner, state, "the wake alarm was already pending before suspend");
    }
    if (write(state, "freeze", 6) != 6) {
        return unfreeze(spinner, state, "a write of freeze to /sys/power/state");
    }
    after = cpu_ticks(spinner);
    woke = ioctl(fd, RTC_WKALM_RD, &back) == 0 && back.pending;
    unfreeze(spinner, state, 0);
    if (!woke) {
        return fail("suspend to idle returned before the wake alarm");
    }
    held();
    if (after - before > 10) {
        return fail("a process ran while a suspend to idle held the processes");
    }
    held();
    return 0;
}

/**
 * Check the clock's wake alarm and suspend to idle: an alarm in the past is
 * ETIME and one two days ahead EINVAL; one two seconds ahead reads back
 * enabled and ends a read of /dev/rtc0 with RTC_AF; the device may wake the
 * machine; a descriptor open on /sys/power/state has mode 0644, as its path
 * does; and a write of freeze to it, with the alarm set again, returns at
 * the alarm with a spinning child given no processor time meanwhile, which
 * the kernel counts. Returns 0, or 1 after the failure is printed.
 */
static int rtcs(void)
{
    struct wkalrm w, back;
    unsigned long data = 0;
    char text[16];
    int fd = open("/dev/rtc0", O_RDONLY), state, status;

    if (fd < 0) {
        return fail("open /dev/rtc0");
    }
    if (wake_in(fd, -10, &w) != -1 || errno != ETIME || wake_in(fd, 2 * 86400, &w) != -1 ||
        errno != EINVAL) {
        return fail("a wake alarm in the past or two days ahead was taken");
    }
    held();
    if (wake_in(fd, 2, &w) != 0 || ioctl(fd, RTC_WKALM_RD, &back) != 0 || !back.enabled ||
        back.tm[0] != w.tm[0] || back.tm[1] != w.tm[1]) {
        return fail("a wake alarm two seconds ahead did not read back");
    }
    held();
    if (read(fd, &data, sizeof data) != (ssize_t)sizeof data || !(data & 0x20) || data >> 8 < 1) {
        return fail("a read of /dev/rtc0 did not end at the alarm with RTC_AF");
    }
    held();
    state = open("/sys/class/rtc/rtc0/device/power/wakeup", O_RDONLY);
    if (state < 0 || read(state, text, sizeof text) != 8 || memcmp(text, "enabled\n", 8) != 0) {
        return fail("the clock may not wake the machine");
    }
    held();
    close(state);
    status = freezes(fd);
    close(fd);
    return status;
}

/** Plan each assertion of `ids`, `n` of them. */
static void plan_rows(const char *const *ids, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        plan_row(ids[i]);
    }
}

#define PLAN(ids) plan_rows(ids, sizeof(ids) / sizeof(ids[0]))

/**
 * Begin the report, with the branch of the architecture, and plan its
 * assertions in the order they are checked; a native run reports `adjtimex`
 * not run.
 */
static void plan(void)
{
    static const char *const before[] = {"sigtimedwait-pending",
                                         "sigtimedwait-eagain",
                                         "sigtimedwait-thread-first",
                                         "sigtimedwait-far",
                                         "zombie-kill",
                                         "zombie-esrch",
                                         "mlock",
                                         "mlockall",
                                         "mlock-gap",
                                         "mincore",
                                         "getcpu",
                                         "personality"};
    static const char *const message_ids[] = {"sendmmsg", "recvmmsg"};
    static const char *const middle[] = {"clock-gettime64",
                                         "time-call",
                                         "close-range-cloexec",
                                         "close-range-below",
                                         "close-range-einval-flag",
                                         "close-range-backward",
                                         "close-range-end",
                                         "splice-einval",
                                         "tee-einval",
                                         "sendfile-einval",
                                         "splice-ebadf",
                                         "fallocate-krffs",
                                         "fallocate-ebadf",
                                         "fallocate-einval",
                                         "fallocate-espipe",
                                         "fallocate-mode",
                                         "fallocate-proc",
                                         "fallocate-shm"};
    static const char *const after[] = {
        "clock-adjtime",      "listxattr",         "getxattr",
        "lsetxattr-enoent",   "readahead-fadvise", "fadvise-espipe",
        "noctty-session",     "maxrss-self",       "maxrss-child",
        "udp-refused",        "setgid-file",       "setgid-dir",
        "privacy-other-root", "privacy-own-maps",  "privacy-dumpable",
        "timer-si-timer",     "timer-overrun",     "timer-delete",
        "sigqueue",           "rt-sigqueueinfo",   "pty-nonblock-eagain",
        "pty-writer-sleeps",  "pty-64k",           "pty-whole-lines",
        "signalfd-eagain",    "signalfd-read",     "itimer-interval",
        "itimer-signals",     "ptimer-gettime",    "ptimer-signals",
        "devmem-low",         "devmem-ram-eperm",  "devmem-private-einval",
        "devmem-apic",        "klog-size",         "klog-records",
        "klog-clear",         "dev-log-link",      "rtc-alarm-refused",
        "rtc-alarm-readback", "rtc-read-af",       "rtc-wakeup-enabled",
        "power-state-mode",   "freeze-wakes",      "freeze-holds"};

    report_begin(report_case(environ, "guest.default.callsmusl"));
#if defined __x86_64__
    report_branch("arch", "x86-64");
#else
    report_branch("arch", "i686");
#endif
    PLAN(before);
#if defined __x86_64__
    plan_row("ids-unsigned");
#endif
    PLAN(message_ids);
#if defined __x86_64__
    plan_row("recvmmsg-wide-count");
    plan_row("socket-wide-domain");
#endif
    PLAN(middle);
    if (probe_native()) {
        report_notrun("adjtimex", "the machine's NTP client has adjusted the clock");
    } else {
        plan_row("adjtimex");
    }
    PLAN(after);
}

int main(int argc, char **argv)
{
    probe_args(argc, argv);
    plan();
    if (signals() || memory() || identity() || messages() || clocks() || ranges() || copies() ||
        reserves() || adjusts() || advises() || terminals() || peaks() || refusals() || setgids() ||
        privacies() || timers() || queues() || flows() || signalfds() || cputimers() || devmems() ||
        klogs() || rtcs()) {
        return 1;
    }
    report_end();
    printf("CALLSMUSL OK\n");
    return 0;
}
