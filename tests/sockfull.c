/**
 * Fills the machine's socket pool, which one process cannot do alone.
 *
 * The pool holds sixteen sockets and a process holds sixteen descriptors, of
 * which three are already stdio: one process runs out of descriptors at
 * thirteen and never reaches the pool's own limit. So this forks. The parent
 * fills its table, the child inherits those descriptors (the sockets they name
 * stay taken while the parent holds them), closes three of its own to make
 * room, and opens three more. The pool is full then, and the next open is the
 * one under test.
 *
 * The refusal has to be `ENOMEM`, which is what `do_socket` returns when
 * `socket_alloc` finds no free entry, and a socket the child closes has to
 * make room at once. A pool that leaked an entry per close would pass a test
 * that only opened once.
 *
 * Prints "SOCKFULL OK" and exits 0. Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT       1
#define SYS_FORK       2
#define SYS_WRITE      4
#define SYS_CLOSE      6
#define SYS_WAITPID    7
#define SYS_SOCKETCALL 102

#define SC_SOCKET 1

#define AF_INET    2
#define SOCK_DGRAM 2

#define ENOMEM 12
#define EMFILE 24

#define TRIES 32

#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

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

static int sys_fork(void)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_FORK));
    return r;
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

/* A datagram socket, which needs no peer and no address. */
static int make_socket(void)
{
    unsigned int args[3];

    args[0] = AF_INET;
    args[1] = SOCK_DGRAM;
    args[2] = 0;
    return syscall3(SYS_SOCKETCALL, SC_SOCKET, (int)(size_t)args, 0);
}

static int fds[TRIES];
static int n_fds;

/* The child's exit statuses, so the parent can say which step failed. */
#define CHILD_OK          0
#define CHILD_NO_ROOM     2
#define CHILD_NOT_REFUSED 3
#define CHILD_WRONG_ERRNO 4
#define CHILD_NO_REUSE    5

static void child(void)
{
    int i, s, refused;
    int mine[8];
    int n_mine = 0;

    /* Three of the inherited descriptors, so this process has table room. The
       sockets they named stay in the pool: the parent still holds them. */
    for (i = 0; i < 3; i++) {
        if (syscall3(SYS_CLOSE, fds[i], 0, 0) != 0) {
            sys_exit(CHILD_NO_ROOM);
        }
    }

    /* Fill what the parent's thirteen left of the pool. */
    refused = 0;
    for (i = 0; i < 8; i++) {
        s = make_socket();
        if (s < 0) {
            refused = s;
            break;
        }
        mine[n_mine++] = s;
    }
    if (refused == 0) {
        sys_exit(CHILD_NOT_REFUSED);
    }
    if (refused != -ENOMEM) {
        sys_exit(CHILD_WRONG_ERRNO);
    }

    /* One back makes room for one more. */
    if (n_mine == 0 || syscall3(SYS_CLOSE, mine[n_mine - 1], 0, 0) != 0) {
        sys_exit(CHILD_NO_REUSE);
    }
    s = make_socket();
    if (s < 0) {
        sys_exit(CHILD_NO_REUSE);
    }
    sys_exit(CHILD_OK);
}

void _start(void)
{
    int i, s, pid, status, refused;

    /* The parent's own table first: it stops at EMFILE, not at the pool. */
    refused = 0;
    for (i = 0; i < TRIES; i++) {
        s = make_socket();
        if (s < 0) {
            refused = s;
            break;
        }
        fds[n_fds++] = s;
    }
    if (refused != -EMFILE) {
        print("SOCKFULL FAIL: the parent's table refused with ");
        print_num(refused);
        puts(", not -EMFILE");
        sys_exit(1);
    }
    if (n_fds < 8) {
        puts("SOCKFULL FAIL: the parent holds too few sockets to fill the pool");
        sys_exit(1);
    }

    pid = sys_fork();
    if (pid == 0) {
        child();
    }
    if (pid < 0) {
        puts("SOCKFULL FAIL: the second process could not be started");
        sys_exit(1);
    }
    if (syscall3(SYS_WAITPID, pid, (int)(size_t)&status, 0) != pid) {
        puts("SOCKFULL FAIL: the second process could not be reaped");
        sys_exit(1);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != CHILD_OK) {
        print("SOCKFULL FAIL: the second process reported ");
        print_num((int)WEXITSTATUS(status));
        puts(" (2 no room, 3 never refused, 4 wrong errno, 5 no reuse)");
        sys_exit(1);
    }

    print("SOCKFULL OK (");
    print_num(n_fds);
    puts(" in one process, the pool refused with ENOMEM, a closed one reused)");
    sys_exit(0);
}
