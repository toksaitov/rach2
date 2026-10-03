/**
 * Exercises the permission and identity syscalls through the ported musl.
 *
 * These are the calls a dropbear pty session and host-key write make, driven
 * by a real musl binary so the exact syscall numbers musl emits, i386's or
 * x86-64's, are pinned. The expected contracts:
 *
 * * `fsync(fd)` on a real file returns 0 (KRFFS is RAM, so durability is a
 *   no-op); `fsync` of a bad descriptor is -1 with EBADF.
 * * `link(a, b)` is -1 with EPERM: KRFFS has no hard links, so dropbear falls
 *   back to a direct host-key write.
 * * `chmod(path, mode)` updates a KRFFS file's permission bits and `stat`
 *   reflects them; `chmod` of a missing path is -1 with ENOENT.
 * * `chown(path, uid, gid)` as root stores the owner and `stat` reflects it
 *   (the deeper rules are ownprobe's to pin).
 * * `seteuid(0)`, `setegid(0)`, and `setresuid(0,0,0)` succeed, and a root
 *   caller's effective-id excursion (`seteuid(1)` out, `seteuid(0)` back, the
 *   idiom around reading authorized_keys) works both ways; the identity rules
 *   themselves are authmusl's to pin.
 * * After a real drop to uid 1000, the nine-bit check answers through libc: a
 *   0644 root file opens for read but not write, and creating in the root
 *   directory is EACCES (the full deny matrix is permprobe's to pin).
 * * A last "." is looked up in the directory before it, which takes search
 *   permission there, as Linux's walk does, and a trailing slash alone takes
 *   none: as uid 1000, with a directory of its own in /dev/shm set to 0600,
 *   stat, access and open of "dir/." are EACCES, and stat of "dir/" reports
 *   the directory.
 * * A process left in a directory below one it may not search, as `su` from
 *   root's home leaves one, still names files there, as Linux's walk from the
 *   working directory lets it: as uid 1000 in "w/in", with "w" root's 0700,
 *   stat of ".", an open of a file in it, getcwd and stat of the file by its
 *   absolute path succeed, and stat of "w/other" is EACCES.
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
#include <sys/wait.h>

/* The directory the dot rows make, in /dev/shm, which anyone may write. */
#define DOT_DIR "/dev/shm/permmusl.d"

/**
 * The rows of a last "." as uid 1000: the directory `DOT_DIR`, the caller's
 * own, set to 0600, so its owner may not search it. 0, or 1 once the failure
 * is reported.
 */
static int dot_search_rows(void)
{
    struct stat st;
    int fd;
    int ok;

    if (mkdir(DOT_DIR, 0700) != 0 || chmod(DOT_DIR, 0600) != 0) {
        printf("PERMMUSL FAIL (make a directory in /dev/shm: %s)\n", strerror(errno));
        return 1;
    }
    ok = stat(DOT_DIR "/.", &st) == -1 && errno == EACCES;
    ok = ok && access(DOT_DIR "/.", F_OK) == -1 && errno == EACCES;
    fd = open(DOT_DIR "/.", O_RDONLY);
    ok = ok && fd == -1 && errno == EACCES;
    if (fd >= 0) {
        close(fd);
    }
    ok = ok && stat(DOT_DIR "/", &st) == 0 && S_ISDIR(st.st_mode);
    chmod(DOT_DIR, 0700);
    rmdir(DOT_DIR);
    if (!ok) {
        printf("PERMMUSL FAIL (a last . looked up without search permission on its directory)\n");
        return 1;
    }
    return 0;
}

/* The directories the working-directory rows make. */
#define CWD_DIR "/dev/shm/permmusl.w"

/**
 * The rows of a working directory below one the caller may not search, as
 * the header says, in a child that drops to uid 1000 in `CWD_DIR/in`; the
 * parent, root, cleans up. 0, or 1 once the failure is reported.
 */
static int cwd_rows(void)
{
    struct stat st;
    char cwd[64];
    pid_t pid;
    int status = 1, fd;

    if (mkdir(CWD_DIR, 0700) != 0 || mkdir(CWD_DIR "/in", 0755) != 0 ||
        mkdir(CWD_DIR "/other", 0755) != 0 ||
        (fd = open(CWD_DIR "/in/f", O_CREAT | O_WRONLY, 0644)) < 0) {
        printf("PERMMUSL FAIL (make the working-directory rows' tree: %s)\n", strerror(errno));
        return 1;
    }
    close(fd);
    pid = fork();
    if (pid == 0) {
        int ok = chdir(CWD_DIR "/in") == 0 && setgid(1000) == 0 && setuid(1000) == 0;

        ok = ok && stat(".", &st) == 0 && (fd = open("f", O_RDONLY)) >= 0;
        ok = ok && getcwd(cwd, sizeof(cwd)) && strcmp(cwd, CWD_DIR "/in") == 0;
        ok = ok && stat(CWD_DIR "/in/f", &st) == 0;
        ok = ok && stat(CWD_DIR "/other", &st) == -1 && errno == EACCES;
        _exit(ok ? 0 : 1);
    }
    if (pid > 0) {
        waitpid(pid, &status, 0);
    }
    unlink(CWD_DIR "/in/f");
    rmdir(CWD_DIR "/in");
    rmdir(CWD_DIR "/other");
    rmdir(CWD_DIR);
    if (pid < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("PERMMUSL FAIL (a file of the working directory below an unsearchable one)\n");
        return 1;
    }
    return 0;
}

/**
 * The deny paths through libc, after the drop to uid 1000, which is for
 * good: a 0644 root file reads but does not write, the root directory
 * refuses a creation, and a last "." takes search permission
 * (`dot_search_rows`). 0, or 1 once the failure is reported.
 */
static int deny_rows(void)
{
    int fd;

    if (setgid(1000) != 0 || setuid(1000) != 0) {
        printf("PERMMUSL FAIL (drop to 1000)\n");
        return 1;
    }
    fd = open("/hello.txt", O_RDONLY);
    if (fd < 0) {
        printf("PERMMUSL FAIL (read 0644 as other)\n");
        return 1;
    }
    close(fd);
    if (open("/hello.txt", O_WRONLY) != -1 || errno != EACCES) {
        printf("PERMMUSL FAIL (write 0644 as other not EACCES)\n");
        return 1;
    }
    if (open("/denied.musl", O_CREAT | O_WRONLY, 0644) != -1 || errno != EACCES) {
        printf("PERMMUSL FAIL (create in / not EACCES)\n");
        return 1;
    }
    return dot_search_rows();
}

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
        printf("PERMMUSL FAIL (chmod not reflected: 0%o)\n", (unsigned)(st.st_mode & 0777));
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
    if (chown("/hello.txt", 1000, 1000) != 0 || stat("/hello.txt", &st) != 0 || st.st_uid != 1000 ||
        st.st_gid != 1000) {
        printf("PERMMUSL FAIL (chown 1000:1000 round trip)\n");
        return 1;
    }
    if (chown("/hello.txt", 0, 0) != 0) {
        printf("PERMMUSL FAIL (chown back to root)\n");
        return 1;
    }
    /* Restore the staged 0644 before the drop below counts on it. */
    if (chmod("/hello.txt", 0644) != 0) {
        printf("PERMMUSL FAIL (restore 0644)\n");
        return 1;
    }

    /* seteuid/setegid/setresuid: the identity rules themselves are authmusl's
     * to pin; here just prove the excursion a file server runs under works
     * both ways for a root caller. */
    if (seteuid(0) != 0 || setegid(0) != 0 || setresuid(0, 0, 0) != 0) {
        printf("PERMMUSL FAIL (seteuid/setegid/setresuid 0: %s)\n", strerror(errno));
        return 1;
    }
    if (seteuid(1) != 0 || geteuid() != 1 || seteuid(0) != 0) {
        printf("PERMMUSL FAIL (seteuid excursion)\n");
        return 1;
    }

    if (cwd_rows() != 0) {
        return 1;
    }

    /* The deny paths through libc, last: the drop is for good. */
    if (deny_rows() != 0) {
        return 1;
    }

    printf("PERMMUSL OK\n");
    return 0;
}
