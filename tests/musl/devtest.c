/*
 * devtest.c - Phase 15 integration: the synthetic /dev and /proc through musl.
 *
 * Drives the real libc wrappers (not raw int 0x80): open/read/write on the char
 * devices, and ttyname(0) -- which exercises isatty (TIOCGWINSZ), readlink of
 * /proc/self/fd/0, and the stat/fstat identity match. Installed as "mdevtest".
 * Prints "DEVTEST OK".
 */
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

static int fails;

static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        printf("  FAIL: %s\n", msg);
    }
}

int main(void)
{
    int fd, i, allzero;
    char buf[64];
    ssize_t n;
    const char *tn;

    /* /dev/zero yields zeros. */
    fd = open("/dev/zero", O_RDONLY);
    check(fd >= 0, "open /dev/zero");
    memset(buf, 0xAB, sizeof(buf));
    n = read(fd, buf, sizeof(buf));
    check(n == 64, "/dev/zero read 64");
    allzero = 1;
    for (i = 0; i < 64; i++) {
        if (buf[i]) {
            allzero = 0;
        }
    }
    check(allzero, "/dev/zero is all zeros");
    close(fd);

    /* /dev/null reads EOF. */
    fd = open("/dev/null", O_RDONLY);
    check(fd >= 0 && read(fd, buf, sizeof(buf)) == 0, "/dev/null read is EOF");
    close(fd);

    /* /dev/full fails writes with ENOSPC. */
    fd = open("/dev/full", O_WRONLY);
    check(fd >= 0, "open /dev/full");
    errno = 0;
    n = write(fd, "x", 1);
    check(n < 0 && errno == ENOSPC, "/dev/full write -> ENOSPC");
    close(fd);

    /* ttyname(0): exercises isatty + /proc/self/fd readlink + stat/fstat match. */
    tn = ttyname(0);
    check(tn != NULL && strncmp(tn, "/dev/", 5) == 0, "ttyname(0) is a /dev name");
    if (tn) {
        printf("  ttyname(0) = %s\n", tn);
    }

    /* opendir("/dev") lists the devices (synthetic directory getdents). */
    {
        DIR *d = opendir("/dev");
        struct dirent *de;
        int found_null = 0;
        check(d != NULL, "opendir /dev");
        if (d) {
            while ((de = readdir(d)) != NULL) {
                if (strcmp(de->d_name, "null") == 0) {
                    found_null = 1;
                }
            }
            closedir(d);
        }
        check(found_null, "/dev lists null");
    }

    /* opendir("/proc") lists "self" and our pid (what ps enumerates). */
    {
        DIR *d = opendir("/proc");
        struct dirent *de;
        int found_self = 0, found_pid = 0;
        char pidstr[16];
        snprintf(pidstr, sizeof(pidstr), "%d", (int)getpid());
        check(d != NULL, "opendir /proc");
        if (d) {
            while ((de = readdir(d)) != NULL) {
                if (strcmp(de->d_name, "self") == 0) {
                    found_self = 1;
                }
                if (strcmp(de->d_name, pidstr) == 0) {
                    found_pid = 1;
                }
            }
            closedir(d);
        }
        check(found_self, "/proc lists self");
        check(found_pid, "/proc lists our pid");
    }

    /* openat relative to a synthetic directory fd (the dir-fd fix). */
    {
        int dfd = open("/dev", O_RDONLY | O_DIRECTORY);
        check(dfd >= 0, "open /dev O_DIRECTORY");
        if (dfd >= 0) {
            int nfd = openat(dfd, "null", O_RDONLY);
            check(nfd >= 0, "openat(devfd, \"null\") resolves");
            if (nfd >= 0) {
                close(nfd);
            }
            close(dfd);
        }
    }

    /* A read-only device rejects writes (access mode is enforced before dispatch). */
    {
        int z = open("/dev/zero", O_RDONLY);
        check(z >= 0, "open /dev/zero O_RDONLY");
        errno = 0;
        check(z >= 0 && write(z, "x", 1) < 0 && errno == EBADF,
              "write to O_RDONLY /dev/zero -> EBADF");
        if (z >= 0) {
            close(z);
        }
    }

    /* O_DIRECTORY on a character device is ENOTDIR. */
    {
        errno = 0;
        check(open("/dev/null", O_RDONLY | O_DIRECTORY) < 0 && errno == ENOTDIR,
              "open /dev/null O_DIRECTORY -> ENOTDIR");
    }

    /* access(2) on synthetic paths (the new syscall the shell's `test` uses). */
    check(access("/dev/null", F_OK) == 0, "access(/dev/null, F_OK)");
    check(access("/proc/self/stat", R_OK) == 0, "access(/proc/self/stat, R_OK)");
    errno = 0;
    check(access("/proc/self/stat", W_OK) < 0 && errno == EACCES,
          "access(/proc/self/stat, W_OK) -> EACCES");
    errno = 0;
    check(access("/dev/nope", F_OK) < 0 && errno == ENOENT,
          "access(/dev/nope) -> ENOENT");
    errno = 0;
    check(access("/proc", 8) < 0 && errno == EINVAL,
          "access(/proc, bad mode) -> EINVAL");

    /* /proc/self is a symlink to our numeric pid. */
    {
        char lbuf[16];
        char pidstr[16];
        ssize_t ln = readlink("/proc/self", lbuf, sizeof(lbuf) - 1);
        snprintf(pidstr, sizeof(pidstr), "%d", (int)getpid());
        if (ln > 0) {
            lbuf[ln] = '\0';
        }
        check(ln > 0 && strcmp(lbuf, pidstr) == 0, "readlink /proc/self -> our pid");
    }

    /* Symlink follow semantics: lstat sees the magic link, stat follows it. */
    {
        struct stat ls, fs;
        check(lstat("/proc/self", &ls) == 0 && S_ISLNK(ls.st_mode),
              "lstat /proc/self -> symlink");
        check(stat("/proc/self", &fs) == 0 && S_ISDIR(fs.st_mode),
              "stat /proc/self -> directory");
        check(stat("/proc/self/fd/0", &fs) == 0 && S_ISCHR(fs.st_mode),
              "stat /proc/self/fd/0 -> char device (followed to the console)");
    }

    if (fails == 0) {
        printf("DEVTEST OK\n");
    } else {
        printf("DEVTEST FAIL: %d check(s)\n", fails);
    }
    return fails ? 1 : 0;
}
