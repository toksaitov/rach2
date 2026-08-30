/**
 * Fills the TCP control-block pool over the machine's own loopback.
 *
 * The pool holds sixteen control blocks and a connection to this machine's own
 * listener takes two: one for the side that connected and one for the side
 * that was connected to. So a listener and seven connections hold fifteen, and
 * the eighth connection is the one that cannot be made. That is reached with
 * nine sockets, which one process has descriptors for, so unlike the socket
 * pool this needs no second process.
 *
 * A connection to `127.0.0.1` never reaches the wire: the socket layer
 * rewrites 127/8 to this machine's one address and the datagram goes on the
 * loopback ring for the timer tick to deliver. The listener is bound to no
 * address, which is what a server does.
 *
 * The refusal comes from the other side of the connection rather than from
 * this one: the connecting side takes the block it needs, and the side being
 * connected to finds none left and answers with a reset, which reaches the
 * caller as `ECONNREFUSED`. A machine that dropped that SYN instead would
 * leave the caller retransmitting for six seconds and then reporting a
 * timeout, which is what a machine that is not there looks like.
 *
 * What this does not check is a block coming back: a closed connection holds
 * its two through the teardown and TIME_WAIT, which is the protocol and not a
 * leak, and a probe that waited that out would spend a minute doing it.
 * `tests/host/tcp_hosttest.c` checks a freed block being handed out again,
 * directly and in no time at all.
 *
 * Prints "TCBFULL OK" and exits 0. Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;
typedef unsigned short uint16_t;

#define STDOUT_FILENO 1

#define SYS_EXIT       1
#define SYS_WRITE      4
#define SYS_CLOSE      6
#define SYS_SOCKETCALL 102

#define SC_SOCKET  1
#define SC_BIND    2
#define SC_CONNECT 3
#define SC_LISTEN  4

#define AF_INET     2
#define SOCK_STREAM 1

#define ECONNREFUSED 111

/* The port this machine talks to itself on, and more attempts than the pool
   can serve, so the loop always reaches the refusal. */
#define PORT  7350
#define TRIES 12

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
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

static void print(const char *s)
{
    syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)s, (int)strlen(s));
}

static void puts(const char *s)
{
    print(s);
    syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)"\n", 1);
}

static void print_num(int v)
{
    char buf[12];
    int i = 0;

    if (v < 0) {
        print("-");
        v = -v;
    }
    if (v == 0) {
        buf[i++] = '0';
    }
    while (v > 0) {
        buf[i++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (i > 0) {
        syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)&buf[--i], 1);
    }
}

/* A sockaddr_in built by hand: the port in network order, and either no
   address at all (what a listener binds) or the loopback one. */
static void make_sockaddr(unsigned char *sa, uint16_t port, int loopback)
{
    int i;

    for (i = 0; i < 16; i++) {
        sa[i] = 0;
    }
    sa[0] = AF_INET;
    sa[2] = (unsigned char)(port >> 8);
    sa[3] = (unsigned char)(port & 0xFF);
    if (loopback) {
        sa[4] = 127;
        sa[7] = 1;
    }
}

static int make_stream(void)
{
    unsigned int args[3];

    args[0] = AF_INET;
    args[1] = SOCK_STREAM;
    args[2] = 0;
    return syscall3(SYS_SOCKETCALL, SC_SOCKET, (int)(size_t)args, 0);
}

static int conns[TRIES];
static int n_conns;

static void close_all(int listener)
{
    int i;

    for (i = 0; i < n_conns; i++) {
        syscall3(SYS_CLOSE, conns[i], 0, 0);
    }
    n_conns = 0;
    if (listener >= 0) {
        syscall3(SYS_CLOSE, listener, 0, 0);
    }
}

void _start(void)
{
    unsigned char any[16], loop[16];
    unsigned int args[3];
    int listener, fd, i, r, refused, made;

    listener = make_stream();
    if (listener < 0) {
        puts("TCBFULL FAIL: no socket for the listener");
        sys_exit(1);
    }
    make_sockaddr(any, PORT, 0);
    args[0] = (unsigned int)listener;
    args[1] = (unsigned int)(size_t)any;
    args[2] = 16;
    if (syscall3(SYS_SOCKETCALL, SC_BIND, (int)(size_t)args, 0) != 0) {
        puts("TCBFULL FAIL: the listener could not bind");
        sys_exit(1);
    }
    args[0] = (unsigned int)listener;
    args[1] = 8;
    if (syscall3(SYS_SOCKETCALL, SC_LISTEN, (int)(size_t)args, 0) != 0) {
        puts("TCBFULL FAIL: the listener could not listen");
        sys_exit(1);
    }

    make_sockaddr(loop, PORT, 1);
    refused = 0;
    for (i = 0; i < TRIES; i++) {
        fd = make_stream();
        if (fd < 0) {
            close_all(listener);
            print("TCBFULL FAIL: out of sockets before control blocks (");
            print_num(fd);
            puts(")");
            sys_exit(1);
        }
        args[0] = (unsigned int)fd;
        args[1] = (unsigned int)(size_t)loop;
        args[2] = 16;
        r = syscall3(SYS_SOCKETCALL, SC_CONNECT, (int)(size_t)args, 0);
        if (r != 0) {
            refused = r;
            syscall3(SYS_CLOSE, fd, 0, 0);
            break;
        }
        conns[n_conns++] = fd;
    }

    if (refused == 0) {
        close_all(listener);
        puts("TCBFULL FAIL: the pool never refused a connection");
        sys_exit(1);
    }
    if (refused != -ECONNREFUSED) {
        print("TCBFULL FAIL: the refusal was ");
        print_num(refused);
        puts(", not -ECONNREFUSED");
        close_all(listener);
        sys_exit(1);
    }
    /* Seven: a listener and seven connections are fifteen of the sixteen
       blocks, and the eighth connection needs two. A range would pass a pool
       of another size, which is the number this probe exists to pin. */
    made = n_conns;
    if (made != 7) {
        print("TCBFULL FAIL: made ");
        print_num(made);
        puts(" connections, not the 7 a 16-block pool leaves");
        close_all(listener);
        sys_exit(1);
    }

    close_all(listener);
    print("TCBFULL OK (");
    print_num(made);
    puts(" connections, and the eighth refused rather than ignored)");
    sys_exit(0);
}
