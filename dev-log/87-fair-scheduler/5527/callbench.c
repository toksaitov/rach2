/* callbench.c: the cost of the calls a browser's video threads make most,
 * timed on the monotonic clock, for 5527's comparison of Rach2 and Linux:
 * a plain call, a futex wake with no waiter, a futex round trip between two
 * threads on two processors, a poll of pipes, a zero-timeout poll of a
 * socket pair, sched_yield, and a page fault. Build static with any C99
 * compiler: cc -O2 -static -pthread -o callbench callbench.c */
#define _GNU_SOURCE
#include <linux/futex.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static long futex(volatile int *w, int op, int v)
{
    return syscall(SYS_futex, w, op, v, NULL, NULL, 0);
}

static volatile int turn;
static int rounds;

/* The partner: wait for its turn (1), give it back (0), and wake. */
static void *partner(void *arg)
{
    int i;
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(2, &set);
    sched_setaffinity(0, sizeof set, &set);
    (void)arg;
    for (i = 0; i < rounds; i++) {
        while (turn != 1) {
            futex(&turn, FUTEX_WAIT_PRIVATE, 0);
        }
        turn = 0;
        futex(&turn, FUTEX_WAKE_PRIVATE, 1);
    }
    return NULL;
}

static void report(const char *what, int n, double t)
{
    printf("callbench: %-22s %8d in %7.3f s, %8.2f us each\n", what, n, t, t * 1e6 / n);
}

int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 200000, i, p[16][2], sv[2];
    double t;
    volatile int word = 0;
    pthread_t th;
    struct pollfd fds[16];
    cpu_set_t set;
    char *m;
    size_t len = (size_t)256 << 20;

    CPU_ZERO(&set);
    CPU_SET(0, &set);
    sched_setaffinity(0, sizeof set, &set);

    t = now();
    for (i = 0; i < n; i++) {
        syscall(SYS_getppid);
    }
    report("getppid", n, now() - t);

    t = now();
    for (i = 0; i < n; i++) {
        futex(&word, FUTEX_WAKE_PRIVATE, 1);
    }
    report("futex wake, no waiter", n, now() - t);

    rounds = n / 4;
    turn = 0;
    pthread_create(&th, NULL, partner, NULL);
    t = now();
    for (i = 0; i < rounds; i++) {
        turn = 1;
        futex(&turn, FUTEX_WAKE_PRIVATE, 1);
        while (turn != 0) {
            futex(&turn, FUTEX_WAIT_PRIVATE, 1);
        }
    }
    report("futex round trip", rounds, now() - t);
    pthread_join(th, NULL);

    for (i = 0; i < 16; i++) {
        if (pipe(p[i]) != 0) {
            return 1;
        }
        fds[i].fd = p[i][0];
        fds[i].events = POLLIN;
    }
    t = now();
    for (i = 0; i < n; i++) {
        poll(fds, 16, 0);
    }
    report("poll 16 pipes", n, now() - t);

    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    fds[0].fd = sv[0];
    t = now();
    for (i = 0; i < n; i++) {
        poll(fds, 1, 0);
    }
    report("poll unix socket", n, now() - t);

    t = now();
    for (i = 0; i < n; i++) {
        sched_yield();
    }
    report("sched_yield", n, now() - t);

    m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    t = now();
    for (i = 0; i < (int)(len / 4096); i++) {
        m[(size_t)i * 4096] = 1;
    }
    report("page fault, anonymous", (int)(len / 4096), now() - t);
    munmap(m, len);
    return 0;
}
