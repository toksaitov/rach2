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
 * int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT      1
#define SYS_LSEEK     19
#define SEEK_SET      0
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

static int wpid; /* the writer, stopped on a failure so the shell returns */

static void fail(const char *what)
{
    int status = 0;

    print("ZOMBIEPROBE FAIL: ");
    print(what);
    print("\n");
    if (wpid > 0) {
        sys2(SYS_KILL, wpid, SIGKILL);
        sys3(SYS_WAITPID, wpid, (int)(size_t)&status, 0);
    }
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
            sys1(SYS_EXIT, -fds[i]);
        }
        for (j = 0; j < 16; j++) {
            sys3(SYS_WRITE, fds[i], (int)(size_t)buf, (int)sizeof(buf));
        }
        sys1(SYS_UNLINK, (int)(size_t)names[i]);
    }
    for (;;) {
        for (j = 0; j < 16; j++) {
            sys3(SYS_WRITE, fds[0], (int)(size_t)buf, (int)sizeof(buf));
        }
        sys3(SYS_LSEEK, fds[0], 0, SEEK_SET);
    }
}

/* The child that holds the file system busy, so the victim's closes wait. */
static void writer(void)
{
    int fd = sys3(SYS_OPEN, (int)(size_t)"zpw", O_RDWR | O_CREAT | O_TRUNC, 0644);
    static char buf[4096];
    int i;

    if (fd < 0) {
        sys1(SYS_EXIT, -fd);
    }
    for (i = 0; i < (int)sizeof(buf); i++) {
        buf[i] = (char)(i & 0x7F);
    }
    for (;;) {
        for (i = 0; i < 64; i++) {
            sys3(SYS_WRITE, fd, (int)(size_t)buf, (int)sizeof(buf));
        }
        sys3(SYS_LSEEK, fd, 0, SEEK_SET);
    }
}

void _start(void)
{
    int vpid, status, round;

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
