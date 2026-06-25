/**
 * Exercises, through the ported musl, the identity, resource, and tail
 * syscalls a login server leans on.
 *
 * Each check probes one contract: getpwnam("root") parses /etc/passwd to uid 0
 * and gid 0; getrlimit(RLIMIT_NOFILE) reports the honest cap (MAX_FDS == 16);
 * gethostname() round-trips with sethostname(); dup3 onto an explicit high fd
 * with O_CLOEXEC dups and sets close-on-exec; an advisory fcntl(F_SETLK) is
 * granted with no conflict (single-user locks); setuid(0)/setgid(0) succeed as
 * a no-op privilege drop, while a drop to a nonexistent user must fail rather
 * than silently leave us root.
 *
 * Prints "AUTHMUSL OK" on success, "AUTHMUSL FAIL" otherwise. Linked against
 * the ported musl.
 */

#define _GNU_SOURCE /* dup3, sethostname */
#include <pwd.h>
#include <sys/resource.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>

int main(void)
{
    struct passwd *pw;
    struct rlimit rl;
    char host[64];
    int fd, nfd, fl;
    struct flock lk;

    pw = getpwnam("root");
    if (!pw || pw->pw_uid != 0 || pw->pw_gid != 0) {
        printf("AUTHMUSL FAIL (getpwnam root)\n");
        return 1;
    }

    if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur != 16) {
        printf("AUTHMUSL FAIL (RLIMIT_NOFILE=%ld)\n", (long)rl.rlim_cur);
        return 1;
    }

    if (sethostname("rachtest", 8) != 0 ||
        gethostname(host, sizeof(host)) != 0 || strcmp(host, "rachtest") != 0) {
        printf("AUTHMUSL FAIL (hostname round-trip: %s)\n", host);
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

    /* Advisory lock on the dup'd fd. */
    memset(&lk, 0, sizeof(lk));
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;
    if (fcntl(nfd, F_SETLK, &lk) != 0) {
        printf("AUTHMUSL FAIL (F_SETLK)\n");
        return 1;
    }
    close(fd);
    close(nfd);

    if (setuid(0) != 0 || setgid(0) != 0) {
        printf("AUTHMUSL FAIL (setuid/setgid 0)\n");
        return 1;
    }
    /* Drop to a nonexistent user must fail. */
    if (setuid(1000) == 0 || setgid(1000) == 0) {
        printf("AUTHMUSL FAIL (setuid 1000 wrongly succeeded)\n");
        return 1;
    }

    printf("AUTHMUSL OK\n");
    return 0;
}
