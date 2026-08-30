/**
 * Fills the process table, then proves a reaped slot is reusable.
 *
 * The kernel has sixteen process slots and no way to ask how many are free, so
 * a program that forks in a loop meets the limit rather than being told it.
 * What matters is what happens there: the refusal has to be `EAGAIN`, the
 * count has to be a number the machine could have (the table's size, less
 * whatever was already running), and a slot given back by a reaped child has
 * to be usable again. A kernel that leaked a slot per fork would pass a test
 * that only forked once.
 *
 * The children do nothing but exit, so no slot is held longer than it must be.
 * The parent reaps them all at the end, whatever happened, because a probe
 * that leaves zombies behind changes what the next probe in the same boot can
 * do.
 *
 * Prints "PROCFULL OK" and exits 0. Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT    1
#define SYS_FORK    2
#define SYS_WRITE   4
#define SYS_WAITPID 7

#define EAGAIN 11

/* More than the table can hold, so the loop below always reaches the
   refusal. */
#define TRIES 32

static int sys_write(int fd, const void *buf, size_t count)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count)
                         : "memory");
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

static int sys_waitpid(int pid, int *status, int options)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_WAITPID), "b"(pid), "c"(status), "d"(options)
                         : "memory");
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
    sys_write(STDOUT_FILENO, s, strlen(s));
}

static void puts(const char *s)
{
    print(s);
    sys_write(STDOUT_FILENO, "\n", 1);
}

static void print_num(int v)
{
    char buf[12];
    int i = 0;

    if (v < 0) {
        sys_write(STDOUT_FILENO, "-", 1);
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
        sys_write(STDOUT_FILENO, &buf[--i], 1);
    }
}

static int kids[TRIES];
static int n_kids;

/* Returns the number that could not be reaped, which the caller reports: a
   probe that left a zombie behind would take a slot from the next probe in
   the same boot. */
static int reap_all(void)
{
    int i, status, missed = 0;

    for (i = 0; i < n_kids; i++) {
        if (sys_waitpid(kids[i], &status, 0) != kids[i]) {
            missed++;
        }
    }
    n_kids = 0;
    return missed;
}

static void fail(const char *why)
{
    (void)reap_all();
    print("PROCFULL FAIL: ");
    puts(why);
    sys_exit(1);
}

void _start(void)
{
    int pid, status, i, refused, slots;

    /* Fork until the table refuses. Each child exits at once; the parent does
       not reap yet, so the slots stay taken and the limit is reached. */
    refused = 0;
    for (i = 0; i < TRIES; i++) {
        pid = sys_fork();
        if (pid == 0) {
            sys_exit(0);
        }
        if (pid < 0) {
            refused = pid;
            break;
        }
        kids[n_kids++] = pid;
    }

    if (refused == 0) {
        fail("the table never refused a fork");
    }
    if (refused != -EAGAIN) {
        print("PROCFULL FAIL: the refusal was ");
        print_num(refused);
        puts(", not -EAGAIN");
        (void)reap_all();
        sys_exit(1);
    }
    /* The table holds sixteen and two of them are taken: the debug shell and
       this probe. A range would pass a table of another size, which is the
       one number this probe exists to pin. */
    slots = n_kids;
    if (slots != 14) {
        print("PROCFULL FAIL: forked ");
        print_num(slots);
        puts(" children, not the 14 a 16-slot table leaves");
        (void)reap_all();
        sys_exit(1);
    }

    /* A reaped child gives its slot back, so the next fork must succeed. This
       is what a kernel that leaked a slot per exit would fail. */
    if (sys_waitpid(kids[0], &status, 0) != kids[0]) {
        fail("a child could not be reaped");
    }
    pid = sys_fork();
    if (pid == 0) {
        sys_exit(0);
    }
    if (pid < 0) {
        fail("a reaped slot was not reusable");
    }
    kids[0] = pid;

    if (reap_all() != 0) {
        puts("PROCFULL FAIL: a child could not be reaped, so a slot is still taken");
        sys_exit(1);
    }
    print("PROCFULL OK (");
    print_num(slots);
    puts(" slots, refused with EAGAIN, a reaped slot reused)");
    sys_exit(0);
}
