/**
 * permprobe: the nine permission bits, checked by each call that reads them.
 *
 * A root parent stages the fixtures (a 0600 root file, a 0666 root file, a
 * 0700 root directory, a 0700 directory owned by uid 1000), then a child
 * drops to 1000 and is denied everything the bits deny: writing a 0644
 * root file, reading a 0600 root file, creating or unlinking or renaming in
 * the root directory, chdir into a 0700 root directory, exec of a file with
 * no execute bit, and setting explicit utimensat times on a root file it can
 * only write; while its own directory grants creation and entry, the
 * other-write bit grants the utimensat touch to "now", and access(2) answers
 * with the real ids. The root parent then removes every fixture through its
 * override, which is itself the override's proof.
 *
 * Prints "PERMPROBE OK" and exits 0 on success; prints "PERMPROBE FAIL: "
 * and the failing step and exits 1 otherwise.
 */

#include "probe.h"

typedef unsigned int u32;

#define STDOUT   1
#define EPERM    1
#define EACCES   13
#define EBADF    9
#define O_RDONLY 0
#define O_WRONLY 1
#define R_OK     4
#define W_OK     2
#define AT_FDCWD (-100)

/* utimensat tv_nsec sentinels (each timespec is {sec, nsec}, [atime,
 * mtime]). */
#define UTIME_NOW  ((1 << 30) - 1)
#define UTIME_OMIT ((1 << 30) - 2)

/** The length of the string `s`. */
static u32 slen(const char *s)
{
    u32 n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Write `s` to standard output, with no newline. */
static void puts_(const char *s)
{
    syscall3(SYS_WRITE, STDOUT, (long)s, (int)slen(s));
}

/** Print "PERMPROBE FAIL: " and `step`, and exit 1. */
static void fail(const char *step)
{
    puts_("PERMPROBE FAIL: ");
    puts_(step);
    puts_("\n");
    syscall3(SYS_EXIT, 1, 0, 0);
}

/**
 * The child's half: drop to uid and gid 1000 and make each denied call and
 * each granted one. It exits 0 when every answer is right, and 1 through
 * `fail` at the first that is not.
 *
 * utimensat splits its rights: write permission grants a touch to "now"; an
 * explicit time, or any pair not both UTIME_NOW, is the owner's or root's
 * alone; and a request with both UTIME_OMIT changes nothing and needs no
 * right.
 */
static void child_main(void)
{
    static char *exec_argv[2];
    int fd;

    if (syscall3(SYS_SETRESGID, 1000, 1000, 1000) != 0 ||
        syscall3(SYS_SETRESUID, 1000, 1000, 1000) != 0) {
        fail("drop to 1000");
    }

    /* Other may read a 0644 file and may not write it. */
    fd = syscall3(SYS_OPEN, (long)"/hello.txt", O_RDONLY, 0);
    if (fd < 0) {
        fail("read 0644 as other");
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    if (syscall3(SYS_OPEN, (long)"/hello.txt", O_WRONLY, 0) != -EACCES) {
        fail("write 0644 as other not EACCES");
    }
    if (syscall3(SYS_OPEN, (long)"/perm600", O_RDONLY, 0) != -EACCES) {
        fail("read 0600 as other not EACCES");
    }

    /* The root directory is 0755: no creation, removal, or renaming by 1000. */
    if (syscall3(SYS_CREAT, (long)"/denied.txt", 0644, 0) != -EACCES) {
        fail("create in / not EACCES");
    }
    if (syscall3(SYS_UNLINK, (long)"/hello.txt", 0, 0) != -EACCES) {
        fail("unlink in / not EACCES");
    }
    if (syscall3(SYS_RENAME, (long)"/hello.txt", (long)"/perm.own/h", 0) != -EACCES) {
        fail("rename out of / not EACCES");
    }
    if (syscall3(SYS_MKDIR, (long)"/denied.d", 0755, 0) != -EACCES) {
        fail("mkdir in / not EACCES");
    }

    /* Search of a directory and exec of a file each need the x bit. */
    if (syscall3(SYS_CHDIR, (long)"/perm.priv", 0, 0) != -EACCES) {
        fail("chdir into 0700 root dir not EACCES");
    }
    exec_argv[0] = (char *)"/hello.txt";
    exec_argv[1] = 0;
    if (syscall3(SYS_EXECVE, (long)"/hello.txt", (long)exec_argv, 0) != -EACCES) {
        fail("exec of non-executable not EACCES");
    }

    /* The child's own directory grants entry and creation. */
    if (syscall3(SYS_CHDIR, (long)"/perm.own", 0, 0) != 0) {
        fail("chdir into own dir");
    }
    fd = syscall3(SYS_CREAT, (long)"/perm.own/mine.txt", 0600, 0);
    if (fd < 0) {
        fail("create in own dir");
    }
    syscall3(SYS_CLOSE, fd, 0, 0);

    /* The utimensat split of rights. */
    {
        static const long expl[4] = {5, 0, 5, 0};
        static const long mixed[4] = {0, UTIME_NOW, 0, UTIME_OMIT};
        static const long omit[4] = {0, UTIME_OMIT, 0, UTIME_OMIT};

        if (syscall4(SYS_UTIMENSAT, AT_FDCWD, (long)"/perm.rw", 0, 0) != 0) {
            fail("touch through other-write");
        }
        if (syscall4(SYS_UTIMENSAT, AT_FDCWD, (long)"/perm.rw", (long)expl, 0) != -EPERM) {
            fail("explicit times as non-owner not EPERM");
        }
        if (syscall4(SYS_UTIMENSAT, AT_FDCWD, (long)"/perm.rw", (long)mixed, 0) != -EPERM) {
            fail("mixed NOW/OMIT as non-owner not EPERM");
        }
        if (syscall4(SYS_UTIMENSAT, AT_FDCWD, (long)"/hello.txt", 0, 0) != -EACCES) {
            fail("touch without write not EACCES");
        }
        if (syscall4(SYS_UTIMENSAT, AT_FDCWD, (long)"/hello.txt", (long)omit, 0) != 0) {
            fail("both-OMIT needs no right");
        }
        if (syscall4(SYS_UTIMENSAT, AT_FDCWD, (long)"/perm.own/mine.txt", (long)expl, 0) != 0) {
            fail("explicit times as owner");
        }
    }

    /* access(2) answers with the real ids. */
    if (syscall3(SYS_ACCESS, (long)"/hello.txt", R_OK, 0) != 0 ||
        syscall3(SYS_ACCESS, (long)"/hello.txt", W_OK, 0) != -EACCES) {
        fail("access real-id answers");
    }

    syscall3(SYS_EXIT, 0, 0, 0);
}

/**
 * The probe: root's own two refusals, the fixtures, the child's walk, and
 * root's removal of the fixtures. /perm.rw is world-writable and not the
 * child's, so the child has write permission on it without ownership, the
 * pair the utimensat split tells apart; chmod sets its mode again after the
 * creat, which applies the umask.
 */
PROBE_START void _start(void)
{
    static char *exec_argv[2];
    int pid, status;
    int fd;

    /* Exec of a file with no execute bit is -EACCES even for root. */
    exec_argv[0] = (char *)"/hello.txt";
    exec_argv[1] = 0;
    if (syscall3(SYS_EXECVE, (long)"/hello.txt", (long)exec_argv, 0) != -EACCES) {
        fail("root exec of non-executable not EACCES");
    }

    /* creat's descriptor is write-only, so a read is -EBADF even for root. */
    fd = syscall3(SYS_CREAT, (long)"/perm.wo", 0644, 0);
    if (fd < 0) {
        fail("creat write-only");
    }
    {
        char c;
        if (syscall3(SYS_READ, fd, (long)&c, 1) != -EBADF) {
            fail("read through creat fd not EBADF");
        }
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    syscall3(SYS_UNLINK, (long)"/perm.wo", 0, 0);

    /* Root stages the fixtures. */
    fd = syscall3(SYS_CREAT, (long)"/perm600", 0600, 0);
    if (fd < 0) {
        fail("stage perm600");
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    if (syscall3(SYS_MKDIR, (long)"/perm.priv", 0700, 0) != 0 ||
        syscall3(SYS_MKDIR, (long)"/perm.own", 0700, 0) != 0 ||
        syscall3(SYS_CHOWN, (long)"/perm.own", 1000, 1000) != 0) {
        fail("stage directories");
    }
    /* The 0666 fixture, its mode set again past the umask. */
    fd = syscall3(SYS_CREAT, (long)"/perm.rw", 0666, 0);
    if (fd < 0 || syscall3(SYS_CLOSE, fd, 0, 0) != 0 ||
        syscall3(SYS_CHMOD, (long)"/perm.rw", 0666, 0) != 0) {
        fail("stage perm.rw");
    }

    pid = syscall3(SYS_FORK, 0, 0, 0);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        child_main();
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || status != 0) {
        fail("denied walk");
    }

    /* Root removes every fixture, uid 1000's included, through its override. */
    if (syscall3(SYS_UNLINK, (long)"/perm.own/mine.txt", 0, 0) != 0 ||
        syscall3(SYS_RMDIR, (long)"/perm.own", 0, 0) != 0 ||
        syscall3(SYS_RMDIR, (long)"/perm.priv", 0, 0) != 0 ||
        syscall3(SYS_UNLINK, (long)"/perm600", 0, 0) != 0 ||
        syscall3(SYS_UNLINK, (long)"/perm.rw", 0, 0) != 0) {
        fail("root override cleanup");
    }

    puts_("PERMPROBE OK\n");
    syscall3(SYS_EXIT, 0, 0, 0);
}
