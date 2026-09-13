/**
 * Kills processes whose teardown has to wait, and requires the machine to go
 * on running.
 *
 * A process killed by a signal is marked a zombie before its descriptors are
 * closed, and closing one on a mounted file system can wait: an ext2 close
 * takes the mount's namespace lock, which another process may hold. A wait
 * ends with the waiter RUNNING whatever it was before, so the dying process
 * was made READY again, picked again, and returned from its last `schedule`
 * into a halt taken with interrupts off. Nothing could end that halt: no
 * keyboard, no timer, no network, and no record of why. That is what this
 * probe makes happen, on purpose, many times over.
 *
 * The arrangement: one child writes the same file system in a loop, which is
 * what the other child's teardown has to wait for; the other fills six files,
 * unlinks each while it is still open, and is then killed with SIGKILL, so its
 * teardown has six inodes to free before it can end, and is reaped.
 * A kernel with the fault stops the machine somewhere in these rounds, and the
 * suite that runs this probe fails on its deadline with nothing more printed.
 *
 * What this row does not do is prove the repair. The wait it is built to
 * provoke did not happen in any run of it, on the boot root or on an ext2
 * root, with the fault compiled back in: forty rounds passed either way. It is
 * kept as the stress the machine was under when it stopped, not as the proof,
 * and what the repair rests on is the reading recorded with entry 56.
 *
 * Prints "ZOMBIEPROBE OK" when every round was reaped. Freestanding; direct
 * int 0x80.
 *
 * Needs: a writable working directory. It makes its files there and removes
 * them.
 */

typedef unsigned int size_t;

#define SYS_EXIT      1
#define SYS_FORK      2
#define SYS_WRITE     4
#define SYS_OPEN      5
#define SYS_CLOSE     6
#define SYS_WAITPID   7
#define SYS_UNLINK    10
#define SYS_KILL      37
#define SYS_NANOSLEEP 162

#define O_RDWR  2
#define O_CREAT 0100
#define O_TRUNC 01000

#define SIGKILL 9
#define ROUNDS  40
#define HOLDERS 6

static int sys1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a));
    return r;
}

static int sys2(int n, int a, int b)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b));
    return r;
}

static int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c));
    return r;
}

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
    sys3(SYS_WRITE, 1, (int)(size_t)s, (int)slen(s));
}

static void fail(const char *what)
{
    print("ZOMBIEPROBE FAIL: ");
    print(what);
    print("\n");
    sys1(SYS_EXIT, 1);
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
        fds[i] = sys3(SYS_OPEN, (int)(size_t)names[i], O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fds[i] < 0) {
            sys1(SYS_EXIT, 1);
        }
        for (j = 0; j < 16; j++) {
            sys3(SYS_WRITE, fds[i], (int)(size_t)buf, (int)sizeof(buf));
        }
        sys1(SYS_UNLINK, (int)(size_t)names[i]);
    }
    for (;;) {
        sys3(SYS_WRITE, fds[0], (int)(size_t)buf, (int)sizeof(buf));
    }
}

/* The child that holds the file system busy, so the victim's closes wait. */
static void writer(void)
{
    int fd = sys3(SYS_OPEN, (int)(size_t)"zpw", O_RDWR | O_CREAT | O_TRUNC, 0644);
    static char buf[4096];
    int i;

    if (fd < 0) {
        sys1(SYS_EXIT, 1);
    }
    for (i = 0; i < (int)sizeof(buf); i++) {
        buf[i] = (char)(i & 0x7F);
    }
    for (;;) {
        sys3(SYS_WRITE, fd, (int)(size_t)buf, (int)sizeof(buf));
    }
}

void _start(void)
{
    int wpid, vpid, status, round;

    wpid = sys1(SYS_FORK, 0);
    if (wpid < 0) {
        fail("fork the writer");
    }
    if (wpid == 0) {
        writer();
    }

    for (round = 0; round < ROUNDS; round++) {
        vpid = sys1(SYS_FORK, 0);
        if (vpid < 0) {
            fail("fork the victim");
        }
        if (vpid == 0) {
            victim();
        }
        {
            unsigned int req[2];

            req[0] = 0;
            req[1] = 60000000u; /* 60 ms, long enough for the victim to fill and unlink */
            sys2(SYS_NANOSLEEP, (int)(size_t)req, 0);
        }
        if (sys2(SYS_KILL, vpid, SIGKILL) != 0) {
            fail("kill the victim");
        }
        status = 0;
        if (sys3(SYS_WAITPID, vpid, (int)(size_t)&status, 0) != vpid) {
            fail("reap the victim");
        }
    }

    sys2(SYS_KILL, wpid, SIGKILL);
    status = 0;
    sys3(SYS_WAITPID, wpid, (int)(size_t)&status, 0);
    sys1(SYS_UNLINK, (int)(size_t)"zpw");

    print("ZOMBIEPROBE OK\n");
    sys1(SYS_EXIT, 0);
}
