/*
 * permmusl.c - Phase 23 integration: the permission/identity syscalls dropbear's
 * pty session and host-key write exercise, driven through the ported musl so the
 * exact i386 syscall numbers musl emits are pinned by a real binary.
 *
 *   - fsync(fd) on a real file returns 0 (KRFFS is RAM; durability is a no-op);
 *     fsync(bad) is -1/EBADF;
 *   - link(a, b) is -1/EPERM (KRFFS has no hard links, so dropbear falls back to
 *     a direct host-key write);
 *   - chmod(path, mode) updates a KRFFS file's permission bits (stat reflects it);
 *     chmod(missing) is -1/ENOENT;
 *   - chown(path, 0, 0) succeeds (only root exists); chown(path, 1, 0) is -1/EPERM;
 *   - seteuid(0)/setegid(0)/setresuid(0,0,0) succeed (the login-server privilege
 *     drop+restore around reading authorized_keys); seteuid(1) is -1/EPERM.
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

    /* fsync: a valid descriptor flushes to 0; a bad one is EBADF. */
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

    /* link: unsupported on KRFFS -> EPERM (the fallback dropbear depends on). */
    if (link("/etc/motd", "/etc/motd.link") != -1 || errno != EPERM) {
        printf("PERMMUSL FAIL (link not EPERM: %s)\n", strerror(errno));
        return 1;
    }

    /* chmod: changes a KRFFS file's permission bits; stat must reflect them. */
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

    /* chown: only root (0) exists; 0:0 is a no-op success, nonzero is EPERM. */
    if (chown("/hello.txt", 0, 0) != 0) {
        printf("PERMMUSL FAIL (chown 0:0: %s)\n", strerror(errno));
        return 1;
    }
    if (chown("/hello.txt", 1, 0) != -1 || errno != EPERM) {
        printf("PERMMUSL FAIL (chown nonzero not EPERM)\n");
        return 1;
    }

    /* seteuid/setegid/setresuid: the privilege drop+restore dropbear performs
     * around reading authorized_keys: root->root succeeds, ->nonzero is EPERM. */
    if (seteuid(0) != 0 || setegid(0) != 0 || setresuid(0, 0, 0) != 0) {
        printf("PERMMUSL FAIL (seteuid/setegid/setresuid 0: %s)\n",
               strerror(errno));
        return 1;
    }
    if (seteuid(1) != -1 || errno != EPERM) {
        printf("PERMMUSL FAIL (seteuid nonzero not EPERM)\n");
        return 1;
    }

    printf("PERMMUSL OK\n");
    return 0;
}
