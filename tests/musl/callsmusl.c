/**
 * The calls GNU libc programs make that the process work of milestone 54
 * added.
 *
 * sigtimedwait takes a blocked signal the process sent itself and returns
 * its number and sender, takes a signal sent to the thread before a lower
 * one sent to the process, and with no signal pending its timeout ends it
 * with EAGAIN. mlock, munlock, mlockall and munlockall accept a mapped range
 * and refuse a gap with ENOMEM; mincore reports a touched page resident.
 * getcpu reports CPU 0, and personality keeps and returns its value.
 * sendmmsg sends two datagrams in one call over an AF_UNIX pair and
 * recvmmsg receives both, each with its count. clock_gettime64 reads the
 * monotonic clock with 64-bit fields. close_range marks a range of
 * descriptors close-on-exec and closes it, reaches past the table's end,
 * and refuses a flag it does not take and a backward range.
 *
 * Prints "CALLSMUSL OK" on success and "CALLSMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int fail(const char *what)
{
    printf("CALLSMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

static int signals(void)
{
    sigset_t set;
    siginfo_t info;
    struct timespec zero = {0, 0};

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
    return 0;
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

int main(void)
{
    if (signals() || memory() || identity() || messages() || clocks() || ranges()) {
        return 1;
    }
    printf("CALLSMUSL OK\n");
    return 0;
}
