/**
 * Exercises the permission and identity syscalls through the ported musl.
 *
 * These are the calls a dropbear pty session and host-key write make, driven
 * by a real musl binary so the exact i386 syscall numbers musl emits are
 * pinned. The expected contracts:
 *
 * * `fsync(fd)` on a real file returns 0 (KRFFS is RAM, so durability is a
 *   no-op); `fsync` of a bad descriptor is -1 with EBADF.
 * * `link(a, b)` is -1 with EPERM: KRFFS has no hard links, so dropbear falls
 *   back to a direct host-key write.
 * * `chmod(path, mode)` updates a KRFFS file's permission bits and `stat`
 *   reflects them; `chmod` of a missing path is -1 with ENOENT.
 * * `chown(path, 0, 0)` succeeds because only root exists; a nonzero owner is
 *   -1 with EPERM.
 * * `seteuid(0)`, `setegid(0)`, and `setresuid(0,0,0)` succeed, and a root
 *   caller's effective-id excursion (`seteuid(1)` out, `seteuid(0)` back, the
 *   idiom around reading authorized_keys) works both ways; the identity rules
 *   themselves are authmusl's to pin.
 *
 * Prints "PERMMUSL OK" or "PERMMUSL FAIL ...". Linked against the ported musl.
 */

#define _GNU_SOURCE /* setresuid */
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

int main(void)
{
    int fd;
    struct stat st;

    /* fsync */
    fd = open("/etc/motd", O_RDONLY);
    if (fd < 0) {
        printf("PERMMUSL FAIL (open motd: %s)\n", strerror(errno));
        return 1;
    }
    if (fsync(fd) != 0) {
        printf("PERMMUSL FAIL (fsync valid: %s)\n", strerror(errno));
        return 1;
    }
    close(fd);
    if (fsync(4096) != -1 || errno != EBADF) {
        printf("PERMMUSL FAIL (fsync bad fd not EBADF)\n");
        return 1;
    }

    /* link */
    if (link("/etc/motd", "/etc/motd.link") != -1 || errno != EPERM) {
        printf("PERMMUSL FAIL (link not EPERM: %s)\n", strerror(errno));
        return 1;
    }

    /* chmod */
    if (chmod("/hello.txt", 0600) != 0) {
        printf("PERMMUSL FAIL (chmod: %s)\n", strerror(errno));
        return 1;
    }
    if (stat("/hello.txt", &st) != 0 || (st.st_mode & 0777) != 0600) {
        printf("PERMMUSL FAIL (chmod not reflected: 0%o)\n",
               (unsigned)(st.st_mode & 0777));
        return 1;
    }
    if (chmod("/no_such_file", 0644) != -1 || errno != ENOENT) {
        printf("PERMMUSL FAIL (chmod missing not ENOENT)\n");
        return 1;
    }

    /* chown */
    if (chown("/hello.txt", 0, 0) != 0) {
        printf("PERMMUSL FAIL (chown 0:0: %s)\n", strerror(errno));
        return 1;
    }
    if (chown("/hello.txt", 1, 0) != -1 || errno != EPERM) {
        printf("PERMMUSL FAIL (chown nonzero not EPERM)\n");
        return 1;
    }

    /* seteuid/setegid/setresuid: the identity rules themselves are authmusl's
     * to pin; here just prove the excursion a file server runs under works
     * both ways for a root caller. */
    if (seteuid(0) != 0 || setegid(0) != 0 || setresuid(0, 0, 0) != 0) {
        printf("PERMMUSL FAIL (seteuid/setegid/setresuid 0: %s)\n",
               strerror(errno));
        return 1;
    }
    if (seteuid(1) != 0 || geteuid() != 1 || seteuid(0) != 0) {
        printf("PERMMUSL FAIL (seteuid excursion)\n");
        return 1;
    }

    printf("PERMMUSL OK\n");
    return 0;
}
