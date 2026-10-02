/**
 * The calls GNU libc programs make that the process work of milestone 54
 * added.
 *
 * sigtimedwait takes a blocked signal the process sent itself and returns
 * its number and sender, takes a signal sent to the thread before a lower
 * one sent to the process, waits for an alarm under a timeout of more than
 * 2^31 ticks, and with no signal pending its timeout ends it with EAGAIN.
 * kill of a child that has ended and is not yet reaped succeeds, alone and
 * by its group, as on Linux, and fails with ESRCH once it is reaped. A child
 * that starts a session and opens a free pseudo-terminal slave with
 * O_NOCTTY does not take it, and opening it again without the flag makes it
 * the controlling terminal with the child's group in the foreground, the
 * Linux rule BusyBox's telnetd depends on. getrusage reports the memory a
 * process has touched as ru_maxrss in kilobytes, and wait4 that of a child
 * that touched more, as GNU time reads them. A UDP socket connected to a
 * local port no socket has polls POLLERR once the port unreachable comes
 * back, and its next receive is ECONNREFUSED, as glibc's resolver expects,
 * ten times in a row, since errors over the loopback are not rate-limited.
 * A POSIX timer's signal comes with SI_TIMER and its value, a periodic
 * timer's signal blocked through several periods comes once with the others
 * as its overrun, and a deleted timer's id is EINVAL. sigqueue gives its
 * value and SI_QUEUE to the signal it sends. A writer to a raw
 * pseudo-terminal slave whose master has not read waits for room, so the
 * master reads every byte of 64 KiB, and a non-blocking one takes EAGAIN;
 * two writers' lines reach the master each whole.
 * A signalfd reads a blocked signal sent to the process, with its sender,
 * and a sigqueue's value, and with none pending a non-blocking read is
 * EAGAIN.
 * A setgid directory gives a file made in it its group, and a directory
 * its group and the bit. A process of another user is refused the maps and
 * the descriptors of a root process under /proc, and reads its own. Its
 * /proc directory is root's while a change of ids leaves it not dumpable,
 * and its own once PR_SET_DUMPABLE makes it so, as on Linux, and so is a
 * descriptor open on its maps.
 * mlock, munlock, mlockall and munlockall accept a mapped range and refuse a
 * gap with ENOMEM; mincore reports a touched page resident.
 * getcpu reports CPU 0, and personality keeps and returns its value.
 * sendmmsg sends two datagrams in one call over an AF_UNIX pair and
 * recvmmsg receives both, each with its count. clock_gettime64 reads the
 * monotonic clock with 64-bit fields. close_range marks a range of
 * descriptors close-on-exec and closes it, reaches past the table's end,
 * and refuses a flag it does not take and a backward range. splice from a
 * pipe to /dev/null, tee and sendfile answer EINVAL, which GNU grep takes
 * for a descriptor it must read instead, and EBADF for a closed one.
 * posix_fallocate answers EOPNOTSUPP for a file open for writing, a /proc
 * file among them, after which glibc's writes the range itself, EBADF for
 * one open only for reading, EINVAL for a length of 0, ESPIPE for a pipe,
 * and EOPNOTSUPP for a mode Linux does not take before the check that the
 * descriptor is open for writing. adjtimex reads an unsynchronized clock
 * with its 10 ms tick, and clock_adjtime of CLOCK_MONOTONIC is EOPNOTSUPP.
 * The extended attribute calls find the file and answer as Linux does for a
 * file system without attributes: an empty list and EOPNOTSUPP to get one,
 * and ENOENT for a missing file. readahead and posix_fadvise accept a
 * regular file, and posix_fadvise refuses a pipe with ESPIPE.
 * setitimer's ITIMER_VIRTUAL and ITIMER_PROF post SIGVTALRM and SIGPROF as
 * the process spins, and getitimer reports the interval.
 *
 * Prints "CALLSMUSL OK" on success and "CALLSMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <dirent.h>
#include <poll.h>
#include <stdlib.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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

static int fail(const char *what)
{
    printf("CALLSMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

/* Write a byte to each page of `n` bytes at `p`, through a volatile pointer
   so the compiler cannot drop stores nothing reads. */
static void touch(char *p, long n)
{
    volatile char *v = p;
    long i;

    for (i = 0; i < n; i += 4096) {
        v[i] = 1;
    }
}

/* ru_maxrss, in kilobytes, of the caller and of a child it waits for. */
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
    free(mine);
    return 0;
}

/* Ten datagrams to a closed local port, each answered with a port
   unreachable, since errors over the loopback are not rate-limited. */
static int refusals(void)
{
    struct sockaddr_in a;
    struct pollfd p;
    char c;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int i;

    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(47);
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
    close(fd);
    return 0;
}

/* POSIX timers on SIGUSR2, which stays blocked and is taken by sigtimedwait. */
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
    its.it_interval.tv_nsec = 20000000;
    if (timer_settime(t, 0, &its, NULL) != 0) {
        return fail("timer_settime of a periodic timer");
    }
    usleep(210000);
    if (sigtimedwait(&set, &si, &wait) != SIGUSR2 || si.si_overrun < 3 || timer_getoverrun(t) < 3) {
        return fail("a blocked periodic timer's signal did not carry its overrun");
    }
    if (timer_delete(t) != 0 || syscall(SYS_timer_delete, (long)t) != -1 || errno != EINVAL) {
        return fail("timer_delete, then again");
    }
    while (sigtimedwait(&set, &si, &zero) == SIGUSR2) {
    }
    return 0;
}

/* sigqueue to the caller's own process, taken by sigtimedwait. */
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
    return 0;
}

/* A non-blocking signalfd of SIGUSR2, which stays blocked. */
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
    v.sival_int = 31;
    if (kill(getpid(), SIGUSR2) != 0 || read(fd, &si, sizeof si) != (int)sizeof si ||
        si.ssi_signo != SIGUSR2 || si.ssi_pid != (uint32_t)getpid() ||
        sigqueue(getpid(), SIGUSR2, v) != 0 || read(fd, &si, sizeof si) != (int)sizeof si ||
        si.ssi_int != 31) {
        return fail("a signalfd did not read the signal sent, its sender and sigqueue's value");
    }
    close(fd);
    return 0;
}

/* Files made in a setgid directory of group 4242. */
static int setgids(void)
{
    const char *dir = "/tmp/callsmusl.sg";
    struct stat st;
    int fd;

    unlink("/tmp/callsmusl.sg/f");
    rmdir("/tmp/callsmusl.sg/d");
    rmdir(dir);
    if (mkdir(dir, 0775) != 0 || chown(dir, (uid_t)-1, 4242) != 0 || chmod(dir, 02775) != 0) {
        return fail("a setgid directory of group 4242 in /tmp");
    }
    fd = open("/tmp/callsmusl.sg/f", O_CREAT | O_WRONLY, 0644);
    if (fd < 0 || close(fd) != 0 || stat("/tmp/callsmusl.sg/f", &st) != 0 || st.st_gid != 4242) {
        return fail("a file made in a setgid directory did not take its group");
    }
    if (mkdir("/tmp/callsmusl.sg/d", 0755) != 0 || stat("/tmp/callsmusl.sg/d", &st) != 0 ||
        st.st_gid != 4242 || !(st.st_mode & S_ISGID)) {
        return fail("a directory made in a setgid directory did not take its group and the bit");
    }
    unlink("/tmp/callsmusl.sg/f");
    rmdir("/tmp/callsmusl.sg/d");
    rmdir(dir);
    return 0;
}

/* A child that becomes user 4243 opens its root parent's maps and fd
   directory, which are EACCES, and its own maps; its exit status is the
   number of the first that went wrong. */
static int privacies(void)
{
    char path[64];
    struct stat st;
    int status, fd;
    pid_t pid, parent = getpid();

    pid = fork();
    if (pid == 0) {
        DIR *d;

        if (setuid(4243) != 0) {
            _exit(1);
        }
        snprintf(path, sizeof path, "/proc/%d/maps", (int)parent);
        if (open(path, O_RDONLY) != -1 || errno != EACCES) {
            _exit(2);
        }
        snprintf(path, sizeof path, "/proc/%d/fd", (int)parent);
        d = opendir(path);
        if (d || errno != EACCES) {
            _exit(3);
        }
        fd = open("/proc/self/maps", O_RDONLY);
        if (fd < 0) {
            _exit(4);
        }
        if (stat("/proc/self/", &st) != 0 || st.st_uid != 0 ||
            prctl(PR_SET_DUMPABLE, 1, 0, 0, 0) != 0 || stat("/proc/self/", &st) != 0 ||
            st.st_uid != 4243 || stat("/proc/self/status", &st) != 0 || st.st_uid != 4243 ||
            fstat(fd, &st) != 0 || st.st_uid != 4243) {
            _exit(5);
        }
        _exit(0);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return fail("a child of user 4243");
    }
    if (WEXITSTATUS(status) == 2 || WEXITSTATUS(status) == 3) {
        return fail("another user opened a root process's maps or fd directory");
    }
    if (WEXITSTATUS(status) == 5) {
        return fail("the /proc directory of a dumpable process of user 4243 was not its own");
    }
    if (WEXITSTATUS(status) != 0) {
        return fail("a child of user 4243 could not become it or read its own maps");
    }
    return 0;
}

/* Two children write 20000 lines of seven letters to the slave `name`, one
   write a line, of 'a' and of 'b', through output processing, which turns
   each newline into two bytes: every line reaches the master `master`
   whole, as Linux's terminal write lock keeps a write from being cut by
   another's. 0, or -1 when a line was mixed or a child failed. */
static int whole_writes(int master, const char *name)
{
    char got[4096], line[8], cur = 0;
    int i, n, col = 0, bad = 0, status;
    int slave = open(name, O_RDWR | O_NOCTTY);
    struct termios t;
    pid_t pids[2];

    if (slave < 0 || tcgetattr(slave, &t) != 0) {
        return -1;
    }
    t.c_oflag |= OPOST | ONLCR;
    tcsetattr(slave, TCSANOW, &t);
    for (i = 0; i < 2; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            int fd = open(name, O_WRONLY | O_NOCTTY), k;

            memset(line, i == 0 ? 'a' : 'b', 7);
            line[7] = '\n';
            for (k = 0; k < 20000; k++) {
                if (write(fd, line, 8) != 8) {
                    _exit(1);
                }
            }
            _exit(0);
        }
    }
    close(slave);
    while ((n = (int)read(master, got, sizeof got)) > 0) {
        for (i = 0; i < n; i++) {
            if (got[i] == '\n') {
                bad += col != 7;
                col = 0;
            } else if (got[i] != '\r') {
                bad += col != 0 && got[i] != cur;
                cur = got[i];
                col++;
            }
        }
    }
    for (i = 0; i < 2; i++) {
        if (pids[i] < 0 || waitpid(pids[i], &status, 0) != pids[i] || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0) {
            return -1;
        }
    }
    return bad == 0 ? 0 : -1;
}

/* A slave written past what the master ring holds: a non-blocking write is
   cut and then EAGAIN, and a child's 64 KiB, written while the master waits
   before it reads, all reach the master; two children's lines reach it
   each whole. */
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
    usleep(100000);
    while (total < 16 * (int)sizeof buf && (n = read(master, buf, sizeof buf)) > 0) {
        total += n;
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0 || total != 16 * (int)sizeof buf) {
        return fail("a slave writer's 64 KiB did not all reach the master");
    }
    if (whole_writes(master, name) != 0) {
        return fail("two writers' lines to one slave were mixed");
    }
    close(master);
    return 0;
}

/* Whether a session leader takes a free slave it opens without O_NOCTTY. */
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
    close(master);
    return 0;
}

/* A child that has ended is a target until it is reaped. */
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
    if (waitpid(pid, NULL, 0) != pid || kill(pid, 0) != -1 || errno != ESRCH) {
        return fail("kill of a reaped child was not ESRCH");
    }
    return 0;
}

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
    errno = 0;
    if (sigtimedwait(&set, &info, &zero) != -1 || errno != EAGAIN) {
        return fail("sigtimedwait with nothing pending");
    }
    sigaddset(&set, SIGUSR2);
    if (sigprocmask(SIG_BLOCK, &set, NULL) != 0 || kill(getpid(), SIGUSR1) != 0 ||
        syscall(SYS_tkill, syscall(SYS_gettid), SIGUSR2) != 0) {
        return fail("sending SIGUSR1 to the process and SIGUSR2 to the thread");
    }
    if (sigtimedwait(&set, &info, &zero) != SIGUSR2 || info.si_code != SI_TKILL ||
        sigtimedwait(&set, &info, &zero) != SIGUSR1 || info.si_code != SI_USER) {
        return fail("sigtimedwait did not take the thread's signal before the process's");
    }
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    if (sigprocmask(SIG_BLOCK, &set, NULL) != 0) {
        return fail("blocking SIGALRM");
    }
    alarm(1);
    if (sigtimedwait(&set, &info, &far) != SIGALRM) {
        return fail("sigtimedwait under a timeout past 2^31 ticks did not wait for the alarm");
    }
    return zombies();
}

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
    if (mlockall(MCL_CURRENT) != 0 || munlockall() != 0) {
        return fail("mlockall");
    }
    munmap(p + page, page);
    errno = 0;
    if (mlock(p, 2 * page) != -1 || errno != ENOMEM) {
        return fail("mlock across a gap");
    }
    p[0] = 1;
    if (mincore(p, page, vec) != 0 || !(vec[0] & 1)) {
        return fail("mincore of a touched page");
    }
    munmap(p, page);
    return 0;
}

static int identity(void)
{
    unsigned cpu = 7, node = 7;
    long old;

    if (syscall(SYS_getcpu, &cpu, &node, NULL) != 0 || cpu != 0 || node != 0) {
        return fail("getcpu");
    }
    old = syscall(SYS_personality, 0xffffffffUL);
    if (old < 0 || syscall(SYS_personality, 0x0040000UL) != old ||
        syscall(SYS_personality, 0xffffffffUL) != 0x0040000L ||
        syscall(SYS_personality, (unsigned long)old) != 0x0040000L) {
        return fail("personality");
    }
    return 0;
}

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
    if (recvmmsg(sv[1], in, 2, 0, NULL) != 2 || in[0].msg_len != sizeof(a) ||
        in[1].msg_len != sizeof(b) || strcmp(ra, a) != 0 || strcmp(rb, b) != 0) {
        return fail("recvmmsg");
    }
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int clocks(void)
{
    int64_t t[2] = {-1, -1};

    if (syscall(403, CLOCK_MONOTONIC, t) != 0 || t[0] < 0 || t[1] < 0 || t[1] >= 1000000000) {
        return fail("clock_gettime64");
    }
    return 0;
}

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
    flags = fcntl(0, F_GETFD);
    if (flags < 0 || (flags & FD_CLOEXEC)) {
        return fail("close_range with CLOSE_RANGE_CLOEXEC marked a descriptor below the range");
    }
    errno = 0;
    if (syscall(436, fd[0], fd[0], 8) != -1 || errno != EINVAL) {
        return fail("close_range with an unknown flag");
    }
    errno = 0;
    if (syscall(436, fd[2], fd[0], 0) != -1 || errno != EINVAL) {
        return fail("close_range with first past last");
    }
    if (syscall(436, fd[0], ~0U, 0) != 0 || fcntl(fd[0], F_GETFD) != -1 ||
        fcntl(fd[2], F_GETFD) != -1 || fcntl(0, F_GETFD) < 0) {
        return fail("close_range to the end of the table");
    }
    return 0;
}

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
    if (syscall(SYS_tee, p[0], q[1], 16, 0) != -1 || errno != EINVAL) {
        return fail("tee was not EINVAL");
    }
    if (syscall(SYS_sendfile64, null, p[0], NULL, 16) != -1 || errno != EINVAL) {
        return fail("sendfile was not EINVAL");
    }
    if (syscall(SYS_splice, 999, NULL, null, NULL, 16, 0) != -1 || errno != EBADF) {
        return fail("splice from a closed descriptor was not EBADF");
    }
    close(null);
    close(p[0]);
    close(p[1]);
    close(q[0]);
    close(q[1]);
    return 0;
}

/* adjtimex reads the clock's state as Linux's starts, unsynchronized, and
   clock_adjtime adjusts CLOCK_REALTIME alone. */
static int adjusts(void)
{
    struct timex tx;

    memset(&tx, 0, sizeof tx);
    if (adjtimex(&tx) != TIME_ERROR || !(tx.status & STA_UNSYNC) || tx.tick != 10000 ||
        tx.tolerance != (500L << 16)) {
        return fail("adjtimex did not read an unsynchronized clock with its 10 ms tick");
    }
    memset(&tx, 0, sizeof tx);
    if (clock_adjtime(CLOCK_MONOTONIC, &tx) != -1 || errno != EOPNOTSUPP) {
        return fail("clock_adjtime of CLOCK_MONOTONIC was not EOPNOTSUPP");
    }
    return 0;
}

/* Extended attributes, which no file system here stores, and the advice
   calls, which change nothing but check their descriptor. */
static int advises(void)
{
    char v[16];
    int p[2];
    int fd = open("/bin/sh", O_RDONLY);

    if (fd < 0 || pipe(p) != 0) {
        return fail("/bin/sh and a pipe");
    }
    if (listxattr("/bin/sh", v, sizeof v) != 0 || flistxattr(fd, v, sizeof v) != 0) {
        return fail("listxattr of /bin/sh was not an empty list");
    }
    if (getxattr("/bin/sh", "user.x", v, sizeof v) != -1 || errno != EOPNOTSUPP) {
        return fail("getxattr of /bin/sh was not EOPNOTSUPP");
    }
    if (lsetxattr("/nonexistent", "user.x", "1", 1, 0) != -1 || errno != ENOENT) {
        return fail("lsetxattr of a missing file was not ENOENT");
    }
    if (readahead(fd, 0, 4096) != 0 || posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL) != 0) {
        return fail("readahead and posix_fadvise of /bin/sh did not return 0");
    }
    if (posix_fadvise(p[0], 0, 0, POSIX_FADV_NORMAL) != ESPIPE) {
        return fail("posix_fadvise of a pipe was not ESPIPE");
    }
    close(fd);
    close(p[0]);
    close(p[1]);
    return 0;
}

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
    if (posix_fallocate(ro, 0, 4096) != EBADF) {
        return fail("posix_fallocate of a file open for reading was not EBADF");
    }
    if (posix_fallocate(fd, 0, 0) != EINVAL) {
        return fail("posix_fallocate of no bytes was not EINVAL");
    }
    if (posix_fallocate(p[1], 0, 4096) != ESPIPE) {
        return fail("posix_fallocate of a pipe was not ESPIPE");
    }
    if (fallocate(ro, 0x100, 0, 4096) != -1 || errno != EOPNOTSUPP) {
        return fail("fallocate with a mode Linux does not take was not EOPNOTSUPP first");
    }
    close(p[1]);
    p[1] = open("/proc/self/oom_score_adj", O_WRONLY);
    if (p[1] < 0 || posix_fallocate(p[1], 0, 4096) != EOPNOTSUPP) {
        return fail("posix_fallocate of a /proc file open for writing was not EOPNOTSUPP");
    }
    unlink("/tmp/callsmusl.fallocate");
    close(fd);
    close(ro);
    close(p[0]);
    close(p[1]);
    return 0;
}

static volatile sig_atomic_t vtalrms, profs;

static void on_vtalrm(int signo)
{
    (void)signo;
    vtalrms++;
}

static void on_prof(int signo)
{
    (void)signo;
    profs++;
}

/* The timers of processor time, which gprof's sampling uses: each posts its
   signal three times while the process spins for at most three seconds of
   processor time, and a value of zero disarms it. */
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
    do {
        times(&t);
    } while ((vtalrms < 3 || profs < 3) && t.tms_utime + t.tms_stime - used < 300);
    setitimer(ITIMER_VIRTUAL, &off, NULL);
    setitimer(ITIMER_PROF, &off, NULL);
    signal(SIGVTALRM, SIG_DFL);
    signal(SIGPROF, SIG_DFL);
    if (vtalrms < 3 || profs < 3) {
        return fail("ITIMER_VIRTUAL and ITIMER_PROF did not each post three signals");
    }
    return 0;
}

int main(void)
{
    if (signals() || memory() || identity() || messages() || clocks() || ranges() || copies() ||
        reserves() || adjusts() || advises() || terminals() || peaks() || refusals() || setgids() ||
        privacies() || timers() || queues() || flows() || signalfds() || cputimers()) {
        return 1;
    }
    printf("CALLSMUSL OK\n");
    return 0;
}
