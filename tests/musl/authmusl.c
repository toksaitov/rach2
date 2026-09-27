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
 * sethostname(); dup3 onto an
 * explicit high fd with O_CLOEXEC dups and sets close-on-exec; an advisory
 * fcntl(F_SETLK) read lock on it is granted, and a write lock refused with
 * EBADF, since the file is open for reading alone, as in Linux; and the
 * credential walk a login server performs works through libc: the seteuid
 * excursion and return, the real drop (gid first, then uid), and no way back
 * up afterward.
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

int main(void)
{
    struct passwd *pw;
    struct spwd *sp;
    char *hash;
    struct rlimit rl;
    char host[64];
    int fd, nfd, fl;
    uid_t ru, eu, su;
    struct flock lk;

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

    /* Advisory locks on the dup'd fd, which is open for reading alone. */
    memset(&lk, 0, sizeof(lk));
    lk.l_type = F_RDLCK;
    lk.l_whence = SEEK_SET;
    if (fcntl(nfd, F_SETLK, &lk) != 0) {
        printf("AUTHMUSL FAIL (F_SETLK)\n");
        return 1;
    }
    lk.l_type = F_WRLCK;
    if (fcntl(nfd, F_SETLK, &lk) != -1 || errno != EBADF) {
        printf("AUTHMUSL FAIL (F_SETLK F_WRLCK on a descriptor for reading)\n");
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

    printf("AUTHMUSL OK\n");
    return 0;
}
