/**
 * unlinkdirprobe: removing a directory's files while listing it, as `rm -rf`
 * does.
 *
 * `rm -rf` opens a directory, reads a batch of names, unlinks them, and reads
 * again from where it left off. A directory cursor that counted children
 * instead of naming a position that survives deletion would renumber the
 * survivors at every unlink, and the next read would skip one entry for each
 * file already removed; the skipped files would stay, and `rmdir` would fail
 * with "Directory not empty".
 *
 * The probe makes a directory of more entries than one small read holds,
 * removes them as `rm` does, and requires every name to have been listed and
 * the directory to be removable afterwards. Each record's `d_off` increases
 * strictly, and its inode, length and type are checked. Two descriptors on
 * one directory each have a cursor of their own, and each reports the end,
 * the first a second time as well.
 *
 * Prints "UNLINKDIRPROBE OK" and exits 0 on success; a failure prints
 * "UNLINKDIRPROBE FAIL: " with the reason and exits 1. Freestanding; direct
 * system calls (probe.h).
 */

#include "probe.h"

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;

#define STDOUT_FILENO 1

#define O_RDONLY    0
#define O_DIRECTORY 0200000

#define DT_REG 8
#define DT_DIR 4

/* Enough files that one small read cannot hold them all, so the listing
 * takes several calls and the cursor has to survive the unlinks between
 * them. */
#define FILE_COUNT 40

/* Small, so that the names take several reads: a cursor that loses its place
 * is not seen when every name fits in one. */
#define DIRENT_BUF 256

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
};

/** write(2). */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** exit(2); does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** open(2) with no mode. */
static int sys_open(const char *path, int flags)
{
    return syscall2(SYS_OPEN, (long)path, (long)flags);
}

/** close(2). */
static int sys_close(int fd)
{
    return syscall1(SYS_CLOSE, (long)fd);
}

/** creat(2). */
static int sys_creat(const char *path, int mode)
{
    return syscall2(SYS_CREAT, (long)path, (long)mode);
}

/** unlink(2). */
static int sys_unlink(const char *path)
{
    return syscall1(SYS_UNLINK, (long)path);
}

/** mkdir(2). */
static int sys_mkdir(const char *path, int mode)
{
    return syscall2(SYS_MKDIR, (long)path, (long)mode);
}

/** rmdir(2). */
static int sys_rmdir(const char *path)
{
    return syscall1(SYS_RMDIR, (long)path);
}

/** getdents64(2). */
static int sys_getdents64(int fd, void *dirp, unsigned int count)
{
    return syscall3(SYS_GETDENTS64, (long)fd, (long)dirp, (long)count);
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/** Write the string `s` and a newline to standard output. */
static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

/** Print "UNLINKDIRPROBE FAIL: `why`" and exit 1. */
static void fail(const char *why)
{
    sys_write(STDOUT_FILENO, "UNLINKDIRPROBE FAIL: ", 21);
    puts(why);
    sys_exit(1);
}

/** Build "/unlinkdir/fNN" (or "/unlinkdir" itself when `n` is negative). */
static void path_for(char *out, int n)
{
    const char *base = "/unlinkdir";
    int i = 0;

    while (base[i]) {
        out[i] = base[i];
        i++;
    }
    if (n < 0) {
        out[i] = '\0';
        return;
    }
    out[i++] = '/';
    out[i++] = 'f';
    out[i++] = (char)('0' + (n / 10));
    out[i++] = (char)('0' + (n % 10));
    out[i] = '\0';
}

/** The probe's entry: the listing with unlinks, the two descriptors, and
 * the rmdir at the end. */
PROBE_START void _start(void)
{
    char path[32];
    char dir[32];
    char buf[DIRENT_BUF];
    int i, fd, rounds;
    int removed = 0;
    int64_t last_off = 0;

    path_for(dir, -1);

    /* Start from nothing, in case an earlier run left the directory behind. */
    for (i = 0; i < FILE_COUNT; i++) {
        path_for(path, i);
        sys_unlink(path);
    }
    sys_rmdir(dir);

    if (sys_mkdir(dir, 0755) != 0) {
        fail("could not create the directory");
    }
    for (i = 0; i < FILE_COUNT; i++) {
        path_for(path, i);
        fd = sys_creat(path, 0644);
        if (fd < 0) {
            fail("could not create a file in it");
        }
        sys_close(fd);
    }

    /* Read a batch, unlink what it named, read again from the cursor. */
    fd = sys_open(dir, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        fail("could not open the directory");
    }
    for (rounds = 0; rounds < FILE_COUNT + 8; rounds++) {
        int n = sys_getdents64(fd, buf, sizeof(buf));
        int off = 0;

        if (n < 0) {
            fail("getdents64 failed part way through");
        }
        if (n == 0) {
            break;
        }
        while (off < n) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
            char victim[32];
            int k = 0;

            /* d_off, where the next read resumes, increases strictly. */
            if (d->d_off <= last_off) {
                fail("the cursor did not advance, so a read could repeat or skip entries");
            }
            last_off = d->d_off;
            if (d->d_ino == 0) {
                fail("an entry came back with no inode");
            }
            if (d->d_reclen == 0) {
                fail("an entry came back with no length, which would not advance");
            }
            /* `.` and `..`, where they are listed, are directories. */
            if (d->d_name[0] == '.' &&
                (d->d_name[1] == '\0' || (d->d_name[1] == '.' && d->d_name[2] == '\0'))) {
                if (d->d_type != DT_DIR) {
                    fail("a dot entry came back as something other than a directory");
                }
                off += d->d_reclen;
                continue;
            }
            if (d->d_type != DT_REG) {
                fail("an entry came back with the wrong type");
            }

            while (dir[k]) {
                victim[k] = dir[k];
                k++;
            }
            victim[k++] = '/';
            {
                int j = 0;

                while (d->d_name[j]) {
                    victim[k + j] = d->d_name[j];
                    j++;
                }
                victim[k + j] = '\0';
            }
            if (sys_unlink(victim) != 0) {
                fail("could not unlink a name the listing returned");
            }
            removed++;
            off += d->d_reclen;
        }
    }
    sys_close(fd);

    if (removed != FILE_COUNT) {
        fail("the listing did not name every file, so unlinking during it lost entries");
    }

    /* Two descriptors on one directory, rebuilt with four files. */
    {
        int a, b, na, nb;

        for (i = 0; i < 4; i++) {
            path_for(path, i);
            fd = sys_creat(path, 0644);
            if (fd < 0) {
                fail("could not rebuild the directory for the two-descriptor case");
            }
            sys_close(fd);
        }
        a = sys_open(dir, O_RDONLY | O_DIRECTORY);
        b = sys_open(dir, O_RDONLY | O_DIRECTORY);
        if (a < 0 || b < 0) {
            fail("could not open the directory twice");
        }
        na = sys_getdents64(a, buf, sizeof(buf));
        if (na <= 0) {
            fail("the first descriptor listed nothing");
        }
        nb = sys_getdents64(b, buf, sizeof(buf));
        if (nb != na) {
            fail("the second descriptor saw a different listing, so they share a cursor");
        }
        /* Each reports the end, and again. */
        if (sys_getdents64(a, buf, sizeof(buf)) != 0) {
            fail("the first descriptor did not report the end");
        }
        if (sys_getdents64(a, buf, sizeof(buf)) != 0) {
            fail("the end was not reported a second time, so a caller could loop");
        }
        if (sys_getdents64(b, buf, sizeof(buf)) != 0) {
            fail("the second descriptor did not report the end");
        }
        sys_close(a);
        sys_close(b);
        for (i = 0; i < 4; i++) {
            path_for(path, i);
            if (sys_unlink(path) != 0) {
                fail("could not clear the rebuilt directory");
            }
        }
    }

    /* The directory is empty, and rmdir removes it. */
    if (sys_rmdir(dir) != 0) {
        fail("the directory would not be removed, so files were left behind");
    }

    puts("UNLINKDIRPROBE OK");
    sys_exit(0);
}
