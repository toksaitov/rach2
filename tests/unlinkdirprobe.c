/**
 * Removing a directory's files while listing it, the way `rm -rf` does.
 *
 * This is the shape of a real failure. `rm -rf` opens a directory, reads a
 * batch of names, unlinks them, and reads again from where it left off. If the
 * kernel's directory cursor counts children rather than naming a position that
 * survives deletion, every unlink renumbers the survivors and the second read
 * skips one entry per file already removed. The skipped files stay, `rmdir`
 * then refuses, and the only thing the user sees is "Directory not empty".
 *
 * It was found by `make selfbuild-test`, whose second pass could not remove
 * the kernel source tree it had just copied, and only once that tree had grown
 * enough files that listing it took more than one call.
 *
 * So this makes a directory with more entries than one modest buffer has,
 * removes them the way `rm` does, and requires the directory to be empty and
 * removable afterwards. A single large read would not reproduce it, which is
 * why the buffer here is deliberately small.
 *
 * Prints "UNLINKDIRPROBE OK" and exits 0 on success. Freestanding; int 0x80.
 */

typedef unsigned int uint32_t;
typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;
typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT       1
#define SYS_WRITE      4
#define SYS_OPEN       5
#define SYS_CLOSE      6
#define SYS_CREAT      8
#define SYS_UNLINK     10
#define SYS_MKDIR      39
#define SYS_RMDIR      40
#define SYS_GETDENTS64 220

#define O_RDONLY    0
#define O_DIRECTORY 0200000

#define DT_REG 8

/* Enough files that one small read cannot hold them all, so the listing takes
   several calls and the cursor has to survive the unlinks between them. */
#define FILE_COUNT 40

/* Deliberately small: the defect hides entirely when every name fits in one
   read, which is why the tree that found it had to grow first. */
#define DIRENT_BUF 256

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
};

static int sys_write(int fd, const void *buf, size_t count)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count)
                         : "memory");
    return result;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
    __builtin_unreachable();
}

static int sys_open(const char *path, int flags)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(SYS_OPEN), "b"(path), "c"(flags)
                         : "memory");
    return result;
}

static int sys_close(int fd)
{
    int result;
    __asm__ __volatile__("int $0x80" : "=a"(result) : "0"(SYS_CLOSE), "b"(fd) : "memory");
    return result;
}

static int sys_creat(const char *path, int mode)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(SYS_CREAT), "b"(path), "c"(mode)
                         : "memory");
    return result;
}

static int sys_unlink(const char *path)
{
    int result;
    __asm__ __volatile__("int $0x80" : "=a"(result) : "0"(SYS_UNLINK), "b"(path) : "memory");
    return result;
}

static int sys_mkdir(const char *path, int mode)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(SYS_MKDIR), "b"(path), "c"(mode)
                         : "memory");
    return result;
}

static int sys_rmdir(const char *path)
{
    int result;
    __asm__ __volatile__("int $0x80" : "=a"(result) : "0"(SYS_RMDIR), "b"(path) : "memory");
    return result;
}

static int sys_getdents64(int fd, void *dirp, unsigned int count)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(SYS_GETDENTS64), "b"(fd), "c"(dirp), "d"(count)
                         : "memory");
    return result;
}

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

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

void _start(void)
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

    /* Now the part that matters: read a batch, unlink what it named, read
     * again from the cursor the kernel left. This is what `rm -rf` does, and
     * the cursor has to name a position that the unlinks did not move. */
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

            /* The record's own fields, checked rather than skipped past. The
             * cursor is what this probe is about: `d_off` names where the next
             * read resumes, so it must increase strictly, and it must keep
             * increasing across the unlinks between reads. A cursor that
             * counted surviving children would go backwards here. */
            if (d->d_off <= last_off) {
                fail("the cursor did not advance, so a read could repeat or skip entries");
            }
            last_off = d->d_off;
            if (d->d_ino == 0) {
                fail("an entry came back with no inode");
            }
            if (d->d_type != DT_REG) {
                fail("an entry came back with the wrong type");
            }
            if (d->d_reclen == 0) {
                fail("an entry came back with no length, which would not advance");
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

    /* Two descriptors on one directory must not share a cursor: each has
     * its own offset in its open file description, and a listing that consumed
     * another's position would skip entries for reasons no caller could see.
     * The directory is rebuilt for this, since the loop above emptied it. */
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
        /* And each reports the end independently, repeatedly. */
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

    /* The whole point: the directory must now be empty, and `rmdir` is the
     * check a user would actually meet. */
    if (sys_rmdir(dir) != 0) {
        fail("the directory would not be removed, so files were left behind");
    }

    puts("UNLINKDIRPROBE OK");
    sys_exit(0);
}
