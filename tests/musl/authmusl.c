/**
 * Exercises, through the ported musl, the identity, resource, and tail
 * syscalls a login server leans on.
 *
 * Each check probes one contract: getpwnam("root") parses /etc/passwd to uid 0
 * and gid 0; getspnam("root") parses the staged /etc/shadow and crypt() with
 * the stored setting reproduces the stored hash (the image commits the fixed
 * test credential "rach2", so this proves the guest verifies exactly what the
 * host's mkhash mints); getrlimit(RLIMIT_NOFILE) reports the real limit,
 * a thousand and twenty-four descriptors; gethostname() round-trips with
 * sethostname(); root sets the real time ahead and back again through
 * clock_settime, settimeofday and stime (which x86-64 does not have), and reads
 * back each time it set; dup3 onto an explicit high fd with O_CLOEXEC dups and
 * sets close-on-exec; an advisory fcntl(F_SETLK) read lock on it is granted,
 * and a write lock refused with EBADF, since the file is open for reading
 * alone, as in Linux; and the credential walk a login server performs works
 * through libc: the seteuid excursion and return, the real drop (gid first,
 * then uid), and no way back up afterward, nor to the host name or the clock.
 *
 * Prints "AUTHMUSL OK" on success, "AUTHMUSL FAIL" otherwise. Linked against
 * the ported musl.
 */

#define _GNU_SOURCE /* dup3, sethostname */
#include <errno.h>
#include <pwd.h>
#include <shadow.h>
#include <crypt.h>
#include <sys/resource.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

/*
 * The calls with times of a long's width, which musl names for time64
 * programs by other names: i386's stime and settimeofday, whose times are 32
 * bits wide, and x86-64's settimeofday, whose are 64. x86-64 has no stime.
 */
#if defined __x86_64__
#define NR_SETTIMEOFDAY 164
typedef long tv_word_t;
#else
#define NR_STIME        25
#define NR_SETTIMEOFDAY 79
typedef int32_t tv_word_t;
#endif

/** Whether the real time reads `sec` seconds, give or take two. */
static int reads_near(long long sec)
{
    struct timespec ts;

    return clock_gettime(CLOCK_REALTIME, &ts) == 0 && ts.tv_sec >= sec && ts.tv_sec <= sec + 2;
}

/**
 * Set the real time `delta` seconds ahead with clock_settime, back with the
 * kernel's settimeofday, whose timeval has fields of a long's width, and to the
 * second it reads with stime where there is one: 0 when each reads back the
 * time it set, and -1 with the errno of the first call refused, or EIO for a
 * time that did not take.
 */
static int clock_step(long delta)
{
    struct timespec now, ahead;
    struct {
        tv_word_t sec;
        tv_word_t usec;
    } tv;
#if defined NR_STIME
    int32_t sec;
#endif

    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return -1;
    }
    ahead.tv_sec = now.tv_sec + delta;
    ahead.tv_nsec = now.tv_nsec;
    if (clock_settime(CLOCK_REALTIME, &ahead) != 0) {
        return -1;
    }
    if (!reads_near(ahead.tv_sec) || clock_gettime(CLOCK_REALTIME, &now) != 0) {
        errno = EIO;
        return -1;
    }
    tv.sec = (tv_word_t)(now.tv_sec - delta);
    tv.usec = (tv_word_t)(now.tv_nsec / 1000);
    if (syscall(NR_SETTIMEOFDAY, &tv, NULL) != 0) {
        return -1;
    }
    if (!reads_near(tv.sec)) {
        errno = EIO;
        return -1;
    }
#if defined NR_STIME
    sec = tv.sec;
    if (syscall(NR_STIME, &sec) != 0) {
        return -1;
    }
    if (!reads_near(sec)) {
        errno = EIO;
        return -1;
    }
#endif
    return 0;
}

/** Advisory locks on `fd`, which is open for reading alone: a read lock is
 * granted, and a write lock refused with EBADF. 0, or 1 once the failure is
 * reported. */
static int lock_rows(int fd)
{
    struct flock lk;

    memset(&lk, 0, sizeof(lk));
    lk.l_type = F_RDLCK;
    lk.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &lk) != 0) {
        printf("AUTHMUSL FAIL (F_SETLK)\n");
        return 1;
    }
    lk.l_type = F_WRLCK;
    if (fcntl(fd, F_SETLK, &lk) != -1 || errno != EBADF) {
        printf("AUTHMUSL FAIL (F_SETLK F_WRLCK on a descriptor for reading)\n");
        return 1;
    }
    return 0;
}

int main(void)
{
    struct passwd *pw;
    struct spwd *sp;
    char *hash;
    struct rlimit rl;
    char host[64];
    int fd, nfd, fl;
    uid_t ru, eu, su;

    pw = getpwnam("root");
    if (!pw || pw->pw_uid != 0 || pw->pw_gid != 0) {
        printf("AUTHMUSL FAIL (getpwnam root)\n");
        return 1;
    }

    /* The password walk a login server performs, while still root (the
     * staged /etc/shadow is 0600): the shadow entry resolves, and crypt with
     * the stored hash as its own setting reproduces it for the committed
     * test password. */
    sp = getspnam("root");
    if (!sp || !sp->sp_pwdp || strncmp(sp->sp_pwdp, "$6$", 3) != 0) {
        printf("AUTHMUSL FAIL (getspnam root)\n");
        return 1;
    }
    hash = crypt("rach2", sp->sp_pwdp);
    if (!hash || strcmp(hash, sp->sp_pwdp) != 0) {
        printf("AUTHMUSL FAIL (crypt against shadow)\n");
        return 1;
    }
    if (strcmp(crypt("wrong", sp->sp_pwdp), sp->sp_pwdp) == 0) {
        printf("AUTHMUSL FAIL (crypt accepts a wrong password)\n");
        return 1;
    }

    if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur != 1024) {
        printf("AUTHMUSL FAIL (RLIMIT_NOFILE=%ld)\n", (long)rl.rlim_cur);
        return 1;
    }

    if (sethostname("rachtest", 8) != 0 || gethostname(host, sizeof(host)) != 0 ||
        strcmp(host, "rachtest") != 0) {
        printf("AUTHMUSL FAIL (hostname round-trip: %s)\n", host);
        return 1;
    }
    if (clock_step(100000) != 0) {
        printf("AUTHMUSL FAIL (root sets the real time forward and back)\n");
        return 1;
    }

    /* dup3 onto a high fd: the new fd must be CLOEXEC. */
    fd = open("/etc/passwd", O_RDONLY);
    if (fd < 0) {
        printf("AUTHMUSL FAIL (open passwd)\n");
        return 1;
    }
    nfd = fd + 3;
    if (dup3(fd, nfd, O_CLOEXEC) != nfd) {
        printf("AUTHMUSL FAIL (dup3)\n");
        return 1;
    }
    fl = fcntl(nfd, F_GETFD);
    if (fl < 0 || !(fl & FD_CLOEXEC)) {
        printf("AUTHMUSL FAIL (dup3 cloexec)\n");
        return 1;
    }

    if (lock_rows(nfd) != 0) {
        return 1;
    }
    close(fd);
    close(nfd);

    /* The credential walk, last: it sheds root for good. The excursion out
     * and back is legal while the real and saved uids stay 0; the drop goes
     * gid first (setgid needs privilege, which setuid removes); and after the
     * drop every way back up must fail. */
    if (seteuid(1000) != 0 || geteuid() != 1000 || getuid() != 0) {
        printf("AUTHMUSL FAIL (seteuid excursion out)\n");
        return 1;
    }
    /* musl's seteuid is setresuid(-1, e, -1), so the saved uid must ride
     * through the excursion untouched: that is what makes the return legal. */
    if (getresuid(&ru, &eu, &su) != 0 || ru != 0 || eu != 1000 || su != 0) {
        printf("AUTHMUSL FAIL (saved uid during excursion)\n");
        return 1;
    }
    if (seteuid(0) != 0 || geteuid() != 0) {
        printf("AUTHMUSL FAIL (seteuid excursion back)\n");
        return 1;
    }
    if (setgid(1000) != 0 || setuid(1000) != 0) {
        printf("AUTHMUSL FAIL (drop to 1000)\n");
        return 1;
    }
    if (getuid() != 1000 || geteuid() != 1000 || getgid() != 1000) {
        printf("AUTHMUSL FAIL (identity after drop)\n");
        return 1;
    }
    if (setuid(0) == 0 || seteuid(0) == 0 || setgid(0) == 0) {
        printf("AUTHMUSL FAIL (regained root after drop)\n");
        return 1;
    }
    if (sethostname("other", 5) == 0 || errno != EPERM) {
        printf("AUTHMUSL FAIL (an unprivileged caller set the host name)\n");
        return 1;
    }
    if (clock_step(1000) == 0 || errno != EPERM) {
        printf("AUTHMUSL FAIL (an unprivileged caller set the time)\n");
        return 1;
    }

    printf("AUTHMUSL OK\n");
    return 0;
}
