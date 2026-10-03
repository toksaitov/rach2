/**
 * Stress signal teardown beside a writer on the same file system.
 *
 * Forty children open and unlink files, then die by SIGKILL while another
 * child writes. This does not force a contended close and is not a mutation
 * proof. exit_hosttest forces the cleanup waits and checks the terminal state.
 *
 * Every writer rewrites its file in place, so the file system never fills:
 * a fast machine fills a small module in the victim's first milliseconds,
 * and its creates then fail for space, which is not what the probe is
 * about. A child whose open fails exits with the error's number, so the
 * kernel's exit record says why. Needs a writable working directory. Prints
 * "ZOMBIEPROBE OK" after all children are reaped. Freestanding; direct
 * system calls (probe.h).
 */

#include "probe.h"

#define SEEK_SET 0

#define O_RDWR  2
#define O_CREAT 0100
#define O_TRUNC 01000

#define SIGKILL 9
#define ROUNDS  40
#define HOLDERS 6

static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void print(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)slen(s));
}

static int wpid; /* the writer, stopped on a failure so the shell returns */

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

/* The child that is killed. It fills each fixture and unlinks it while it is
   still open, so its teardown has that many inodes to free, which is the work
   that takes the mount's lock and can wait for a buffer the writer holds. */
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

/* The child that holds the file system busy, so the victim's closes wait. */
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
            req[1] = 60000000u; /* 60 ms, long enough for the victim to fill and unlink */
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
