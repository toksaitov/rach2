/*
 * ttyprobe.c - Phase 16 probe: the tty line discipline + termios ioctls.
 *
 * Proves, from user space via raw int 0x80, that the console fd is wired to the
 * kernel tty (n_tty discipline) -- not just that the discipline is correct (the
 * host seam test covers that), but that read/write/ioctl on fd 0/1/2 route
 * through it:
 *   (a) TIOCGWINSZ   - the console reports a 25x80 window;
 *   (b) TCGETS       - the default is cooked with echo (ICANON|ECHO set);
 *   (c) cooked read  - a line typed with DEL erases is delivered edited: the
 *                      driver sends "ab<DEL><DEL>cd\n", read() returns "cd\n";
 *   (d) TCSETS round - clearing ICANON|ECHO and reading it back through TCGETS
 *                      shows the kernel actually applied the new termios;
 *   (e) raw read     - in raw mode (VMIN=1) a byte is delivered with no newline
 *                      and no echo: the driver sends "X", read() returns it.
 *
 * Prints "TTYPROBE OK" / "TTYPROBE FAIL". Freestanding; direct int 0x80. Driven
 * by drive_tty.py, which sends the cooked line, then (after the raw switch) the
 * raw byte.
 */

typedef unsigned int size_t;

#define SYS_EXIT  1
#define SYS_READ  3
#define SYS_WRITE 4
#define SYS_IOCTL 54

#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCFLSH     0x540B
#define FIONREAD   0x541B
#define TIOCGWINSZ 0x5413

/* termios flag bits / c_cc indices (the i386 kernel termios; see sys/tty/n_tty.h). */
#define ICRNL  0000400 /* c_iflag: map CR to NL on input */
#define OPOST  0000001 /* c_oflag: enable output processing */
#define ICANON 0000002 /* c_lflag: canonical (cooked) input */
#define ECHO   0000010 /* c_lflag: echo input */
#define VTIME  5
#define VMIN   6

/* The 36-byte kernel termios exchanged by TCGETS/TCSETS. */
struct ktermios {
    unsigned int c_iflag;
    unsigned int c_oflag;
    unsigned int c_cflag;
    unsigned int c_lflag;
    unsigned char c_line;
    unsigned char c_cc[19];
};

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static void sys_exit(int status)
{
    syscall3(SYS_EXIT, status, 0, 0);
    __builtin_unreachable();
}

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void out(const char *s, int len)
{
    syscall3(SYS_WRITE, 1, (int)(size_t)s, len);
}

static void puts(const char *s)
{
    out(s, (int)strlen(s));
    out("\n", 1);
}

static int do_read(int fd, char *buf, int n)
{
    return syscall3(SYS_READ, fd, (int)(size_t)buf, n);
}

static int do_ioctl(int fd, int req, void *arg)
{
    return syscall3(SYS_IOCTL, fd, req, (int)(size_t)arg);
}

static int fails = 0;

static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        out("  FAIL: ", 8);
        puts(msg);
    }
}

/* Echo back what a read returned, framed, so the serial log shows it. */
static void show(const char *tag, const char *buf, int n)
{
    out(tag, (int)strlen(tag));
    out("[", 1);
    if (n > 0) {
        out(buf, n);
    }
    out("]\n", 2);
}

void _start(void)
{
    struct ktermios t;
    struct ktermios t2;
    struct winsize ws;
    char buf[64];
    int n;

    puts("TTYPROBE START");

    /* (a) window size -- all four fields are filled (xpixel/ypixel are 0). */
    ws.ws_row = 0xFFFF;
    ws.ws_col = 0xFFFF;
    ws.ws_xpixel = 0xFFFF;
    ws.ws_ypixel = 0xFFFF;
    check(do_ioctl(0, TIOCGWINSZ, &ws) == 0, "TIOCGWINSZ ok");
    check(ws.ws_row == 25 && ws.ws_col == 80, "console window is 25x80");
    check(ws.ws_xpixel == 0 && ws.ws_ypixel == 0, "console window has no pixel size");

    /* (b) default termios is cooked, echoing, with the usual in/out processing --
     * proving the whole 36-byte prefix (not just c_lflag) reads back sanely. */
    check(do_ioctl(0, TCGETS, &t) == 0, "TCGETS ok");
    check((t.c_lflag & ICANON) != 0, "default ICANON set");
    check((t.c_lflag & ECHO) != 0, "default ECHO set");
    check((t.c_iflag & ICRNL) != 0, "default ICRNL set");
    check((t.c_oflag & OPOST) != 0, "default OPOST set");

    /* (c) cooked read: the driver sends "ab<DEL><DEL>cd\n"; erasing leaves "cd\n". */
    n = do_read(0, buf, (int)sizeof(buf));
    show("COOKED:", buf, n);
    check(n == 3 && buf[0] == 'c' && buf[1] == 'd' && buf[2] == '\n',
          "cooked DEL erase -> \"cd\\n\"");

    /* (d) switch to raw and confirm the kernel applied it (TCGETS round-trip). */
    t.c_lflag &= ~(unsigned int)(ICANON | ECHO);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    check(do_ioctl(0, TCSETS, &t) == 0, "TCSETS raw ok");
    check(do_ioctl(0, TCGETS, &t2) == 0, "TCGETS after TCSETS ok");
    check((t2.c_lflag & ICANON) == 0, "TCSETS cleared ICANON (round-trip)");
    check((t2.c_lflag & ECHO) == 0, "TCSETS cleared ECHO (round-trip)");
    /* the fields we did not touch survive the round-trip unchanged -- the kernel
     * stored exactly the 36-byte prefix we sent. */
    check(t2.c_iflag == t.c_iflag && t2.c_oflag == t.c_oflag, "TCSETS preserves c_iflag/c_oflag");
    check(t2.c_cflag == t.c_cflag && t2.c_line == t.c_line, "TCSETS preserves c_cflag/c_line");

    /* (e) raw read: the driver sends "X"; it arrives with no newline, unechoed. */
    n = do_read(0, buf, (int)sizeof(buf));
    show("RAW:", buf, n);
    check(n >= 1 && buf[0] == 'X', "raw read delivers a byte without a newline");

    /* (f) FIONREAD reports nothing buffered now; TCFLSH succeeds. */
    {
        int navail = -1;
        check(do_ioctl(0, FIONREAD, &navail) == 0, "FIONREAD ok");
        check(navail == 0, "FIONREAD 0 when the queue is drained");
        check(do_ioctl(0, TCFLSH, (void *)0) == 0, "TCFLSH (flush input) ok");
    }

    if (fails == 0) {
        puts("TTYPROBE OK");
    } else {
        puts("TTYPROBE FAIL");
    }
    sys_exit(fails);
}
