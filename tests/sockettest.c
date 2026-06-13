/*
 * sockettest.c - Socket lifecycle test program for Rach2 OS
 *
 * Verifies that sockets are first-class file descriptors with a correct
 * reference count: close releases them, dup2 shares them, fork shares them,
 * and process exit releases any still open. A leak or double-free would show
 * up as the socket pool exhausting or the kernel faulting.
 *
 * Compile with:
 *   i686-elf-gcc -m32 -nostdlib -static -ffreestanding -Wl,-Ttext=0x08048000 -o sockettest sockettest.c
 */

typedef unsigned int   size_t;
typedef unsigned int   uint32_t;

#define STDOUT_FILENO 1

#define SYS_EXIT       1
#define SYS_FORK       2
#define SYS_WRITE      4
#define SYS_CLOSE      6
#define SYS_WAITPID    7
#define SYS_DUP2      63
#define SYS_SOCKETCALL 102

#define SC_SOCKET    1
#define AF_INET      2
#define SOCK_DGRAM   2

static int sys_write(int fd, const void *buf, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
        : "=a"(r) : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count) : "memory");
    return r;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
    __builtin_unreachable();
}

static int sys_close(int fd)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_CLOSE), "b"(fd));
    return r;
}

static int sys_fork(void)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_FORK) : "memory");
    return r;
}

static int sys_waitpid(int pid, int *status, int options)
{
    int r;
    __asm__ __volatile__("int $0x80"
        : "=a"(r) : "0"(SYS_WAITPID), "b"(pid), "c"(status), "d"(options)
        : "memory");
    return r;
}

static int sys_dup2(int oldfd, int newfd)
{
    int r;
    __asm__ __volatile__("int $0x80"
        : "=a"(r) : "0"(SYS_DUP2), "b"(oldfd), "c"(newfd) : "memory");
    return r;
}

static int socketcall(int call, uint32_t *args)
{
    int r;
    __asm__ __volatile__("int $0x80"
        : "=a"(r) : "0"(SYS_SOCKETCALL), "b"(call), "c"(args) : "memory");
    return r;
}

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static void print(const char *s)  { sys_write(STDOUT_FILENO, s, strlen(s)); }
static void puts(const char *s)   { print(s); sys_write(STDOUT_FILENO, "\n", 1); }

static void print_num(int n)
{
    char buf[12];
    int i = 0, neg = 0;
    if (n < 0) { neg = 1; n = -n; }
    if (n == 0) buf[i++] = '0';
    while (n > 0) { buf[i++] = (char)('0' + n % 10); n /= 10; }
    if (neg) sys_write(STDOUT_FILENO, "-", 1);
    while (i > 0) sys_write(STDOUT_FILENO, &buf[--i], 1);
}

static int make_socket(void)
{
    uint32_t args[3];
    args[0] = AF_INET; args[1] = SOCK_DGRAM; args[2] = 0;
    return socketcall(SC_SOCKET, args);
}

void _start(void)
{
    int passed = 0, failed = 0;
    int i, fd, fd2, pid, status;

    puts("Socket lifecycle test starting...");
    puts("");

    /* Test 1: create and close many times; the pool must not leak. */
    print("Test 1: 30x create+close... ");
    {
        int ok = 1;
        for (i = 0; i < 30; i++) {
            fd = make_socket();
            if (fd < 0) { ok = 0; break; }
            sys_close(fd);
        }
        if (ok) { puts("OK"); passed++; }
        else { print("FAILED (exhausted at "); print_num(i); puts(")"); failed++; }
    }

    /* Test 2: dup2 shares the socket; closing both releases it once. */
    print("Test 2: dup2 share + close... ");
    {
        int ok = 1;
        fd = make_socket();
        if (fd < 0) ok = 0;
        if (ok && sys_dup2(fd, 10) != 10) ok = 0;
        if (ok) {
            sys_close(fd);    /* socket stays alive via the dup */
            sys_close(10);    /* last reference, released here */
            /* If the refcount were wrong the pool would now be short. */
            for (i = 0; i < 20; i++) {
                fd2 = make_socket();
                if (fd2 < 0) { ok = 0; break; }
                sys_close(fd2);
            }
        }
        if (ok) { puts("OK"); passed++; }
        else { puts("FAILED"); failed++; }
    }

    /* Test 3: fork shares the socket; parent and child each close it. */
    print("Test 3: fork share + close... ");
    {
        int ok = 1;
        fd = make_socket();
        if (fd < 0) ok = 0;
        if (ok) {
            pid = sys_fork();
            if (pid == 0) {
                sys_close(fd);
                sys_exit(0);
            }
            sys_waitpid(pid, &status, 0);
            sys_close(fd);    /* must not double-free the child's release */
            /* Parent still healthy: pool intact. */
            fd2 = make_socket();
            if (fd2 < 0) ok = 0;
            else sys_close(fd2);
        }
        if (ok) { puts("OK"); passed++; }
        else { puts("FAILED"); failed++; }
    }

    /* Test 4: a child that exits without closing must not leak its socket. */
    print("Test 4: 30x exit-without-close... ");
    {
        int ok = 1;
        for (i = 0; i < 30; i++) {
            pid = sys_fork();
            if (pid == 0) {
                int s = make_socket();
                sys_exit(s < 0 ? 1 : 0);   /* deliberately no close */
            }
            sys_waitpid(pid, &status, 0);
            if (status != 0) { ok = 0; break; }
        }
        if (ok) { puts("OK"); passed++; }
        else { print("FAILED (leak at "); print_num(i); puts(")"); failed++; }
    }

    puts("");
    print("Results: "); print_num(passed); print(" passed, ");
    print_num(failed); puts(" failed");

    if (failed == 0) {
        puts("All socket tests passed!");
        sys_exit(0);
    }
    puts("Some tests failed!");
    sys_exit(1);
}
