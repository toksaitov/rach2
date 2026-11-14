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
 * of a root process under /proc, and reads its own. Its /proc directory,
 * mode 0555, is its own whether or not it is dumpable; a stat of its status
 * is root's while a change of ids leaves it not dumpable and its own once
 * PR_SET_DUMPABLE makes it so; and a descriptor open on its maps before that
 * stays root's, the owner the open's lookup gave it, as on Linux. Normal
 * and O_PATH descriptors on files and directories keep their user and group
 * IDs across credential and dumpability changes. A walk, including one to a
 * missing child, refreshes the directories it crosses; listing /dev does
 * not. O_PATH descriptors on unfollowed links keep each link's owner until
 * lstat reaches it. An inherited fd directory's owner governs fchdir from
 * another process before and after a walk updates it. readlink, a followed
 * stat and a walk through a link each update that link's owner, as Linux's
 * walk revalidates every component it reaches. The size of a process's fd
 * directory counts the descriptor an open of a FIFO has reserved while it
 * waits for a writer, as Linux counts its open-descriptor bitmap. mlock,
 * mlock2, munlock, mlockall and munlockall accept a mapped range and count
 * in VmLck, a mapping made under MCL_FUTURE is locked, an
 * unprivileged mlock past RLIMIT_MEMLOCK fails with ENOMEM and with a limit
 * of 0 with EPERM, and mlock fails on a gap with ENOMEM; mincore reports a
 * touched page resident.
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
 * splice moves a pipe's bytes to /dev/null and a file's into a pipe from an
 * offset it moves, tee copies a pipe's bytes to another and leaves them,
 * sendfile sends a file to an AF_UNIX socket and to a file, copy_file_range
 * copies a file's bytes into another at both offsets and stops at the end,
 * SEEK_DATA finds data at the offset and SEEK_HOLE the hole at the end,
 * FICLONE is EOPNOTSUPP, faccessat2 takes AT_EACCESS, AT_SYMLINK_NOFOLLOW and
 * AT_EMPTY_PATH, and splice from a closed descriptor is EBADF.
 * copy_file_range of 100 KiB between ranges of one file that overlap, opened
 * through a directory and a bind of it, is EINVAL and leaves the file as it
 * was. Two readers of a pipe, one splicing it into a full socket and one
 * reading it, never both get a byte. vmsplice into a pipe's write end from
 * two buffers puts their bytes in it whole; out of its read end into three
 * buffers it waits for the first byte and stops when the pipe is empty;
 * with SPLICE_F_NONBLOCK it is EAGAIN on an empty pipe's read end and a
 * full pipe's write end; on a descriptor that is not a pipe it is EBADF,
 * and with more than IOV_MAX buffers EINVAL. read(2) of a pipe into NULL
 * is EAGAIN when the pipe is empty and nonblocking and 0 once it has ended,
 * as Linux's pipe_read answers before its copy. A pty master
 * reads the slave's last output and then EIO once the slave has closed.
 * posix_fallocate gives a file in /dev/shm its pages, the size and the blocks
 * of the range, and answers EOPNOTSUPP for a file of KRFFS or of /proc open for
 * writing, after which glibc's writes the range itself, EBADF for one open only
 * for reading, EINVAL for a length of 0, ESPIPE for a pipe, and EOPNOTSUPP for
 * a mode Linux does not take before the check that the descriptor is open for
 * writing. adjtimex reads an unsynchronized clock with its 10 ms tick, and
 * clock_adjtime of CLOCK_MONOTONIC is EOPNOTSUPP. The extended attribute calls
 * find the file: an empty list, EOPNOTSUPP for an unsupported file system or
 * ENODATA on ext2 for a missing user value, and ENOENT for a missing file.
 * readahead and posix_fadvise accept a regular file, and posix_fadvise fails on
 * a pipe with ESPIPE. setitimer's ITIMER_VIRTUAL and ITIMER_PROF post SIGVTALRM
 * and SIGPROF as the process spins, and getitimer reports the interval; so do
 * POSIX timers on the process's and the thread's clocks of processor time.
 * ITIMER_PROF and ITIMER_VIRTUAL of 10 ms expire at the tick charged that
 * passes 10 ms and a tick more, as Linux's do; a POSIX timer of processor
 * time set to an absolute time past expires during the setting, though the
 * process then sleeps, with the intervals since that time as its overrun;
 * and one on another thread's clock counts an absolute time from that
 * thread's time, whoever sets it. A tick that debugfs's cputime_test holds
 * between a setting's sample and its arming is taken off the time left. A
 * timer on a thread's clock reports nothing, refuses a setting with ESRCH
 * and never expires once the thread is reaped, though a thread made through
 * /proc/sys/kernel/ns_last_pid with the same id spins.
 * /dev/mem reads the first megabyte and maps the local APIC, and fails with
 * EPERM for RAM and with EINVAL for a private mapping. klogctl gives the log
 * ring's size, its records whole, and after a clear none of those before;
 * /dev/log leads to /var/run/log. The clock's wake alarm, set for a time past
 * or enabled by RTC_AIE_ON with its time past, comes, which a read waits for,
 * and is then off with its time kept; one two days ahead fails with EINVAL,
 * and one an hour ahead reads back with its time, which an invalid date and
 * time leaves set. The alarm ends a read at its time and wakes a suspend to
 * idle that gives no process time meanwhile.
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
 * `splice-pipe-null`, `splice-file-pipe`, `tee-pipes`, `sendfile-socket`,
 * `sendfile-file`, `copy-file-range`, `seek-data-hole`, `ficlone-eopnotsupp`,
 * `faccessat2`, `splice-ebadf`, `copy-file-range-bind-overlap`,
 * `splice-readers-once`, `vmsplice-write`, `vmsplice-read`,
 * `vmsplice-nonblock`, `vmsplice-ebadf`, `vmsplice-einval`,
 * `pipe-read-null-eagain`, `pipe-read-null-eof`, `pty-hangup-eio`,
 * `fallocate-krffs`, `fallocate-ebadf`, `fallocate-einval`, `fallocate-espipe`,
 * `fallocate-mode`, `fallocate-proc`, `fallocate-shm`, `adjtimex`,
 * `clock-adjtime`, `listxattr`, `getxattr-absent`, `lsetxattr-enoent`,
 * `readahead-fadvise`, `fadvise-espipe`, `noctty-session`, `maxrss-self`,
 * `maxrss-child`, `udp-refused`, `setgid-file`, `setgid-dir`,
 * `privacy-other-root`, `privacy-own-maps`, `privacy-kept-dirs`,
 * `privacy-kept-opens`, `privacy-dumpable`, `privacy-kept-files`,
 * `privacy-dev-list`, `privacy-kept-links`, `privacy-fchdir-owner`,
 * `privacy-fd-walk`, `privacy-link-walks`, `fd-size-reserved`,
 * `timer-si-timer`, `timer-overrun`, `timer-delete`, `sigqueue`,
 * `rt-sigqueueinfo`, `pty-nonblock-eagain`, `pty-writer-sleeps`, `pty-64k`,
 * `pty-whole-lines`, `signalfd-eagain`, `signalfd-read`, `itimer-interval`,
 * `itimer-signals`, `ptimer-gettime`, `ptimer-signals`, `itimer-first-tick`,
 * `ptimer-past-due`, `ptimer-other-thread`, `ptimer-tick-at-arming`,
 * `ptimer-reused-tid`, `ptimer-reaped-esrch`, `ptimer-zombie-leader`,
 * `devmem-low`,
 * `devmem-ram-eperm`, `devmem-private-einval`, `devmem-apic`, `klog-size`,
 * `klog-records`, `klog-clear`, `dev-log-link`, `rtc-alarm-range`,
 * `rtc-aie-on-past`, `rtc-alarm-readback`, `rtc-alarm-invalid`,
 * `rtc-read-af`, `rtc-wakealarm-empty`, `rtc-wakeup-enabled`,
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
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/signalfd.h>
#include <sys/timex.h>
#include <sys/xattr.h>
#include <termios.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/uio.h>
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
/* ioctl's FICLONE, _IOW(0x94, 9, int), from linux/fs.h. */
#define FICLONE_REQUEST 0x40049409

#if defined __x86_64__
#define SYS_CLOCK_GETTIME64 SYS_clock_gettime
#define SYS_sendfile64      SYS_sendfile
#else
#define SYS_CLOCK_GETTIME64 403
#endif

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
static const char *rows[128];
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
        if (poll(&p, 1, -1) != 1 || !(p.revents & POLLERR)) {
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
 * sigwaitinfo. Returns 0, or 1 after the failure is printed.
 */
static int timers(void)
{
    struct sigevent se;
    struct itimerspec its;
    struct timespec zero = {0, 0};
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
    if (sigwaitinfo(&set, &si) != SIGUSR2 || si.si_code != SI_TIMER ||
        si.si_value.sival_int != 99) {
        return fail("the timer's signal did not come with SI_TIMER and its value");
    }
    held();
    its.it_interval.tv_nsec = 20000000;
    if (timer_settime(t, 0, &its, NULL) != 0) {
        return fail("timer_settime of a periodic timer");
    }
    /* interval: the blocked periodic timer of 20 ms runs for ten periods */
    usleep(210000);
    if (sigwaitinfo(&set, &si) != SIGUSR2 || si.si_overrun < 3 || timer_getoverrun(t) < 3) {
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
 * sigwaitinfo. Returns 0, or 1 after the failure is printed.
 */
static int queues(void)
{
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
    if (sigwaitinfo(&set, &si) != SIGUSR2 || si.si_code != SI_QUEUE ||
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
    if (sigwaitinfo(&set, &si) != SIGUSR2 || si.si_errno != 77 || si.si_value.sival_int != 4343) {
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

/* Descriptors kept by privacies_child; its exit closes every one. */
struct privacy_fds {
    uid_t user;
    gid_t group;
    int dirs[3][2];        /* fd, fdinfo, task; each opened for reading and with O_PATH */
    struct stat before[3]; /* directory owners before the child's IDs change */
    int maps;
    int pathmaps;
    int info[2]; /* the same fdinfo file, opened for reading and with O_PATH */
    char info_path[64];
    int links[7];
    char link_path[7][64];
};

/** Whether fstat of fd reports the given owner and complete mode. */
static int privacy_owner(int fd, uid_t user, gid_t group, mode_t mode)
{
    struct stat st;

    return fstat(fd, &st) == 0 && st.st_uid == user && st.st_gid == group && st.st_mode == mode;
}

/** Open the three directories before changing IDs; 0, or 1 on failure. */
static int privacy_dirs_open(struct privacy_fds *p)
{
    static const char *const paths[] = {"/proc/self/fd", "/proc/self/fdinfo", "/proc/self/task"};
    int i;

    for (i = 0; i < 3; i++) {
        p->dirs[i][0] = open(paths[i], O_RDONLY | O_DIRECTORY);
        p->dirs[i][1] = open(paths[i], O_PATH | O_DIRECTORY);
        if (p->dirs[i][0] < 0 || p->dirs[i][1] < 0 || fstat(p->dirs[i][0], &p->before[i]) != 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * Check that changing IDs leaves each directory's owner alone, and a walk
 * below fdinfo or task updates it even when the last lookup fails. Returns
 * 0, or 1 on failure. The caller has changed both IDs since opening them.
 */
static int privacy_dirs_kept(struct privacy_fds *p)
{
    static const char *const missing[] = {"/proc/self/fdinfo/2147483647",
                                          "/proc/self/task/2147483647/status"};
    struct stat st;
    mode_t mode;
    int i, j;

    for (i = 0; i < 3; i++) {
        mode = S_IFDIR | (i == 0 ? 0500 : 0555);
        for (j = 0; j < 2; j++) {
            if (!privacy_owner(p->dirs[i][j], p->before[i].st_uid, p->before[i].st_gid, mode)) {
                return 1;
            }
        }
    }
    for (i = 1; i < 3; i++) {
        if (stat(missing[i - 1], &st) != -1 || errno != ENOENT) {
            return 1;
        }
        for (j = 0; j < 2; j++) {
            if (!privacy_owner(p->dirs[i][j], p->user, p->group, S_IFDIR | 0555)) {
                return 1;
            }
        }
    }
    return 0;
}

/** Open the files and unfollowed links while not dumpable; 0, or 1. */
static int privacy_leaves_open(struct privacy_fds *p)
{
    static const char *const names[] = {"exe", "cwd", "root", "ns/mnt", "ns/uts"};
    int i;

    p->pathmaps = open("/proc/self/maps", O_PATH);
    snprintf(p->info_path, sizeof p->info_path, "/proc/self/fdinfo/%d", p->maps);
    p->info[0] = open(p->info_path, O_RDONLY);
    p->info[1] = open(p->info_path, O_PATH);
    if (p->pathmaps < 0 || p->info[0] < 0 || p->info[1] < 0) {
        return 1;
    }
    for (i = 0; i < 7; i++) {
        if (i < 5) {
            snprintf(p->link_path[i], sizeof p->link_path[i], "/proc/self/%s", names[i]);
        } else {
            snprintf(p->link_path[i], sizeof p->link_path[i], "/proc/self/fd/%d",
                     i == 5 ? p->maps : p->dirs[0][0]);
        }
        p->links[i] = open(p->link_path[i], O_PATH | O_NOFOLLOW);
        if (p->links[i] < 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * Check the directory's 0555 mode and owner and status's changing owner
 * across PR_SET_DUMPABLE, while maps keeps its old owner. Returns 0, or 1.
 */
static int privacy_dumpable(struct privacy_fds *p)
{
    struct stat st;

    if (stat("/proc/self/", &st) != 0 || st.st_mode != (S_IFDIR | 0555) || st.st_uid != p->user ||
        st.st_gid != p->group || stat("/proc/self/status", &st) != 0 || st.st_uid != 0 ||
        st.st_gid != 0 || !privacy_owner(p->maps, 0, 0, S_IFREG | 0444)) {
        return 1;
    }
    if (prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0 || stat("/proc/self/", &st) != 0 ||
        st.st_mode != (S_IFDIR | 0555) || st.st_uid != p->user || st.st_gid != p->group ||
        stat("/proc/self/status", &st) != 0 || st.st_uid != p->user || st.st_gid != p->group ||
        !privacy_owner(p->maps, 0, 0, S_IFREG | 0444)) {
        return 1;
    }
    return 0;
}

/**
 * Check the retained owners of normal and O_PATH files after dumpability
 * changes, then their update by stat, including fdinfo's 0444 mode. 0, or 1.
 */
static int privacy_files_kept(struct privacy_fds *p)
{
    struct stat st;
    int i;

    if (!privacy_owner(p->pathmaps, 0, 0, S_IFREG | 0444)) {
        return 1;
    }
    for (i = 0; i < 2; i++) {
        if (!privacy_owner(p->info[i], 0, 0, S_IFREG | 0444)) {
            return 1;
        }
    }
    if (stat("/proc/self/maps", &st) != 0 || st.st_uid != p->user || st.st_gid != p->group ||
        !privacy_owner(p->maps, p->user, p->group, S_IFREG | 0444) ||
        !privacy_owner(p->pathmaps, p->user, p->group, S_IFREG | 0444) ||
        stat(p->info_path, &st) != 0 || st.st_mode != (S_IFREG | 0444) || st.st_uid != p->user ||
        st.st_gid != p->group) {
        return 1;
    }
    for (i = 0; i < 2; i++) {
        if (!privacy_owner(p->info[i], p->user, p->group, S_IFREG | 0444)) {
            return 1;
        }
    }
    return 0;
}

/** List /dev, including fd, without changing /proc/self/fd's owner; 0, or 1. */
static int privacy_dev_list(struct privacy_fds *p)
{
    DIR *d = opendir("/dev");
    struct dirent *entry;
    int found = 0, error, i;

    if (!d) {
        return 1;
    }
    errno = 0;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, "fd") == 0) {
            found = 1;
        }
    }
    error = errno;
    if (closedir(d) != 0 || error != 0 || !found) {
        return 1;
    }
    for (i = 0; i < 2; i++) {
        if (!privacy_owner(p->dirs[0][i], 0, 0, S_IFDIR | 0500)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Check each unfollowed link's old owner, then update it by lstat without
 * changing any other link's owner, including another namespace or fd link.
 * Returns 0, or 1 on failure; Linux's fd-link modes depend on the target.
 */
static int privacy_links_kept(struct privacy_fds *p)
{
    struct stat st;
    int i, j;

    for (i = 0; i < 7; i++) {
        for (j = i; j < 7; j++) {
            if (fstat(p->links[j], &st) != 0 || !S_ISLNK(st.st_mode) || st.st_uid != 0 ||
                st.st_gid != 0) {
                return 1;
            }
        }
        if (lstat(p->link_path[i], &st) != 0 || !S_ISLNK(st.st_mode) || st.st_uid != p->user ||
            st.st_gid != p->group || fstat(p->links[i], &st) != 0 || !S_ISLNK(st.st_mode) ||
            st.st_uid != p->user || st.st_gid != p->group) {
            return 1;
        }
    }
    return 0;
}

/**
 * Have a separate process try fchdir on the inherited fd; 0 if it gets
 * error (0 for success), 1 otherwise. The child exits and is reaped here,
 * and cannot use Linux's exception that lets a process enter its own fd.
 */
static int privacy_chdir(int fd, int error)
{
    pid_t pid = fork();
    int rc, status;

    if (pid == 0) {
        rc = fchdir(fd);
        _exit(error == 0 ? rc != 0 : !(rc == -1 && errno == error));
    }
    if (pid < 0) {
        return 1;
    }
    do {
        rc = waitpid(pid, &status, 0);
    } while (rc < 0 && errno == EINTR);
    return rc != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0;
}

/**
 * Give fd a root owner, then make the process dumpable without another
 * lookup. Its inherited descriptors must refuse fchdir. Returns 0, or 1.
 */
static int privacy_chdir_kept(struct privacy_fds *p)
{
    struct stat st;
    int i;

    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0 || stat("/proc/self/fd", &st) != 0 ||
        st.st_uid != 0 || st.st_gid != 0 || prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0) {
        return 1;
    }
    for (i = 0; i < 2; i++) {
        if (!privacy_owner(p->dirs[0][i], 0, 0, S_IFDIR | 0500) ||
            privacy_chdir(p->dirs[0][i], EACCES)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Walk through fd to update its owner; after clearing dumpability again,
 * its descriptors keep that owner and allow another process's fchdir.
 * Returns 0, or 1 on failure.
 */
static int privacy_fd_walk(struct privacy_fds *p)
{
    struct stat st;
    char path[64];
    int i;

    snprintf(path, sizeof path, "/proc/self/fd/%d", p->maps);
    if (stat(path, &st) != 0) {
        return 1;
    }
    for (i = 0; i < 2; i++) {
        if (!privacy_owner(p->dirs[0][i], p->user, p->group, S_IFDIR | 0500)) {
            return 1;
        }
    }
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
        return 1;
    }
    for (i = 0; i < 2; i++) {
        if (!privacy_owner(p->dirs[0][i], p->user, p->group, S_IFDIR | 0500) ||
            privacy_chdir(p->dirs[0][i], 0)) {
            return 1;
        }
    }
    return 0;
}

/**
 * With the process not dumpable again and each link's kept owner the user's
 * since privacy_links_kept, reach five links by a path other than lstat:
 * readlink of exe and of a descriptor link, a stat that follows a
 * descriptor link and a namespace link, and a stat of a name past root,
 * which walks through it. Each must give the O_PATH descriptor kept on that
 * link root's owner, as Linux's walk revalidates the component before it
 * reads or follows the link (fs/namei.c:1694-1706). Returns 0, or 1.
 */
static int privacy_link_walks(struct privacy_fds *p)
{
    char link[256];
    struct stat st;
    int i;

    for (i = 0; i < 7; i++) {
        if (fstat(p->links[i], &st) != 0 || st.st_uid != p->user || st.st_gid != p->group) {
            return 1;
        }
    }
    if (readlink(p->link_path[0], link, sizeof link) <= 0 ||
        readlink(p->link_path[6], link, sizeof link) <= 0 || stat(p->link_path[5], &st) != 0 ||
        stat(p->link_path[3], &st) != 0 || stat("/proc/self/root/dev", &st) != 0) {
        return 1;
    }
    for (i = 0; i < 7; i++) {
        int walked = i == 0 || i == 2 || i == 3 || i == 5 || i == 6;

        if (fstat(p->links[i], &st) != 0 || st.st_uid != (walked ? 0 : p->user) ||
            st.st_gid != (walked ? 0 : p->group)) {
            return 1;
        }
    }
    return 0;
}

/**
 * Run the privacy rows in a child of root, changing both IDs to the test's
 * user and group. All descriptors and the changed credentials die with the
 * child. Returns 0, or the step that failed, which privacies maps to a row.
 */
static int privacies_child(pid_t parent)
{
    struct privacy_fds p;
    char path[64], link[64];
    DIR *d;

    p.user = (uid_t)probe_id(4243);
    p.group = (gid_t)probe_id(4242);
    if (privacy_dirs_open(&p) || setgid(p.group) != 0 || setuid(p.user) != 0 ||
        prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
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
    p.maps = open("/proc/self/maps", O_RDONLY);
    if (p.maps < 0 || read(p.maps, link, sizeof link) <= 0) {
        return 4;
    }
    if (privacy_dirs_kept(&p)) {
        return 5;
    }
    if (privacy_leaves_open(&p)) {
        return 6;
    }
    if (privacy_dumpable(&p)) {
        return 7;
    }
    if (privacy_files_kept(&p)) {
        return 8;
    }
    if (privacy_dev_list(&p)) {
        return 9;
    }
    if (privacy_links_kept(&p)) {
        return 10;
    }
    if (privacy_chdir_kept(&p)) {
        return 11;
    }
    if (privacy_fd_walk(&p)) {
        return 12;
    }
    return privacy_link_walks(&p) ? 13 : 0;
}

/** Run the privacy child and report its eleven rows in order; 0, or 1. */
static int privacies(void)
{
    static const char *const failures[] = {
        "privacy setup or another user's access to a root process",
        "a child of user 4243 could not read its own maps",
        "retained /proc directory owners across changed IDs and failed walks",
        "an O_PATH open of maps or of an unfollowed link, or an open of fdinfo",
        "/proc directory mode or owner across PR_SET_DUMPABLE",
        "retained /proc file owners or fdinfo mode",
        "listing /dev changed a retained /proc owner",
        "retained /proc link owners or their separate lookup updates",
        "fchdir ignored a retained root owner",
        "a walk through fd did not update its owner, or fchdir lost that owner",
        "readlink, a followed stat or a walk through a link kept its old owner"};
    int status, row;
    int base = rows_reported;
    pid_t pid, waited, parent = getpid();

    pid = fork();
    if (pid == 0) {
        _exit(privacies_child(parent));
    }
    if (pid < 0) {
        return fail("fork for /proc privacy");
    }
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != pid || !WIFEXITED(status)) {
        return fail("a child of user 4243");
    }
    if (WEXITSTATUS(status) != 0) {
        row = WEXITSTATUS(status) > 3 ? WEXITSTATUS(status) - 3 : 0;
        if (row >= (int)(sizeof failures / sizeof failures[0])) {
            return fail("unknown /proc privacy step");
        }
        held_to(base + row);
        return fail(failures[row]);
    }
    held_to(base + (int)(sizeof failures / sizeof failures[0]));
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
 * gone from /proc.
 */
static int seen_asleep(pid_t pid)
{
    int state;

    for (;;) {
        state = proc_state(pid);
        if (state == 'S') {
            return 1;
        }
        if (state == 0 || state == 'Z') {
            return 0;
        }
        /* interval: a process's state is looked at again every 10 ms */
        usleep(10000);
    }
}

/**
 * Check that the size of a process's /proc fd directory counts the
 * descriptor an open has reserved before it blocks, as Linux's
 * proc_readfd_count weighs the open-descriptor bitmap (Linux 6.12
 * fs/proc/fd.c:299), whose bit the open sets (fs/file.c:531) before it waits
 * in a FIFO's open (fs/open.c:1419). A child writes the size of its own fd
 * directory and opens a FIFO for reading with no writer; once it is seen
 * asleep, stat and fstat of its fd directory give one more, and an open for
 * writing lets it go. Returns 0, or 1.
 */
static int reserved_fds(void)
{
    static const char fifo[] = "/tmp/callsmusl.fifo";
    char path[64];
    struct stat st, fst;
    int res[2], dir = -1, w = -1, n = -1, status = 0, ok;
    pid_t pid, waited;

    unlink(fifo);
    if (mkfifo(fifo, 0600) != 0 || pipe(res) != 0) {
        unlink(fifo);
        return fail("mkfifo and a pipe for the reserved descriptor");
    }
    pid = fork();
    if (pid == 0) {
        close(res[0]);
        if (stat("/proc/self/fd", &st) != 0) {
            _exit(1);
        }
        n = (int)st.st_size;
        if (write(res[1], &n, sizeof n) != (ssize_t)sizeof n) {
            _exit(1);
        }
        _exit(open(fifo, O_RDONLY) < 0);
    }
    close(res[1]);
    ok = pid > 0 && read(res[0], &n, sizeof n) == (ssize_t)sizeof n && seen_asleep(pid);
    if (ok) {
        snprintf(path, sizeof path, "/proc/%d/fd", (int)pid);
        dir = open(path, O_RDONLY | O_DIRECTORY);
        ok = stat(path, &st) == 0 && dir >= 0 && fstat(dir, &fst) == 0 && st.st_size == n + 1 &&
             fst.st_size == n + 1;
    }
    if (pid > 0) {
        w = open(fifo, O_WRONLY | O_NONBLOCK);
        if (w < 0) {
            kill(pid, SIGKILL);
        }
        do {
            waited = waitpid(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
        ok = ok && w >= 0 && waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    if (dir >= 0) {
        close(dir);
    }
    if (w >= 0) {
        close(w);
    }
    close(res[0]);
    unlink(fifo);
    if (!ok) {
        return fail("the size of /proc/PID/fd left out a descriptor a FIFO's open reserved");
    }
    held();
    return 0;
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

/** VmLck of /proc/self/status in KiB, or -1 when it cannot be read. Read
 * with no stdio, whose buffer would be memory that MCL_FUTURE locks. */
static long locked_kb(void)
{
    static char text[4096];
    int fd = open("/proc/self/status", O_RDONLY);
    ssize_t n = fd < 0 ? -1 : read(fd, text, sizeof(text) - 1);
    char *at;

    if (fd >= 0) {
        close(fd);
    }
    if (n <= 0) {
        return -1;
    }
    text[n] = '\0';
    at = strstr(text, "VmLck:");
    return at ? strtol(at + 6, NULL, 10) : -1;
}

/**
 * In a child made unprivileged, with RLIMIT_MEMLOCK of one page: mlock of
 * two pages fails with ENOMEM and of one succeeds, and with a limit of 0
 * mlock fails with EPERM, as Linux's can_do_mlock answers. The child's
 * status is 0, or the number of the check that failed.
 */
static int memlock_limit(char *p, long page)
{
    struct rlimit one = {(rlim_t)page, (rlim_t)page}, none = {0, 0};
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        if (setrlimit(RLIMIT_MEMLOCK, &one) != 0 || (geteuid() == 0 && setuid(65534) != 0)) {
            _exit(1);
        }
        if (mlock(p, 2 * page) != -1 || errno != ENOMEM) {
            _exit(2);
        }
        if (mlock(p, page) != 0 || locked_kb() != page / 1024 || munlock(p, page) != 0) {
            _exit(3);
        }
        if (setrlimit(RLIMIT_MEMLOCK, &none) != 0 || mlock(p, page) != -1 || errno != EPERM) {
            _exit(4);
        }
        _exit(0);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return 9;
    }
    return WEXITSTATUS(status);
}

/**
 * Check mlockall and munlockall with VmLck, which held `base` KiB before,
 * read after each, and that a mapping made under MCL_FUTURE is locked.
 * Returns 0, or 1 after the failure is printed.
 */
static int lock_all(long page, long base)
{
    long kb = page / 1024;
    char *q;

    if (mlockall(MCL_CURRENT) != 0 || locked_kb() < base + 2 * kb || munlockall() != 0 ||
        locked_kb() != 0) {
        return fail("mlockall, and VmLck after mlockall and munlockall");
    }
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0 || (base = locked_kb()) < 2 * kb) {
        return fail("mlockall with MCL_FUTURE");
    }
    q = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q == MAP_FAILED || locked_kb() != base + kb || munlockall() != 0 || locked_kb() != 0) {
        return fail("a mapping under MCL_FUTURE is locked");
    }
    munmap(q, page);
    return 0;
}

/**
 * Check mlock, mlock2 with MLOCK_ONFAULT and munlock on a mapped range, with
 * VmLck read after each call, and RLIMIT_MEMLOCK's errors; mlockall and
 * munlockall, and MCL_FUTURE's lock of a later mapping; mlock across a gap;
 * and mincore of a touched page. Returns 0, or 1 after the failure is
 * printed.
 */
static int memory(void)
{
    long page = sysconf(_SC_PAGESIZE), kb = page / 1024, base;
    unsigned char vec[2];
    char *p = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int rc;

    if (p == MAP_FAILED || (base = locked_kb()) < 0) {
        return fail("mmap, or VmLck of /proc/self/status");
    }
    if (mlock(p, 2 * page) != 0 || locked_kb() != base + 2 * kb || munlock(p, 2 * page) != 0 ||
        locked_kb() != base) {
        return fail("mlock of a mapped range, and VmLck after mlock and munlock");
    }
    if (syscall(SYS_mlock2, p, page, 1 /* MLOCK_ONFAULT */) != 0 || locked_kb() != base + kb ||
        munlock(p, page) != 0 || locked_kb() != base) {
        return fail("mlock2 with MLOCK_ONFAULT, and VmLck after it");
    }
    if ((rc = memlock_limit(p, page)) != 0) {
        errno = rc;
        return fail("RLIMIT_MEMLOCK: ENOMEM past it, EPERM at 0 (errno is the check)");
    }
    held();
    if (lock_all(page, base) != 0) {
        return 1;
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
 * Check the copies between descriptors, on files in /tmp and a link in
 * /dev/shm, since KRFFS keeps no links: splice of a pipe's bytes to /dev/null
 * and of a file's from an offset into a pipe, tee between pipes, sendfile to
 * an AF_UNIX socket and to a file, copy_file_range, SEEK_DATA and SEEK_HOLE,
 * FICLONE, faccessat2's flags, and splice from a closed descriptor. Returns
 * 0, or 1 after the failure is printed.
 */
static int copies(void)
{
    static const char text[] = "0123456789abcdef";
    char got[32];
    int p[2], q[2], sv[2];
    int null = open("/dev/null", O_WRONLY);
    int fd = open("/tmp/callsmusl.copy", O_CREAT | O_RDWR | O_TRUNC, 0600);
    int out = open("/tmp/callsmusl.copied", O_CREAT | O_RDWR | O_TRUNC, 0600);
    off_t at, to;

    unlink("/dev/shm/callsmusl.link");
    if (null < 0 || fd < 0 || out < 0 || pipe(p) != 0 || pipe(q) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 || write(fd, text, 16) != 16 ||
        write(p[1], "abc", 3) != 3 ||
        symlink("/dev/shm/callsmusl.none", "/dev/shm/callsmusl.link") != 0) {
        return fail("files, pipes, a socket pair, a dangling link and /dev/null");
    }
    if (splice(p[0], NULL, null, NULL, 16, 0) != 3) {
        return fail("splice of a pipe's 3 bytes to /dev/null did not move them");
    }
    held();
    at = 4;
    if (splice(fd, &at, p[1], NULL, 6, 0) != 6 || at != 10 || read(p[0], got, sizeof got) != 6 ||
        memcmp(got, "456789", 6) != 0 || lseek(fd, 0, SEEK_CUR) != 16) {
        return fail("splice of a file from an offset into a pipe");
    }
    held();
    if (write(p[1], "xyz", 3) != 3 || tee(p[0], q[1], 16, 0) != 3 ||
        read(q[0], got, sizeof got) != 3 || read(p[0], got + 3, sizeof got - 3) != 3 ||
        memcmp(got, "xyzxyz", 6) != 0) {
        return fail("tee did not copy a pipe's bytes and leave them");
    }
    held();
    at = 8;
    if (sendfile(sv[0], fd, &at, 8) != 8 || at != 16 || read(sv[1], got, sizeof got) != 8 ||
        memcmp(got, "89abcdef", 8) != 0) {
        return fail("sendfile of a file to an AF_UNIX socket");
    }
    held();
    if (lseek(fd, 2, SEEK_SET) != 2 || sendfile(out, fd, NULL, 4) != 4 ||
        lseek(fd, 0, SEEK_CUR) != 6 || pread(out, got, sizeof got, 0) != 4 ||
        memcmp(got, "2345", 4) != 0) {
        return fail("sendfile of a file to a file");
    }
    held();
    at = 10;
    to = 4;
    if (copy_file_range(fd, &at, out, &to, 64, 0) != 6 || at != 16 || to != 10 ||
        pread(out, got, sizeof got, 0) != 10 || memcmp(got, "2345abcdef", 10) != 0 ||
        copy_file_range(fd, &at, out, &to, 64, 0) != 0) {
        return fail("copy_file_range");
    }
    held();
    if (lseek(fd, 5, SEEK_DATA) != 5 || lseek(fd, 5, SEEK_HOLE) != 16 ||
        lseek(fd, 16, SEEK_DATA) != -1 || errno != ENXIO) {
        return fail("lseek's SEEK_DATA and SEEK_HOLE");
    }
    held();
    if (ioctl(out, FICLONE_REQUEST, fd) != -1 || errno != EOPNOTSUPP) {
        return fail("FICLONE was not EOPNOTSUPP");
    }
    held();
    if (syscall(SYS_faccessat2, AT_FDCWD, "/tmp/callsmusl.copy", R_OK | W_OK, AT_EACCESS) != 0 ||
        syscall(SYS_faccessat2, AT_FDCWD, "/dev/shm/callsmusl.link", F_OK, AT_SYMLINK_NOFOLLOW) !=
            0 ||
        syscall(SYS_faccessat2, fd, "", R_OK, AT_EMPTY_PATH) != 0 ||
        syscall(SYS_faccessat2, AT_FDCWD, "/dev/shm/callsmusl.link", F_OK, 0) != -1 ||
        errno != ENOENT) {
        return fail("faccessat2's flags");
    }
    held();
    if (splice(999, NULL, null, NULL, 16, 0) != -1 || errno != EBADF) {
        return fail("splice from a closed descriptor was not EBADF");
    }
    held();
    close(null);
    close(fd);
    close(out);
    close(p[0]);
    close(p[1]);
    close(q[0]);
    close(q[1]);
    close(sv[0]);
    close(sv[1]);
    unlink("/tmp/callsmusl.copy");
    unlink("/tmp/callsmusl.copied");
    unlink("/dev/shm/callsmusl.link");
    return 0;
}

/* The directory, the bind of it, and the bytes of the file of
 * `bind_overlap`: more than three of a copy's 32 KiB chunks, so that a
 * forward copy over itself would overwrite input its later chunks read. */
#define OVERLAP_SRC  "/tmp/callsmusl.src"
#define OVERLAP_DST  "/tmp/callsmusl.dst"
#define OVERLAP_SIZE (100 * 1024)

/* The bytes `spliced_once` puts in its pipe. */
#define SPLICED_SIZE 4096

/** The byte at `i` of the files and pipes of `overlaps`. */
static char pattern_byte(unsigned i)
{
    return (char)('a' + (i * 7 + i / 26) % 26);
}

/** Remove what `bind_overlap` made, whatever of it there is. */
static void bind_overlap_clean(void)
{
    umount(OVERLAP_DST);
    unlink(OVERLAP_SRC "/f");
    rmdir(OVERLAP_DST);
    rmdir(OVERLAP_SRC);
}

/**
 * Check that copy_file_range from one name of a file to another, through a
 * directory of the KRFFS root and a bind of it, whose mounts differ, of
 * ranges that overlap is EINVAL, as Linux answers for one inode, and leaves
 * the file as it was. Returns 0, or 1 after the failure is printed.
 */
static int bind_overlap(void)
{
    static char data[OVERLAP_SIZE];
    off_t at = 0, to = 4096;
    unsigned i;
    int in, out, wrong;

    for (i = 0; i < OVERLAP_SIZE; i++) {
        data[i] = pattern_byte(i);
    }
    bind_overlap_clean();
    if (mkdir(OVERLAP_SRC, 0755) != 0 || mkdir(OVERLAP_DST, 0755) != 0 ||
        mount(OVERLAP_SRC, OVERLAP_DST, NULL, MS_BIND, NULL) != 0) {
        bind_overlap_clean();
        return fail("a file of 100 KiB under a directory and a bind of it");
    }
    in = open(OVERLAP_SRC "/f", O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (in < 0 || write(in, data, OVERLAP_SIZE) != OVERLAP_SIZE ||
        (out = open(OVERLAP_DST "/f", O_WRONLY)) < 0) {
        if (in >= 0) {
            close(in);
        }
        bind_overlap_clean();
        return fail("a file of 100 KiB under a directory and a bind of it");
    }
    errno = 0;
    wrong = copy_file_range(in, &at, out, &to, OVERLAP_SIZE - 4096, 0) != -1 || errno != EINVAL;
    for (i = 0; !wrong && i < OVERLAP_SIZE; i += 4096) {
        char got[4096];

        wrong = pread(in, got, sizeof got, (off_t)i) != (ssize_t)sizeof got ||
                memcmp(got, data + i, sizeof got) != 0;
    }
    close(in);
    close(out);
    bind_overlap_clean();
    if (wrong) {
        return fail(
            "copy_file_range over itself through a bind was not EINVAL or changed the file");
    }
    held();
    return 0;
}

/**
 * The splicing child of `spliced_once`: splice the pipe's read end `from`
 * into the socket `to` and write the count it returned, an int, to `res`.
 * Never returns.
 */
static void splice_child(int from, int to, int res)
{
    int n = (int)splice(from, NULL, to, NULL, SPLICED_SIZE, 0);

    _exit(write(res, &n, sizeof n) == (ssize_t)sizeof n ? 0 : 1);
}

/**
 * The reading child of `spliced_once`: read the pipe's read end `from` to
 * its end and write the count, an int, and the bytes to `res`. Never
 * returns.
 */
static void read_child(int from, int res)
{
    char got[SPLICED_SIZE + 1];
    int n = 0;
    ssize_t r;

    while (n < (int)sizeof got && (r = read(from, got + n, sizeof got - (size_t)n)) > 0) {
        n += (int)r;
    }
    _exit(write(res, &n, sizeof n) == (ssize_t)sizeof n && write(res, got, (size_t)n) == (ssize_t)n
              ? 0
              : 1);
}

/** Wait for the child `pid` and return whether it exited with status 0. */
static int reaped_ok(pid_t pid)
{
    int status;

    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/** Read a child's count of bytes from its result pipe `fd` into `*n`, and
 * return whether a whole count came and is not negative. */
static int count_read(int fd, int *n)
{
    return read(fd, n, sizeof *n) == (ssize_t)sizeof *n && *n >= 0;
}

/**
 * Return whether the pipe's `data` went to the splicer and the reader each
 * once, in order: `spliced` bytes on the socket `sock`, which are the first,
 * and the `read_n` bytes after them on the reader's result pipe `rres`.
 */
static int spliced_bytes_ok(const char *data, int spliced, int read_n, int rres, int sock)
{
    char got[SPLICED_SIZE + 1];
    size_t left;
    ssize_t r;
    int ok;

    ok = spliced + read_n == SPLICED_SIZE && read(rres, got, (size_t)read_n) == (ssize_t)read_n &&
         memcmp(got, data + spliced, (size_t)read_n) == 0;
    for (left = (size_t)spliced; ok && left > 0; left -= (size_t)r) {
        r = read(sock, got, left);
        ok = r > 0 && memcmp(got, data + (spliced - (int)left), (size_t)r) == 0;
    }
    return ok;
}

/**
 * Check that a byte of a pipe goes to one reader. A child splices
 * SPLICED_SIZE bytes from the pipe into a socket that is full, and is seen
 * asleep with them in hand; a second child then reads the pipe to its end,
 * and once it has ended or is seen asleep the caller empties the socket. The
 * bytes the socket carries and those the reader got must be the pipe's,
 * each once, in order. Each child reports on a pipe of its own, since the
 * reader can see the end of the pipe and report before the splicer returns.
 * Returns 0, or 1 after the failure is printed.
 */
static int spliced_once(void)
{
    char data[SPLICED_SIZE], fill[4096], got[SPLICED_SIZE + 1];
    int p[2], sv[2], sres[2], rres[2], spliced = -1, read_n = -1;
    size_t filled = 0, left;
    ssize_t r;
    pid_t splicer, reader;
    unsigned i;
    int ok;

    for (i = 0; i < SPLICED_SIZE; i++) {
        data[i] = pattern_byte(i);
    }
    memset(fill, '#', sizeof fill);
    if (pipe(p) != 0 || pipe(sres) != 0 || pipe(rres) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0 ||
        write(p[1], data, SPLICED_SIZE) != SPLICED_SIZE || fcntl(sv[0], F_SETFL, O_NONBLOCK) != 0) {
        return fail("a pipe and a socket pair to splice it into");
    }
    close(p[1]);
    while ((r = write(sv[0], fill, sizeof fill)) > 0) {
        filled += (size_t)r;
    }
    if (errno != EAGAIN || fcntl(sv[0], F_SETFL, 0) != 0) {
        return fail("filling the socket");
    }
    splicer = fork();
    if (splicer == 0) {
        splice_child(p[0], sv[0], sres[1]);
    }
    if (splicer < 0 || !seen_asleep(splicer)) {
        return fail("the splice into a full socket did not sleep");
    }
    reader = fork();
    if (reader == 0) {
        read_child(p[0], rres[1]);
    }
    if (reader < 0) {
        return fail("fork of the reader");
    }
    seen_asleep(reader);
    for (left = filled; left > 0; left -= (size_t)r) {
        r = read(sv[1], got, left < sizeof got ? left : sizeof got);
        if (r <= 0) {
            return fail("reading back the socket's filling");
        }
    }
    ok = reaped_ok(splicer) && reaped_ok(reader);
    close(sv[0]);
    close(p[0]);
    close(sres[1]);
    close(rres[1]);
    if (!ok || !count_read(sres[0], &spliced) || !count_read(rres[0], &read_n)) {
        return fail("the splicer's and the reader's counts");
    }
    ok = spliced_bytes_ok(data, spliced, read_n, rres[0], sv[1]);
    close(sv[1]);
    close(sres[0]);
    close(rres[0]);
    if (!ok) {
        char why[96];

        snprintf(why, sizeof why, "the splice gave %d bytes and the reader %d of %d", spliced,
                 read_n, SPLICED_SIZE);
        return fail(why);
    }
    held();
    return 0;
}

/**
 * Check vmsplice in both directions as Linux's on the pipe `p`: into its
 * write end, the bytes of two buffers whole; out of its read end into three
 * buffers, a wait for the first byte, which a child writes once it sees the
 * caller asleep, and an end when the pipe is empty. On i686 each call passes
 * i386's 8-byte iovec, which the x86-64 kernel translates. Returns 0, or 1
 * after the failure is printed.
 */
static int vmsplice_moves(const int *p)
{
    char one[4], two[4], three[4], got[16];
    struct iovec iov[3];
    pid_t child;
    int status, n;

    iov[0].iov_base = "vmsp";
    iov[0].iov_len = 4;
    iov[1].iov_base = "lice";
    iov[1].iov_len = 4;
    if (vmsplice(p[1], iov, 2, 0) != 8 || read(p[0], got, sizeof got) != 8 ||
        memcmp(got, "vmsplice", 8) != 0) {
        return fail("vmsplice of two buffers into a pipe did not put their 8 bytes in it");
    }
    held();
    child = fork();
    if (child < 0) {
        return fail("fork");
    }
    if (child == 0) {
        _exit(seen_asleep(getppid()) && write(p[1], "abcde", 5) == 5 ? 0 : 1);
    }
    iov[0].iov_base = one;
    iov[0].iov_len = 4;
    iov[1].iov_base = two;
    iov[1].iov_len = 4;
    iov[2].iov_base = three;
    iov[2].iov_len = 4;
    n = (int)vmsplice(p[0], iov, 3, 0);
    if (waitpid(child, &status, 0) != child || status != 0 || n != 5 ||
        memcmp(one, "abcd", 4) != 0 || two[0] != 'e') {
        return fail("vmsplice out of a pipe into three buffers did not wait for its first byte "
                    "and stop at 5");
    }
    held();
    return 0;
}

/* The buffers of `vmsplices`' count past IOV_MAX, 1024 on Linux. */
#define VMSPLICE_PAST_MAX 1025

/**
 * Check vmsplice's moves (`vmsplice_moves`) and its refusals as Linux's:
 * SPLICE_F_NONBLOCK's EAGAIN on an empty pipe's read end and a full pipe's
 * write end, the pipe itself blocking; EBADF for a descriptor that is not a
 * pipe; and EINVAL for more than IOV_MAX buffers. Returns 0, or 1 after the
 * failure is printed.
 */
static int vmsplices(void)
{
    static struct iovec many[VMSPLICE_PAST_MAX];
    char got[16];
    struct iovec iov[1];
    int p[2], null, i;

    null = open("/dev/null", O_WRONLY);
    if (null < 0 || pipe(p) != 0) {
        return fail("a pipe and /dev/null");
    }
    if (vmsplice_moves(p) != 0) {
        return 1;
    }
    iov[0].iov_base = got;
    iov[0].iov_len = sizeof got;
    if (vmsplice(p[0], iov, 1, SPLICE_F_NONBLOCK) != -1 || errno != EAGAIN) {
        return fail("vmsplice with SPLICE_F_NONBLOCK out of an empty pipe was not EAGAIN");
    }
    fcntl(p[1], F_SETFL, O_NONBLOCK);
    while (write(p[1], got, sizeof got) > 0) {
    }
    fcntl(p[1], F_SETFL, 0);
    iov[0].iov_base = "x";
    iov[0].iov_len = 1;
    if (vmsplice(p[1], iov, 1, SPLICE_F_NONBLOCK) != -1 || errno != EAGAIN) {
        return fail("vmsplice with SPLICE_F_NONBLOCK into a full pipe was not EAGAIN");
    }
    held();
    if (vmsplice(null, iov, 1, 0) != -1 || errno != EBADF) {
        return fail("vmsplice into /dev/null was not EBADF");
    }
    held();
    close(p[0]);
    close(p[1]);
    for (i = 0; i < VMSPLICE_PAST_MAX; i++) {
        many[i].iov_base = "x";
        many[i].iov_len = 1;
    }
    if (pipe(p) != 0) {
        return fail("a second pipe");
    }
    if (vmsplice(p[1], many, VMSPLICE_PAST_MAX, 0) != -1 || errno != EINVAL) {
        return fail("vmsplice of more than IOV_MAX buffers was not EINVAL");
    }
    held();
    close(p[0]);
    close(p[1]);
    close(null);
    return 0;
}

/**
 * Check read(2) of a pipe into NULL as Linux's, whose vfs_read passes NULL
 * to the pipe since access_ok takes it: an empty nonblocking pipe is EAGAIN
 * and an ended one 0, each before the copy would fault. Returns 0, or 1
 * after the failure is printed.
 */
static int null_reads(void)
{
    char *volatile nowhere = NULL;
    int p[2];

    if (pipe2(p, O_NONBLOCK) != 0) {
        return fail("a nonblocking pipe");
    }
    errno = 0;
    if (read(p[0], nowhere, 1) != -1 || errno != EAGAIN) {
        return fail("a read of an empty nonblocking pipe into NULL was not EAGAIN");
    }
    held();
    close(p[1]);
    if (read(p[0], nowhere, 1) != 0) {
        return fail("a read of an ended pipe into NULL did not return 0");
    }
    held();
    close(p[0]);
    return 0;
}

/** The rows of copies between pipes and files past `copies`: those of
 * `bind_overlap`, `spliced_once`, `vmsplices` and `null_reads`. Returns 0,
 * or 1 after the failure is printed. */
static int overlaps(void)
{
    return bind_overlap() || spliced_once() || vmsplices() || null_reads();
}

/**
 * Check that a pty master reads the slave's last output and then EIO once
 * the slave has closed, as Linux's does. Returns 0, or 1 after the failure
 * is printed.
 */
static int hangups(void)
{
    char name[32], got[8];
    struct termios t;
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    int slave;

    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 ||
        ptsname_r(master, name, sizeof name) != 0 || (slave = open(name, O_RDWR | O_NOCTTY)) < 0 ||
        tcgetattr(slave, &t) != 0) {
        return fail("a pseudo-terminal pair");
    }
    cfmakeraw(&t);
    tcsetattr(slave, TCSANOW, &t);
    if (write(slave, "ok", 2) != 2) {
        return fail("a slave write");
    }
    close(slave);
    if (read(master, got, sizeof got) != 2 || read(master, got, sizeof got) != -1 || errno != EIO) {
        return fail("a pty master did not read EIO after the slave's output once it closed");
    }
    held();
    close(master);
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
 * Check absent extended attributes on a regular file, and
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
    if (getxattr(file, "user.x", v, sizeof v) != -1 || (errno != EOPNOTSUPP && errno != ENODATA)) {
        return fail("getxattr of a missing attribute was not EOPNOTSUPP or ENODATA");
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

/** The processor time on `clock` in nanoseconds, or -1 when it cannot be
 * read. */
static long long cpu_ns(clockid_t clock)
{
    struct timespec ts;

    if (clock_gettime(clock, &ts) != 0) {
        return -1;
    }
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/** The process's user time in microseconds, as getrusage reports it. */
static long long user_us(void)
{
    struct rusage ru;

    getrusage(RUSAGE_SELF, &ru);
    return (long long)ru.ru_utime.tv_sec * 1000000LL + ru.ru_utime.tv_usec;
}

/* The signals `on_first_tick` took, and the process's processor time in
 * nanoseconds and its user time in microseconds at the first. */
static volatile sig_atomic_t firsts;
static volatile long long first_cpu_ns, first_user_us;

/** Note the process's times at the first SIGPROF or SIGVTALRM. */
static void on_first_tick(int signo)
{
    (void)signo;
    if (firsts++ == 0) {
        first_cpu_ns = cpu_ns(CLOCK_PROCESS_CPUTIME_ID);
        first_user_us = user_us();
    }
}

/** The process's processor time in nanoseconds, or for `virt` its user
 * time, which ITIMER_VIRTUAL counts. */
static long long itimer_time(int virt)
{
    return virt ? user_us() * 1000 : cpu_ns(CLOCK_PROCESS_CPUTIME_ID);
}

/**
 * Check that ITIMER_PROF, or ITIMER_VIRTUAL for `virt`, set to 10 ms expires
 * at the tick charged that passes 10 ms and a tick more, as Linux's
 * set_cpu_itimer adds TICK_NSEC to the first value: at 250 Hz the fourth
 * tick, where 10 ms alone is the third. `tick` is the length of a tick of
 * processor time in nanoseconds. The ticks are counted in the time the
 * timer counts, from a setting that no tick came during to the first
 * signal, which comes at the return from the tick that ran the timer out;
 * one more is allowed for a tick that came before the handler read the
 * time. Returns 0, or 1 after the failure is printed.
 */
static int itimer_first(int virt, long long tick)
{
    struct itimerval once = {{0, 0}, {0, 10000}}, off = {{0, 0}, {0, 0}};
    int which = virt ? ITIMER_VIRTUAL : ITIMER_PROF, signo = virt ? SIGVTALRM : SIGPROF;
    long long want = (10000000LL + tick + tick - 1) / tick, before = 0, ticks;
    int tries;
    static char why[160];

    signal(signo, on_first_tick);
    for (tries = 0; tries < 100; tries++) {
        firsts = 0;
        before = itimer_time(virt);
        if (setitimer(which, &once, NULL) != 0) {
            return fail("setitimer of 10 ms");
        }
        if (itimer_time(virt) == before) {
            break;
        }
        setitimer(which, &off, NULL);
    }
    while (firsts == 0 && itimer_time(virt) - before < 3000000000LL) {
        spin_user();
    }
    setitimer(which, &off, NULL);
    signal(signo, SIG_DFL);
    ticks = ((virt ? first_user_us * 1000 : first_cpu_ns) - before) / tick;
    if (tries == 100 || firsts == 0 || ticks < want || ticks > want + 1) {
        snprintf(why, sizeof why, "%s of 10 ms expired after %lld ticks charged, not %lld",
                 virt ? "ITIMER_VIRTUAL" : "ITIMER_PROF", firsts ? ticks : -1LL, want);
        return fail(why);
    }
    return 0;
}

/**
 * Check that a POSIX timer of the process's processor time set to an
 * absolute time 35 ms past, with an interval of 10 ms, expires during
 * timer_settime, though the process then only sleeps, and that its
 * signal's overrun counts the intervals since that time: three, or more
 * for ticks charged before the signal is taken; timer_gettime then reports
 * no more than the interval left. Linux's posix_cpu_timer_set fires such a
 * timer at once and bump_cpu_timer forwards it from its expiry. Returns 0,
 * or 1 after the failure is printed.
 */
static int ptimer_past(void)
{
    struct itimerspec set = {{0, 10000000}, {0, 0}}, cur;
    struct timespec nap = {0, 30000000}, zero = {0, 0};
    struct sigevent ev;
    sigset_t one, mask, pending;
    siginfo_t info, rest;
    timer_t timer;
    long long now, at, after;
    int got = -1;

    sigemptyset(&one);
    sigaddset(&one, SIGUSR1);
    sigprocmask(SIG_BLOCK, &one, &mask);
    memset(&ev, 0, sizeof ev);
    ev.sigev_notify = SIGEV_SIGNAL;
    ev.sigev_signo = SIGUSR1;
    if (timer_create(CLOCK_PROCESS_CPUTIME_ID, &ev, &timer) != 0) {
        return fail("timer_create on the process's clock of processor time");
    }
    do {
        spin_user();
        now = cpu_ns(CLOCK_PROCESS_CPUTIME_ID);
    } while (now < 50000000LL);
    at = now - 35000000LL;
    set.it_value.tv_sec = (time_t)(at / 1000000000LL);
    set.it_value.tv_nsec = (long)(at % 1000000000LL);
    if (timer_settime(timer, TIMER_ABSTIME, &set, NULL) != 0) {
        return fail("timer_settime of a time of processor time past");
    }
    /* interval: the process sleeps, so no tick charges it processor time */
    nanosleep(&nap, NULL);
    sigpending(&pending);
    if (sigismember(&pending, SIGUSR1)) {
        got = sigtimedwait(&one, &info, &zero);
    }
    after = cpu_ns(CLOCK_PROCESS_CPUTIME_ID);
    if (timer_gettime(timer, &cur) != 0) {
        return fail("timer_gettime of a timer set to a time past");
    }
    timer_delete(timer);
    while (sigtimedwait(&one, &rest, &zero) == SIGUSR1) {
    }
    sigprocmask(SIG_SETMASK, &mask, NULL);
    if (got != SIGUSR1) {
        return fail("a timer of processor time set to a time past did not expire as the process "
                    "slept");
    }
    if (info.si_code != SI_TIMER || info.si_overrun < (now - at) / 10000000LL ||
        info.si_overrun > (after - at) / 10000000LL) {
        return fail("the overrun of a timer set to a time past did not count its intervals since");
    }
    if (cur.it_value.tv_sec != 0 || cur.it_value.tv_nsec <= 0 || cur.it_value.tv_nsec > 10000000) {
        return fail("timer_gettime of a timer set to a time past reported more than its interval");
    }
    return 0;
}

/* The pipes `timer_thread` reports its timer on and waits on, and the timer
 * it made, with whether it made one. */
static int made_pipe[2], go_pipe[2];
static timer_t made_timer;
static int made_ok;

/** Make a timer of SIGEV_NONE on this thread's clock of processor time,
 * say so, and wait without running until told to end. */
static void *timer_thread(void *arg)
{
    struct sigevent ev;
    char c = 0;

    (void)arg;
    memset(&ev, 0, sizeof ev);
    ev.sigev_notify = SIGEV_NONE;
    made_ok = timer_create(CLOCK_THREAD_CPUTIME_ID, &ev, &made_timer) == 0;
    if (write(made_pipe[1], &c, 1) != 1 || read(go_pipe[0], &c, 1) != 1) {
        made_ok = 0;
    }
    return NULL;
}

/**
 * Check that timer_settime of a timer on another thread's clock of
 * processor time counts an absolute time from that thread's time, not the
 * caller's, as Linux's posix_cpu_timer_set samples the timer's own task:
 * with the caller past the time and the timer's thread, which only waits,
 * short of it, timer_gettime reports what that thread has left. Returns 0,
 * or 1 after the failure is printed.
 */
static int ptimer_other_thread(void)
{
    struct itimerspec set = {{0, 0}, {0, 0}}, cur;
    pthread_t thread;
    clockid_t clock;
    long long mine, theirs, theirs_after, at, left;
    char c = 0;
    int bad;

    if (pipe(made_pipe) != 0 || pipe(go_pipe) != 0) {
        return fail("pipe for the timer's thread");
    }
    if (pthread_create(&thread, NULL, timer_thread, NULL) != 0) {
        return fail("pthread_create of the timer's thread");
    }
    if (read(made_pipe[0], &c, 1) != 1 || !made_ok || pthread_getcpuclockid(thread, &clock) != 0) {
        return fail("timer_create on another thread's clock of processor time");
    }
    do {
        spin_user();
        mine = cpu_ns(CLOCK_THREAD_CPUTIME_ID);
    } while (mine < 100000000LL);
    at = mine / 2;
    theirs = cpu_ns(clock);
    set.it_value.tv_sec = (time_t)(at / 1000000000LL);
    set.it_value.tv_nsec = (long)(at % 1000000000LL);
    bad = theirs < 0 || theirs >= at || timer_settime(made_timer, TIMER_ABSTIME, &set, NULL) != 0 ||
          timer_gettime(made_timer, &cur) != 0;
    theirs_after = cpu_ns(clock);
    write(go_pipe[1], &c, 1);
    pthread_join(thread, NULL);
    timer_delete(made_timer);
    close(made_pipe[0]);
    close(made_pipe[1]);
    close(go_pipe[0]);
    close(go_pipe[1]);
    if (bad) {
        return fail("timer_settime of a time on another thread's clock of processor time");
    }
    left = (long long)cur.it_value.tv_sec * 1000000000LL + cur.it_value.tv_nsec;
    if (left < at - theirs_after || left > at - theirs) {
        return fail("timer_settime counted a time on another thread's clock from the caller's");
    }
    return 0;
}

/** Write `text` to the file `path`; 0, or -1 with errno set. */
static int write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    ssize_t n;

    if (fd < 0) {
        return -1;
    }
    n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

/** Read the three numbers of debugfs's cputime_test into `v`; 0, or -1. */
static int cputime_test_read(long v[3])
{
    char line[96];
    int fd = open("/sys/kernel/debug/cputime_test", O_RDONLY);
    ssize_t n = fd < 0 ? -1 : read(fd, line, sizeof line - 1);

    if (fd >= 0) {
        close(fd);
    }
    if (n <= 0) {
        return -1;
    }
    line[n] = 0;
    return sscanf(line, "%ld %ld %ld", &v[0], &v[1], &v[2]) == 3 ? 0 : -1;
}

/* The settings of `ptimer_tick_at_arming`, each of which cputime_test
 * holds for two ticks between its sample and its arming. */
#define ARM_WAITS 25

/**
 * Check that a tick charged while timer_settime samples the clock and arms
 * the timer is either in the sample or taken off the time left, as Linux's
 * tick compares the clock itself with the timer's absolute expiry: a one-shot
 * timer of SIGEV_NONE on the process's clock, set again and again to a
 * nanosecond past the clock, must have expired whenever the clock read after
 * the setting has reached that time. debugfs's cputime_test holds each of
 * ARM_WAITS settings for two ticks between its sample and its arming, so
 * that a tick is charged there, and counts those whose clock moved by the
 * arming's end, which must be all of them; the tick it held is charged as
 * the setting lets the timers' lock go, so no later tick comes before the
 * clock is read. Returns 0, or 1 after the failure is printed.
 */
static int ptimer_tick_at_arming(void)
{
    struct itimerspec set = {{0, 0}, {0, 0}}, cur;
    struct sigevent ev;
    timer_t timer;
    long long now, at;
    long counts[3];
    char text[24];
    int i;

    memset(&ev, 0, sizeof ev);
    ev.sigev_notify = SIGEV_NONE;
    if (timer_create(CLOCK_PROCESS_CPUTIME_ID, &ev, &timer) != 0) {
        return fail("timer_create of SIGEV_NONE on the process's clock of processor time");
    }
    snprintf(text, sizeof text, "arm %d\n", ARM_WAITS);
    if (write_text("/sys/kernel/debug/cputime_test", text) != 0) {
        timer_delete(timer);
        return fail("write of arm to /sys/kernel/debug/cputime_test");
    }
    now = cpu_ns(CLOCK_PROCESS_CPUTIME_ID);
    for (i = 0; i < ARM_WAITS; i++) {
        at = now + 1;
        set.it_value.tv_sec = (time_t)(at / 1000000000LL);
        set.it_value.tv_nsec = (long)(at % 1000000000LL);
        if (timer_settime(timer, TIMER_ABSTIME, &set, NULL) != 0) {
            timer_delete(timer);
            return fail("timer_settime of a nanosecond past the process's clock");
        }
        now = cpu_ns(CLOCK_PROCESS_CPUTIME_ID);
        if (timer_gettime(timer, &cur) != 0) {
            timer_delete(timer);
            return fail("timer_gettime of a timer of processor time");
        }
        if (now >= at && (cur.it_value.tv_sec != 0 || cur.it_value.tv_nsec != 0)) {
            timer_delete(timer);
            return fail("a tick between the sample and the arming of a timer of processor time "
                        "reached its time and left it armed");
        }
    }
    timer_delete(timer);
    if (cputime_test_read(counts) != 0) {
        return fail("read of /sys/kernel/debug/cputime_test");
    }
    if (counts[0] != 0 || counts[1] != ARM_WAITS) {
        errno = 0;
        return fail("cputime_test did not land a tick between each setting's sample and arming");
    }
    return 0;
}

/* The pipes the threads of `ptimer_reused_tid` report their ids on and
 * wait on. */
static int tid_pipe[2], tid_go[2];

/** Report this thread's id, spin in user mode for `arg`, a pointer to a
 * long long of nanoseconds of this thread's processor time, when it is not
 * NULL, report again, and wait to be told to end. */
static void *tid_thread(void *arg)
{
    pid_t tid = (pid_t)syscall(SYS_gettid);
    char c = 0;

    if (write(tid_pipe[1], &tid, sizeof tid) != (ssize_t)sizeof tid) {
        return NULL;
    }
    if (arg) {
        while (cpu_ns(CLOCK_THREAD_CPUTIME_ID) < *(long long *)arg) {
            spin_user();
        }
        if (write(tid_pipe[1], &tid, sizeof tid) != (ssize_t)sizeof tid) {
            return NULL;
        }
    }
    if (read(tid_go[0], &c, 1) != 1) {
        return NULL;
    }
    return NULL;
}

/**
 * Check that a timer on a thread's clock of processor time stays with that
 * thread once it is gone and another thread is given its id, as Linux's
 * timer keeps the struct pid, which the new thread does not share: armed on
 * a thread that only waits, which then ends and is reaped, it reports
 * nothing and a setting fails with ESRCH, and it posts no signal, while a
 * thread made with the same id, through /proc/sys/kernel/ns_last_pid,
 * spins for 60 ms of processor time. Returns 0, or 1 after the failure is
 * printed.
 */
static int ptimer_reused_tid(void)
{
    struct itimerspec soon = {{0, 0}, {0, 20000000}}, cur;
    struct timespec zero = {0, 0};
    long long spin = 60000000LL;
    struct sigevent ev;
    sigset_t one, mask, pending;
    siginfo_t info;
    pthread_t first, second;
    clockid_t clock;
    timer_t timer;
    pid_t old_tid = 0, new_tid = 0, again = 0;
    char c = 0, text[24];
    int tries, set_rc, set_errno, got, fired;

    sigemptyset(&one);
    sigaddset(&one, SIGUSR2);
    sigprocmask(SIG_BLOCK, &one, &mask);
    if (pipe(tid_pipe) != 0 || pipe(tid_go) != 0) {
        return fail("pipe for the timer's threads");
    }
    if (pthread_create(&first, NULL, tid_thread, NULL) != 0 ||
        read(tid_pipe[0], &old_tid, sizeof old_tid) != (ssize_t)sizeof old_tid ||
        pthread_getcpuclockid(first, &clock) != 0) {
        return fail("the first thread of the reused id");
    }
    memset(&ev, 0, sizeof ev);
    ev.sigev_notify = SIGEV_SIGNAL;
    ev.sigev_signo = SIGUSR2;
    if (timer_create(clock, &ev, &timer) != 0 || timer_settime(timer, 0, &soon, NULL) != 0) {
        return fail("timer_create and timer_settime on the first thread's clock");
    }
    write(tid_go[1], &c, 1);
    pthread_join(first, NULL);
    for (tries = 0; tries < 10000; tries++) {
        if (timer_gettime(timer, &cur) == 0 && cur.it_value.tv_sec == 0 &&
            cur.it_value.tv_nsec == 0) {
            break;
        }
        sched_yield();
    }
    if (tries == 10000) {
        return fail("the first thread of the reused id was not seen to be reaped");
    }
    snprintf(text, sizeof text, "%d\n", (int)old_tid - 1);
    if (write_text("/proc/sys/kernel/ns_last_pid", text) != 0) {
        return fail("write of /proc/sys/kernel/ns_last_pid");
    }
    if (pthread_create(&second, NULL, tid_thread, &spin) != 0 ||
        read(tid_pipe[0], &new_tid, sizeof new_tid) != (ssize_t)sizeof new_tid ||
        read(tid_pipe[0], &again, sizeof again) != (ssize_t)sizeof again) {
        return fail("the second thread of the reused id");
    }
    errno = 0;
    set_rc = timer_settime(timer, 0, &soon, NULL);
    set_errno = errno;
    memset(&cur, 0xff, sizeof cur);
    got = timer_gettime(timer, &cur);
    sigpending(&pending);
    fired = sigismember(&pending, SIGUSR2);
    write(tid_go[1], &c, 1);
    pthread_join(second, NULL);
    timer_delete(timer);
    while (sigtimedwait(&one, &info, &zero) == SIGUSR2) {
    }
    sigprocmask(SIG_SETMASK, &mask, NULL);
    close(tid_pipe[0]);
    close(tid_pipe[1]);
    close(tid_go[0]);
    close(tid_go[1]);
    if (new_tid != old_tid) {
        errno = 0;
        return fail("ns_last_pid did not give the first thread's id to the second");
    }
    if (fired) {
        errno = 0;
        return fail(
            "a timer on a gone thread's clock expired on the time of a thread given its id");
    }
    if (got != 0 || cur.it_value.tv_sec != 0 || cur.it_value.tv_nsec != 0) {
        return fail("timer_gettime on a gone thread's clock reported a setting");
    }
    if (set_rc != -1 || set_errno != ESRCH) {
        return fail("timer_settime on a gone thread's clock did not fail with ESRCH");
    }
    return 0;
}

/**
 * Check that a timer on a child's clock of processor time can be set while
 * the child is a zombie, and that once the child is reaped timer_settime
 * fails with ESRCH, a disarming one too, and timer_gettime reports nothing,
 * as Linux's posix_cpu_timer_set and posix_cpu_timer_get find no task.
 * Returns 0, or 1 after the failure is printed.
 */
static int ptimer_reaped(void)
{
    struct itimerspec second = {{0, 0}, {1, 0}}, off = {{0, 0}, {0, 0}}, cur;
    struct sigevent ev;
    clockid_t clock;
    timer_t timer;
    siginfo_t info;
    int fds[2], status, zombie_set, gone_set, gone_errno, off_set, off_errno, got;
    pid_t child;
    char c;

    if (pipe(fds) != 0) {
        return fail("pipe for the timer's child");
    }
    child = fork();
    if (child < 0) {
        return fail("fork of the timer's child");
    }
    if (child == 0) {
        close(fds[1]);
        _exit(read(fds[0], &c, 1) == 0 ? 0 : 1);
    }
    close(fds[0]);
    memset(&ev, 0, sizeof ev);
    ev.sigev_notify = SIGEV_NONE;
    if (clock_getcpuclockid(child, &clock) != 0 || timer_create(clock, &ev, &timer) != 0) {
        close(fds[1]);
        waitpid(child, &status, 0);
        return fail("timer_create on a child's clock of processor time");
    }
    close(fds[1]);
    memset(&info, 0, sizeof info);
    if (waitid(P_PID, (id_t)child, &info, WEXITED | WNOWAIT) != 0) {
        timer_delete(timer);
        return fail("waitid of the timer's child");
    }
    zombie_set = timer_settime(timer, 0, &second, NULL) == 0 && timer_gettime(timer, &cur) == 0 &&
                 (cur.it_value.tv_sec != 0 || cur.it_value.tv_nsec != 0);
    waitpid(child, &status, 0);
    errno = 0;
    gone_set = timer_settime(timer, 0, &second, NULL);
    gone_errno = errno;
    errno = 0;
    off_set = timer_settime(timer, 0, &off, NULL);
    off_errno = errno;
    memset(&cur, 0xff, sizeof cur);
    got = timer_gettime(timer, &cur);
    timer_delete(timer);
    if (!zombie_set) {
        return fail("timer_settime on the clock of a zombie child");
    }
    if (gone_set != -1 || gone_errno != ESRCH || off_set != -1 || off_errno != ESRCH) {
        return fail("timer_settime on the clock of a reaped child did not fail with ESRCH");
    }
    if (got != 0 || cur.it_value.tv_sec != 0 || cur.it_value.tv_nsec != 0 ||
        cur.it_interval.tv_sec != 0 || cur.it_interval.tv_nsec != 0) {
        return fail("timer_gettime on the clock of a reaped child reported a setting");
    }
    return 0;
}

/* The leader's timer on its own clock of processor time and its process id,
 * for `leader_gone`. */
static timer_t leader_timer;
static pid_t leader_pid;

/**
 * Wait until the leader has ended, as its state in /proc shows, and set its
 * timer: the process's status is 0 when the setting succeeds and
 * timer_gettime then reports the time left, 2 when it fails, and 3 when the
 * leader is not seen to end.
 */
static void *leader_gone(void *arg)
{
    struct itimerspec second = {{0, 0}, {1, 0}}, cur;
    char path[64], line[256], *paren;
    int tries, fd;
    ssize_t n;

    (void)arg;
    snprintf(path, sizeof path, "/proc/self/task/%d/stat", (int)leader_pid);
    for (tries = 0; tries < 100000; tries++) {
        fd = open(path, O_RDONLY);
        n = fd < 0 ? -1 : read(fd, line, sizeof line - 1);
        if (fd >= 0) {
            close(fd);
        }
        if (n > 0) {
            line[n] = 0;
            paren = strrchr(line, ')');
            if (paren && paren[1] == ' ' && paren[2] == 'Z') {
                break;
            }
        }
        sched_yield();
    }
    if (tries == 100000) {
        _exit(3);
    }
    if (timer_settime(leader_timer, 0, &second, NULL) != 0 ||
        timer_gettime(leader_timer, &cur) != 0 ||
        (cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0)) {
        _exit(2);
    }
    _exit(0);
    return NULL;
}

/**
 * Check that a timer on the clock of processor time of a leader that has
 * called pthread_exit can be set from another thread while the leader is a
 * zombie, as Linux's posix_cpu_timer_set finds the leader until it is
 * reaped. Returns 0, or 1 after the failure is printed.
 */
static int ptimer_zombie_leader(void)
{
    struct sigevent ev;
    pthread_t thread;
    pid_t child;
    int status;

    fflush(stdout);
    child = fork();
    if (child < 0) {
        return fail("fork of the leader's process");
    }
    if (child == 0) {
        memset(&ev, 0, sizeof ev);
        ev.sigev_notify = SIGEV_NONE;
        leader_pid = getpid();
        if (timer_create(CLOCK_THREAD_CPUTIME_ID, &ev, &leader_timer) != 0 ||
            pthread_create(&thread, NULL, leader_gone, NULL) != 0) {
            _exit(1);
        }
        pthread_exit(NULL);
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) == 1) {
        return fail("the leader's process did not make its timer and thread");
    }
    if (WEXITSTATUS(status) == 3) {
        return fail("the leader that called pthread_exit was not seen to end");
    }
    if (WEXITSTATUS(status) != 0) {
        return fail("timer_settime on the clock of a leader that called pthread_exit");
    }
    return 0;
}

/**
 * The rows of a setting of a timer of processor time: ITIMER_PROF's and
 * ITIMER_VIRTUAL's first expiry a tick later than asked (`itimer_first`),
 * a POSIX timer set to a time past (`ptimer_past`), one on another
 * thread's clock (`ptimer_other_thread`), a tick between a setting's sample
 * and arming (`ptimer_tick_at_arming`), a timer on a thread's clock once
 * another thread has its id (`ptimer_reused_tid`), one on a child's clock
 * once the child is reaped (`ptimer_reaped`), and one on a leader that called
 * pthread_exit (`ptimer_zombie_leader`). Returns 0, or 1 after the failure
 * is printed.
 */
static int cpu_settings(void)
{
    struct timespec res;
    long long tick;

    if (clock_getres(CLOCK_PROCESS_CPUTIME_ID, &res) != 0 || res.tv_sec != 0 || res.tv_nsec <= 0) {
        return fail("clock_getres of the process's clock of processor time");
    }
    tick = res.tv_nsec;
    if (itimer_first(0, tick) != 0 || itimer_first(1, tick) != 0) {
        return 1;
    }
    held();
    if (ptimer_past() != 0) {
        return 1;
    }
    held();
    if (ptimer_other_thread() != 0) {
        return 1;
    }
    held();
    if (ptimer_tick_at_arming() != 0) {
        return 1;
    }
    held();
    if (ptimer_reused_tid() != 0) {
        return 1;
    }
    held();
    if (ptimer_reaped() != 0) {
        return 1;
    }
    held();
    if (ptimer_zombie_leader() != 0) {
        return 1;
    }
    held();
    return 0;
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
    return cpu_settings();
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
 * Check the kernel's log through syslog(2): the ring's size, Linux's default
 * 131072 bytes, the last 16 KiB of the ring read as records that begin with
 * a priority, after a clear nothing of what came before, and /dev/log a link
 * to the syslog daemon's socket.
 * Returns 0, or 1 after the failure is printed.
 */
static int klogs(void)
{
    static char ring[16384];
    char link[32];
    int n;

    if (klogctl(10, NULL, 0) != 131072) {
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
#define RTC_AIE_ON    0x7001u
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

/** The seconds since the epoch of the struct rtc_time `tm`, a time in UTC. */
static time_t rtc_seconds(const int *tm)
{
    struct tm t;

    memset(&t, 0, sizeof t);
    t.tm_sec = tm[0];
    t.tm_min = tm[1];
    t.tm_hour = tm[2];
    t.tm_mday = tm[3];
    t.tm_mon = tm[4];
    t.tm_year = tm[5];
    return timegm(&t);
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
    at = rtc_seconds(w->tm) + ahead;
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

/** Check that freeze waits for the wake alarm while the spinner is held:
 * the clock has reached the alarm's time when the write returns. RTC_WKALM_RD
 * gives `pending` as 0, as on Linux, so the alarm is judged by the clock.
 * The spinner writes a byte to a pipe before it spins, and the freeze waits
 * for the byte. The caller owns the RTC descriptor `fd`; return 0, or report
 * a failure. */
static int freezes(int fd)
{
    struct wkalrm w, back;
    struct stat st;
    int started[2], spun, state, woke, now[9];
    long before, after;
    pid_t spinner;
    char c;

    if (pipe(started) != 0) {
        return fail("a pipe for the spinning child");
    }
    spinner = fork();
    if (spinner == 0) {
        if (write(started[1], "s", 1) != 1) {
            _exit(1);
        }
        for (;;) {
        }
    }
    close(started[1]);
    spun = spinner > 0 && read(started[0], &c, 1) == 1;
    close(started[0]);
    state = open("/sys/power/state", O_WRONLY);
    if (!spun || state < 0 || wake_in(fd, 2, &w) != 0) {
        return unfreeze(spinner, state, "a spinning child, /sys/power/state and the wake alarm");
    }
    if (fstat(state, &st) != 0 || (st.st_mode & 0777) != 0644) {
        return unfreeze(spinner, state, "the mode fstat gives /sys/power/state's descriptor");
    }
    held();
    before = cpu_ticks(spinner);
    if (ioctl(fd, RTC_WKALM_RD, &back) != 0 || !back.enabled) {
        return unfreeze(spinner, state, "the wake alarm came before suspend");
    }
    if (write(state, "freeze", 6) != 6) {
        return unfreeze(spinner, state, "a write of freeze to /sys/power/state");
    }
    after = cpu_ticks(spinner);
    woke = ioctl(fd, RTC_RD_TIME, now) == 0 && rtc_seconds(now) >= rtc_seconds(w.tm);
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

/** Whether the alarm read back in `back` has the date and time of `w`, the
 * second to the year. */
static int same_alarm_time(const struct wkalrm *back, const struct wkalrm *w)
{
    return memcmp(back->tm, w->tm, 6 * sizeof(int)) == 0;
}

/**
 * Check that RTC_WKALM_SET refuses with EINVAL each date and time Linux's
 * rtc_valid_tm refuses, from the alarm `w` set on /dev/rtc0, open as `fd`,
 * and keeps that alarm, which then reads back enabled with its time. `w` is
 * an hour ahead, so that it cannot come meanwhile; `pending` is not read,
 * since Linux gives it as 0. Returns 0, or 1 after the failure is printed.
 */
static int refuses_bad_alarms(int fd, const struct wkalrm *w)
{
    /* The field of struct rtc_time each case changes, and its value: a
       second, minute and hour past their range, day 0, February 31, month 12,
       a year before 1970 and one past INT_MAX - 1900. */
    static const int field[] = {0, 1, 2, 3, 3, 4, 5, 5};
    static const int value[] = {60, -1, 24, 0, 31, 12, 69, 0x7fffffff - 1899};
    struct wkalrm bad, back;
    char why[80];
    unsigned i;

    for (i = 0; i < sizeof field / sizeof field[0]; i++) {
        bad = *w;
        bad.tm[field[i]] = value[i];
        if (field[i] == 3 && value[i] == 31) {
            bad.tm[4] = 1;
        }
        if (ioctl(fd, RTC_WKALM_SET, &bad) != -1 || errno != EINVAL) {
            snprintf(why, sizeof why, "RTC_WKALM_SET took field %d of struct rtc_time as %d",
                     field[i], value[i]);
            return fail(why);
        }
    }
    if (ioctl(fd, RTC_WKALM_RD, &back) != 0 || !back.enabled || !same_alarm_time(&back, w)) {
        return fail("a refused RTC_WKALM_SET did not keep the alarm set before it");
    }
    return 0;
}

/**
 * Check alarms past on /dev/rtc0, open as `fd`: one set comes, is then off
 * and reads back with its whole date and time, as Linux's rtc_timer_enqueue
 * raises it and keeps its expiry, and one two days ahead is EINVAL; and
 * RTC_AIE_ON with the alarm's time past raises it. Linux raises such an
 * alarm from work queued by the set or the ioctl (drivers/rtc/interface.c:
 * 858-861, 939-957), which may run after the call returns, so each check
 * waits for the alarm before it reads it. Returns 0, or 1 after the failure
 * is printed.
 */
static int past_alarms(int fd)
{
    struct wkalrm w, back;
    unsigned long data = 0;
    struct pollfd p;

    if (wake_in(fd, -10, &w) != 0 || read(fd, &data, sizeof data) != (ssize_t)sizeof data ||
        !(data & 0x20) || ioctl(fd, RTC_WKALM_RD, &back) != 0 || back.enabled ||
        !same_alarm_time(&back, &w)) {
        return fail("a wake alarm in the past did not come and keep its date and time");
    }
    if (wake_in(fd, 2 * 86400, &w) != -1 || errno != EINVAL) {
        return fail("a wake alarm two days ahead was taken");
    }
    held();
    p.fd = fd;
    p.events = POLLIN;
    p.revents = 0;
    if (wake_in(fd, -10, &w) != 0 || read(fd, &data, sizeof data) != (ssize_t)sizeof data ||
        ioctl(fd, RTC_AIE_ON, 0) != 0 || poll(&p, 1, -1) != 1 || !(p.revents & POLLIN) ||
        read(fd, &data, sizeof data) != (ssize_t)sizeof data || !(data & 0x20) ||
        ioctl(fd, RTC_WKALM_RD, &back) != 0 || back.enabled || !same_alarm_time(&back, &w)) {
        return fail("RTC_AIE_ON with an alarm past did not raise it");
    }
    held();
    return 0;
}

/**
 * Check the clock's wake alarm and suspend to idle: the alarms past
 * (`past_alarms`); one an hour ahead reads back enabled, and an invalid
 * date and time is EINVAL and keeps it; one two seconds ahead ends a read of
 * /dev/rtc0 with RTC_AF, after which /sys/class/rtc/rtc0/wakealarm reads
 * empty; the device may wake the machine; a descriptor open on
 * /sys/power/state has mode 0644, as its path does; and a write of freeze to
 * it, with the alarm set again, returns at the alarm with a spinning child
 * given no processor time meanwhile, which the kernel counts. Returns 0, or
 * 1 after the failure is printed.
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
    if (past_alarms(fd) != 0) {
        return 1;
    }
    if (wake_in(fd, 3600, &w) != 0 || ioctl(fd, RTC_WKALM_RD, &back) != 0 || !back.enabled ||
        !same_alarm_time(&back, &w)) {
        return fail("a wake alarm an hour ahead did not read back");
    }
    held();
    if (refuses_bad_alarms(fd, &w) != 0) {
        return 1;
    }
    held();
    if (wake_in(fd, 2, &w) != 0 || read(fd, &data, sizeof data) != (ssize_t)sizeof data ||
        !(data & 0x20) || data >> 8 < 1) {
        return fail("a read of /dev/rtc0 did not end at the alarm with RTC_AF");
    }
    held();
    state = open("/sys/class/rtc/rtc0/wakealarm", O_RDONLY);
    if (state < 0 || read(state, text, sizeof text) != 0) {
        return fail("the wakealarm file did not read empty after the alarm came");
    }
    close(state);
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

/* The rows checked after `adjtimex`, in their order. */
static const char *const after_rows[] = {"clock-adjtime",
                                         "listxattr",
                                         "getxattr-absent",
                                         "lsetxattr-enoent",
                                         "readahead-fadvise",
                                         "fadvise-espipe",
                                         "noctty-session",
                                         "maxrss-self",
                                         "maxrss-child",
                                         "udp-refused",
                                         "setgid-file",
                                         "setgid-dir",
                                         "privacy-other-root",
                                         "privacy-own-maps",
                                         "privacy-kept-dirs",
                                         "privacy-kept-opens",
                                         "privacy-dumpable",
                                         "privacy-kept-files",
                                         "privacy-dev-list",
                                         "privacy-kept-links",
                                         "privacy-fchdir-owner",
                                         "privacy-fd-walk",
                                         "privacy-link-walks",
                                         "fd-size-reserved",
                                         "timer-si-timer",
                                         "timer-overrun",
                                         "timer-delete",
                                         "sigqueue",
                                         "rt-sigqueueinfo",
                                         "pty-nonblock-eagain",
                                         "pty-writer-sleeps",
                                         "pty-64k",
                                         "pty-whole-lines",
                                         "signalfd-eagain",
                                         "signalfd-read",
                                         "itimer-interval",
                                         "itimer-signals",
                                         "ptimer-gettime",
                                         "ptimer-signals",
                                         "itimer-first-tick",
                                         "ptimer-past-due",
                                         "ptimer-other-thread",
                                         "ptimer-tick-at-arming",
                                         "ptimer-reused-tid",
                                         "ptimer-reaped-esrch",
                                         "ptimer-zombie-leader",
                                         "devmem-low",
                                         "devmem-ram-eperm",
                                         "devmem-private-einval",
                                         "devmem-apic",
                                         "klog-size",
                                         "klog-records",
                                         "klog-clear",
                                         "dev-log-link",
                                         "rtc-alarm-range",
                                         "rtc-aie-on-past",
                                         "rtc-alarm-readback",
                                         "rtc-alarm-invalid",
                                         "rtc-read-af",
                                         "rtc-wakealarm-empty",
                                         "rtc-wakeup-enabled",
                                         "power-state-mode",
                                         "freeze-wakes",
                                         "freeze-holds"};

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
                                         "splice-pipe-null",
                                         "splice-file-pipe",
                                         "tee-pipes",
                                         "sendfile-socket",
                                         "sendfile-file",
                                         "copy-file-range",
                                         "seek-data-hole",
                                         "ficlone-eopnotsupp",
                                         "faccessat2",
                                         "splice-ebadf",
                                         "copy-file-range-bind-overlap",
                                         "splice-readers-once",
                                         "vmsplice-write",
                                         "vmsplice-read",
                                         "vmsplice-nonblock",
                                         "vmsplice-ebadf",
                                         "vmsplice-einval",
                                         "pipe-read-null-eagain",
                                         "pipe-read-null-eof",
                                         "pty-hangup-eio",
                                         "fallocate-krffs",
                                         "fallocate-ebadf",
                                         "fallocate-einval",
                                         "fallocate-espipe",
                                         "fallocate-mode",
                                         "fallocate-proc",
                                         "fallocate-shm"};

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
    PLAN(after_rows);
}

int main(int argc, char **argv)
{
    probe_args(argc, argv);
    plan();
    if (signals() || memory() || identity() || messages() || clocks() || ranges() || copies() ||
        overlaps() || hangups() || reserves() || adjusts() || advises() || terminals() || peaks() ||
        refusals() || setgids() || privacies() || reserved_fds() || timers() || queues() ||
        flows() || signalfds() || cputimers() || devmems() || klogs() || rtcs()) {
        return 1;
    }
    report_end();
    printf("CALLSMUSL OK\n");
    return 0;
}
