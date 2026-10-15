/**
 * zombieprobe: the teardown of processes killed by a signal, beside a writer
 * on the same file system.
 *
 * ROUNDS children in turn open and unlink files and die by SIGKILL while
 * another child writes, and each is reaped. This does not force a contended
 * close and is not a mutation proof; exit_hosttest forces the waits of the
 * cleanup and checks the state it ends in.
 *
 * Every child rewrites its file in place, so the file system never fills: a
 * fast machine would fill a small image in the victim's first milliseconds,
 * and its creates would then fail for want of space, which is not what the
 * probe is about. A child whose open fails exits with the error's number, so
 * that the kernel's exit record says why. Needs a writable working
 * directory. Prints "ZOMBIEPROBE OK" after every child is reaped, or
 * "ZOMBIEPROBE FAIL: " and the step and exits 1. Freestanding; direct system
 * calls (probe.h).
 */

#include "probe.h"

#define SEEK_SET 0

#define O_RDWR  2
#define O_CREAT 0100
#define O_TRUNC 01000

#define SIGKILL 9
#define ROUNDS  40
#define HOLDERS 6

/** The length of the string `s`. */
static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

/** Write the string `s` to standard output. */
static void print(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)slen(s));
}

static int wpid; /* the writer, stopped on a failure so the shell returns */

/** Print "ZOMBIEPROBE FAIL: `what`", kill and reap the writer, and exit 1. */
static void fail(const char *what)
{
    int status = 0;

    print("ZOMBIEPROBE FAIL: ");
    print(what);
    print("\n");
    if (wpid > 0) {
        syscall2(SYS_KILL, wpid, SIGKILL);
        syscall3(SYS_WAITPID, wpid, (long)&status, 0);
    }
    syscall1(SYS_EXIT, 1);
}

static const char *names[HOLDERS] = {"zp0", "zp1", "zp2", "zp3", "zp4", "zp5"};

/** The child that is killed. It fills each of its files and unlinks it while
 * it is open, so that its teardown has that many inodes to free, the work
 * that takes the mount's lock and can wait for a buffer the writer holds;
 * then it rewrites the first file until it dies. */
static void victim(void)
{
    int fds[HOLDERS];
    static char buf[4096];
    int i, j;

    for (i = 0; i < (int)sizeof(buf); i++) {
        buf[i] = (char)(i & 0x5F);
    }
    for (i = 0; i < HOLDERS; i++) {
        fds[i] = syscall3(SYS_OPEN, (long)names[i], O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fds[i] < 0) {
            syscall1(SYS_EXIT, -fds[i]);
        }
        for (j = 0; j < 16; j++) {
            syscall3(SYS_WRITE, fds[i], (long)buf, (int)sizeof(buf));
        }
        syscall1(SYS_UNLINK, (long)names[i]);
    }
    for (;;) {
        for (j = 0; j < 16; j++) {
            syscall3(SYS_WRITE, fds[0], (long)buf, (int)sizeof(buf));
        }
        syscall3(SYS_LSEEK, fds[0], 0, SEEK_SET);
    }
}

/** The child that keeps the file system busy, so that the victim's closes
 * wait; it rewrites its file until it is killed. */
static void writer(void)
{
    int fd = syscall3(SYS_OPEN, (long)"zpw", O_RDWR | O_CREAT | O_TRUNC, 0644);
    static char buf[4096];
    int i;

    if (fd < 0) {
        syscall1(SYS_EXIT, -fd);
    }
    for (i = 0; i < (int)sizeof(buf); i++) {
        buf[i] = (char)(i & 0x7F);
    }
    for (;;) {
        for (i = 0; i < 64; i++) {
            syscall3(SYS_WRITE, fd, (long)buf, (int)sizeof(buf));
        }
        syscall3(SYS_LSEEK, fd, 0, SEEK_SET);
    }
}

/** The probe's entry: the writer, then ROUNDS victims killed and reaped. */
PROBE_START void _start(void)
{
    int vpid, status, round;

    wpid = syscall1(SYS_FORK, 0);
    if (wpid < 0) {
        fail("fork the writer");
    }
    if (wpid == 0) {
        writer();
    }

    for (round = 0; round < ROUNDS; round++) {
        vpid = syscall1(SYS_FORK, 0);
        if (vpid < 0) {
            fail("fork the victim");
        }
        if (vpid == 0) {
            victim();
        }
        {
            long req[2];

            req[0] = 0;
            req[1] = 60000000u; /* 60 ms, for the victim to fill and unlink */
            syscall2(SYS_NANOSLEEP, (long)req, 0);
        }
        if (syscall2(SYS_KILL, vpid, SIGKILL) != 0) {
            fail("kill the victim");
        }
        status = 0;
        if (syscall3(SYS_WAITPID, vpid, (long)&status, 0) != vpid) {
            fail("reap the victim");
        }
    }

    syscall2(SYS_KILL, wpid, SIGKILL);
    status = 0;
    syscall3(SYS_WAITPID, wpid, (long)&status, 0);
    syscall1(SYS_UNLINK, (long)"zpw");

    print("ZOMBIEPROBE OK\n");
    syscall1(SYS_EXIT, 0);
}
