/**
 * Every operation that changes the file system stamps what it changed.
 *
 * One assertion per site, because a probe that only writes a file leaves every
 * other site free to be wrong: create, write, truncate, rename within a
 * directory and across two, rename onto an existing name, unlink, unlink of a
 * file somebody still holds open (which takes a different path in the kernel
 * and never reaches krffs_unlink), mkdir, rmdir, chmod and chown.
 *
 * Two rules are asserted throughout. Changing a file's CONTENTS moves its
 * modification time and its attribute-change time together; changing only its
 * attributes moves the attribute-change time and leaves the modification time
 * where it was. And a directory is a file whose contents are its entries, so
 * adding or removing a name in it moves ITS modification time.
 *
 * The root is proved here too, which is the one place it can be: `utimensat`
 * reports success for `/` without modifying it, so the stat probe cannot set a
 * time there to read back. Creating a file in the root stamps it, and all three
 * paths that report the root (stat, fstatat, and the old ABI by number) must
 * then agree about when.
 *
 * Every comparison waits out a tick first. The clock advances every 10 ms and
 * two operations in the same tick share a timestamp by design, so a test that
 * did not wait would fail for a reason that is not a defect.
 *
 * Prints "STAMPMUSL OK" on success and "STAMPMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/*
 * The old stat(2) struct, as the kernel writes it (struct linux_stat in
 * rach2/sys/syscall.c). Declared here because musl never issues that call on
 * i386: stat() goes to statx, which this kernel does not have, and falls back
 * to fstatat64. The root has a branch of its own in that fill, so it is asked
 * for by number.
 */
struct old_stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned short st_mode;
    unsigned short st_nlink;
    unsigned short st_uid;
    unsigned short st_gid;
    unsigned long st_rdev;
    unsigned long st_size;
    unsigned long st_blksize;
    unsigned long st_blocks;
    unsigned long st_atime_seconds;
    unsigned long st_atime_nanoseconds;
    unsigned long st_mtime_seconds;
    unsigned long st_mtime_nanoseconds;
    unsigned long st_ctime_seconds;
    unsigned long st_ctime_nanoseconds;
    unsigned long unused4;
    unsigned long unused5;
};

static const char *const DIR = "/stampdir";
static const char *const OTHER = "/stampdir2";
static const char *const FILE_IN_DIR = "/stampdir/f";
static const char *const MOVED = "/stampdir/g";
static const char *const ACROSS = "/stampdir2/h";
static const char *const IN_ROOT = "/stamproot.tmp";
static const char *const FILLER = "/stampfill.tmp";

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("STAMPMUSL FAIL (%s)\n", what);

    return 1;
}

/** Sleep past a clock tick, so two stamps cannot share an instant. */
static void tick(void)
{
    struct timespec how_long;

    how_long.tv_sec = 0;
    how_long.tv_nsec = 60000000L; /* 60 ms, six ticks of the 100 Hz clock */
    nanosleep(&how_long, (struct timespec *)0);
}

/**
 * A path's modification time in milliseconds since the epoch.
 *
 * `long long`, not `long`: this is a 32-bit machine, and the epoch in
 * milliseconds passed what a 32-bit long can hold in 1970 plus 25 days.
 */
static long long mtime_of(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return -1;
    }

    return (long long)st.st_mtim.tv_sec * 1000 + (long long)st.st_mtim.tv_nsec / 1000000;
}

/** A path's attribute-change time, in the same milliseconds. */
static long long ctime_of(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return -1;
    }

    return (long long)st.st_ctim.tv_sec * 1000 + (long long)st.st_ctim.tv_nsec / 1000000;
}

int main(void)
{
    struct stat st;
    long long before;
    long long after;
    long long ctime_before;
    int fd;
    int i;

    /* Create: the file is born now, and the directory it appeared in changed. */
    if (mkdir(DIR, 0755) != 0 || mkdir(OTHER, 0755) != 0) {
        return fail("could not create the directories");
    }
    before = mtime_of(DIR);
    tick();
    fd = open(FILE_IN_DIR, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        return fail("could not create the file");
    }
    if (mtime_of(FILE_IN_DIR) <= 0) {
        return fail("a new file was not stamped when it was created");
    }
    after = mtime_of(DIR);
    if (after <= before) {
        return fail("creating a file did not stamp its directory");
    }

    /* Write: the contents changed, so both times move. */
    before = mtime_of(FILE_IN_DIR);
    ctime_before = ctime_of(FILE_IN_DIR);
    tick();
    if (write(fd, "hello", 5) != 5) {
        return fail("could not write the file");
    }
    if (mtime_of(FILE_IN_DIR) <= before) {
        return fail("a write did not stamp the file");
    }
    if (ctime_of(FILE_IN_DIR) <= ctime_before) {
        return fail("a write did not move the attribute-change time");
    }
    close(fd);

    /* A write of NOTHING changes nothing, times included, and it must not grow
     * the file: the bytes it would publish are whatever the allocator last left
     * there. */
    fd = open(FILE_IN_DIR, O_WRONLY);
    if (fd < 0) {
        return fail("could not reopen the file");
    }
    before = mtime_of(FILE_IN_DIR);
    tick();
    if (lseek(fd, 4096, SEEK_SET) < 0) {
        close(fd);
        return fail("could not seek past the end");
    }
    if (write(fd, "", 0) != 0) {
        close(fd);
        return fail("a zero-byte write did not report zero");
    }
    if (mtime_of(FILE_IN_DIR) != before) {
        close(fd);
        return fail("a zero-byte write stamped the file");
    }
    if (stat(FILE_IN_DIR, &st) != 0 || st.st_size != 5) {
        close(fd);
        return fail("a zero-byte write past the end grew the file");
    }

    /* A write that DOES start past the end leaves a hole, and the hole must
     * read as zeros rather than as whatever was in the free space.
     *
     * The space is dirtied first, on purpose. Reading zeros out of a hole
     * proves nothing when the bytes underneath were zeros anyway: this fills a
     * scratch file with a pattern, frees it, and only then makes the hole, so
     * the allocator has something recognisable to hand back. Without the
     * zero-fill in the kernel, those are the bytes that come out. */
    close(fd);
    {
        char pattern[512];
        int filler;
        int written = 0;

        for (i = 0; i < (int)sizeof(pattern); i++) {
            pattern[i] = (char)0xA5;
        }
        filler = open(FILLER, O_CREAT | O_RDWR, 0644);
        if (filler < 0) {
            return fail("could not create the filler file");
        }
        while (written < 8192) {
            if (write(filler, pattern, sizeof(pattern)) != (ssize_t)sizeof(pattern)) {
                close(filler);
                return fail("could not fill the filler file");
            }
            written += (int)sizeof(pattern);
        }
        close(filler);
        if (unlink(FILLER) != 0) {
            return fail("could not remove the filler file");
        }
    }

    fd = open(FILE_IN_DIR, O_WRONLY);
    if (fd < 0) {
        return fail("could not reopen the file to make a hole");
    }
    if (lseek(fd, 4096, SEEK_SET) < 0 || write(fd, "z", 1) != 1) {
        close(fd);
        return fail("could not write past the end");
    }
    close(fd);
    {
        char buffer[64];
        int reader = open(FILE_IN_DIR, O_RDONLY);

        if (reader < 0) {
            return fail("could not reopen the file to read the hole");
        }
        if (lseek(reader, 5, SEEK_SET) < 0 ||
            read(reader, buffer, sizeof(buffer)) != (ssize_t)sizeof(buffer)) {
            close(reader);
            return fail("could not read back the hole");
        }
        close(reader);
        for (i = 0; i < (int)sizeof(buffer); i++) {
            if (buffer[i] != '\0') {
                char detail[96];

                snprintf(detail, sizeof(detail),
                         "the hole is not zeroed: byte %d of it reads 0x%02x", i,
                         (unsigned char)buffer[i]);

                return fail(detail);
            }
        }
    }

    /* Truncate, through the open that does it: same rule. */
    before = mtime_of(FILE_IN_DIR);
    tick();
    fd = open(FILE_IN_DIR, O_WRONLY | O_TRUNC);
    if (fd < 0) {
        return fail("could not truncate the file");
    }
    close(fd);
    if (mtime_of(FILE_IN_DIR) <= before) {
        return fail("a truncate did not stamp the file");
    }

    /* chmod and chown change attributes and nothing else, so the modification
     * time must hold still while the attribute-change time moves. */
    before = mtime_of(FILE_IN_DIR);
    ctime_before = ctime_of(FILE_IN_DIR);
    tick();
    if (chmod(FILE_IN_DIR, 0600) != 0) {
        return fail("chmod failed");
    }
    if (ctime_of(FILE_IN_DIR) <= ctime_before) {
        return fail("chmod did not move the attribute-change time");
    }
    if (mtime_of(FILE_IN_DIR) != before) {
        return fail("chmod moved the modification time, which it must not");
    }
    ctime_before = ctime_of(FILE_IN_DIR);
    tick();
    /* A GROUP-only chown, which is the case a stamp written inside the uid
     * branch never reaches: the uid here is -1, so that branch does not run. */
    if (chown(FILE_IN_DIR, (uid_t)-1, 1) != 0) {
        return fail("a group-only chown failed");
    }
    if (ctime_of(FILE_IN_DIR) <= ctime_before) {
        return fail("a group-only chown did not move the attribute-change time");
    }
    if (mtime_of(FILE_IN_DIR) != before) {
        return fail("chown moved the modification time, which it must not");
    }

    /* And a chown that changes nothing changes no time. Any caller may make
     * that call, so a stamp on it would be a way to touch a file one has no
     * right to touch. */
    ctime_before = ctime_of(FILE_IN_DIR);
    tick();
    if (chown(FILE_IN_DIR, (uid_t)-1, (gid_t)-1) != 0) {
        return fail("a chown that changes nothing failed");
    }
    if (ctime_of(FILE_IN_DIR) != ctime_before) {
        return fail("a chown that changed nothing still moved a time");
    }

    /* Rename inside one directory: that directory changed twice over, and the
     * file's attributes changed while its contents did not. */
    before = mtime_of(DIR);
    ctime_before = ctime_of(FILE_IN_DIR);
    tick();
    if (rename(FILE_IN_DIR, MOVED) != 0) {
        return fail("rename failed");
    }
    if (mtime_of(DIR) <= before) {
        return fail("a rename did not stamp the directory it happened in");
    }
    if (ctime_of(MOVED) <= ctime_before) {
        return fail("a rename did not move the file's attribute-change time");
    }

    /* Renaming a file to the name it already has does nothing, and doing
     * nothing includes not moving a time. */
    before = mtime_of(DIR);
    ctime_before = ctime_of(MOVED);
    tick();
    if (rename(MOVED, MOVED) != 0) {
        return fail("renaming a file to its own name failed");
    }
    if (mtime_of(DIR) != before) {
        return fail("a rename to the same name stamped the directory");
    }
    if (ctime_of(MOVED) != ctime_before) {
        return fail("a rename to the same name stamped the file");
    }

    /* Rename ACROSS two directories: both ends changed. */
    before = mtime_of(DIR);
    after = mtime_of(OTHER);
    tick();
    if (rename(MOVED, ACROSS) != 0) {
        return fail("rename across directories failed");
    }
    if (mtime_of(DIR) <= before) {
        return fail("a rename did not stamp the directory the name left");
    }
    if (mtime_of(OTHER) <= after) {
        return fail("a rename did not stamp the directory the name arrived in");
    }

    /* Rename ONTO an existing name, which removes an entry as well as adding
     * one, and is the case a stamp placed after the removal would miss. */
    fd = open(MOVED, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        return fail("could not create the file to be replaced");
    }
    close(fd);
    before = mtime_of(DIR);
    tick();
    if (rename(ACROSS, MOVED) != 0) {
        return fail("rename onto an existing name failed");
    }
    if (mtime_of(DIR) <= before) {
        return fail("a rename onto an existing name did not stamp the directory");
    }

    /* Unlink: the directory changed. */
    before = mtime_of(DIR);
    tick();
    if (unlink(MOVED) != 0) {
        return fail("unlink failed");
    }
    if (mtime_of(DIR) <= before) {
        return fail("an unlink did not stamp the directory");
    }

    /* Unlink of a file this program still holds open. The kernel orphans the
     * node and returns before it reaches the ordinary removal, so a stamp
     * placed only there would never run on this path. */
    fd = open(FILE_IN_DIR, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        return fail("could not create the file to unlink while open");
    }
    before = mtime_of(DIR);
    tick();
    if (unlink(FILE_IN_DIR) != 0) {
        close(fd);
        return fail("unlink of an open file failed");
    }
    if (mtime_of(DIR) <= before) {
        close(fd);
        return fail("unlinking an open file did not stamp the directory");
    }
    close(fd);

    /* mkdir and rmdir, on the parent. */
    before = mtime_of(DIR);
    tick();
    if (mkdir("/stampdir/sub", 0755) != 0) {
        return fail("mkdir failed");
    }
    if (mtime_of("/stampdir/sub") <= 0) {
        return fail("a new directory was not stamped when it was created");
    }
    if (mtime_of(DIR) <= before) {
        return fail("mkdir did not stamp its parent");
    }
    before = mtime_of(DIR);
    tick();
    if (rmdir("/stampdir/sub") != 0) {
        return fail("rmdir failed");
    }
    if (mtime_of(DIR) <= before) {
        return fail("rmdir did not stamp its parent");
    }

    /* The ROOT, which is the one directory the stat probe cannot set a time on:
     * utimensat reports success for it and changes nothing. Creating a file in
     * it stamps it, and every path that reports the root must agree. */
    before = mtime_of("/");
    tick();
    fd = open(IN_ROOT, O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        return fail("could not create a file in the root");
    }
    close(fd);
    after = mtime_of("/");
    if (after <= before) {
        return fail("creating a file in the root did not stamp it");
    }
    {
        int dirfd = open("/", O_RDONLY | O_DIRECTORY);

        if (dirfd < 0) {
            return fail("could not open the root");
        }
        if (fstat(dirfd, &st) != 0) {
            close(dirfd);
            return fail("could not fstat the root");
        }
        if ((long long)st.st_mtim.tv_sec * 1000 + (long long)st.st_mtim.tv_nsec / 1000000 !=
            after) {
            close(dirfd);
            return fail("a descriptor on the root reports a different time than a path to it");
        }
        close(dirfd);
    }
    {
        /* The old stat(2) has a root branch of its own, and musl never issues
         * that call on i386, so it is asked for by number or not at all. */
        struct old_stat os;

        if (syscall(SYS_stat, "/", &os) != 0) {
            return fail("the old stat(2) of the root failed");
        }
        if ((long long)os.st_mtime_seconds * 1000 + (long long)os.st_mtime_nanoseconds / 1000000 !=
            after) {
            return fail("the old stat(2) reports a different time for the root");
        }
    }
    if (fstatat(AT_FDCWD, "/", &st, 0) != 0) {
        return fail("could not fstatat the root");
    }
    if ((long long)st.st_mtim.tv_sec * 1000 + (long long)st.st_mtim.tv_nsec / 1000000 != after) {
        return fail("fstatat reports a different time for the root than stat does");
    }
    unlink(IN_ROOT);
    rmdir(DIR);
    rmdir(OTHER);

    printf("STAMPMUSL OK\n");

    return 0;
}
