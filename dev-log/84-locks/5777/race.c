/* race.c: an LD_PRELOAD library that widens and traces the window of Mesa
 * 25.0's virgl_hw_res_destroy race (Mesa commit 6c1c116a0fb6). The mutex
 * Mesa's virgl winsys holds around PRIME_FD_TO_HANDLE and GEM_CLOSE is
 * learned as the last pthread mutex the first PRIME_FD_TO_HANDLE's thread
 * locked. With
 * T5777_DELAY=1, a lock of that mutex waits LOCK_US first one time in
 * LOCK_EVERY, and an unlock of it waits UNLOCK_US after, so that a thread
 * whose decrement reached zero meets another thread's destroy, and the other
 * thread's free comes after its unlock. Every GEM_CLOSE that fails, the
 * second close of a destroyed resource, is printed to stderr.
 *
 *   gcc -shared -fPIC -O2 -o race.so race.c -ldl -lpthread
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PRIME_FD_TO_HANDLE 0xc00c642eUL
#define GEM_CLOSE          0x40086409UL
#define LOCK_US            1000
#define LOCK_EVERY         4
#define UNLOCK_US          200

static int (*real_lock)(pthread_mutex_t *);
static int (*real_unlock)(pthread_mutex_t *);
static int (*real_ioctl)(int, unsigned long, ...);
static pthread_mutex_t *volatile bo_mutex;
static __thread pthread_mutex_t *last;
static __thread unsigned seed;
static int delay = -1;
static unsigned long delayed, closes, failed;

static void spin(long us)
{
    struct timespec a, b;

    clock_gettime(CLOCK_MONOTONIC, &a);
    do
        clock_gettime(CLOCK_MONOTONIC, &b);
    while ((b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000 < us);
}

static void init(void)
{
    if (real_lock)
        return;
    real_lock = (int (*)(pthread_mutex_t *))dlsym(RTLD_NEXT, "pthread_mutex_lock");
    real_unlock = (int (*)(pthread_mutex_t *))dlsym(RTLD_NEXT, "pthread_mutex_unlock");
    real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
}

static int delaying(void)
{
    if (delay < 0) {
        const char *s = getenv("T5777_DELAY");
        delay = s != NULL && strcmp(s, "1") == 0;
    }
    return delay;
}

int pthread_mutex_lock(pthread_mutex_t *m)
{
    int r;

    init();
    if (m == bo_mutex && delaying()) {
        if (seed == 0)
            seed = (unsigned)(size_t)&seed ^ (unsigned)getpid();
        if (rand_r(&seed) % LOCK_EVERY == 0) {
            __atomic_add_fetch(&delayed, 1, __ATOMIC_RELAXED);
            spin(LOCK_US);
        }
    }
    r = real_lock(m);
    last = m;
    return r;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    int r;

    init();
    r = real_unlock(m);
    if (m == bo_mutex && delaying())
        spin(UNLOCK_US);
    return r;
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *arg;
    int r, e;

    init();
    va_start(ap, request);
    arg = va_arg(ap, void *);
    va_end(ap);
    if (request == PRIME_FD_TO_HANDLE && bo_mutex == NULL && last)
        bo_mutex = last;
    r = real_ioctl(fd, request, arg);
    if (request == GEM_CLOSE) {
        e = errno;
        __atomic_add_fetch(&closes, 1, __ATOMIC_RELAXED);
        if (r != 0) {
            char line[96];
            int n = snprintf(line, sizeof line, "t5777: GEM_CLOSE %u failed: %s\n",
                             *(unsigned *)arg, strerror(e));
            __atomic_add_fetch(&failed, 1, __ATOMIC_RELAXED);
            if (write(2, line, (size_t)n) < 0)
                ;
        }
        errno = e;
    }
    return r;
}

__attribute__((destructor)) static void report(void)
{
    char line[128];
    int n = snprintf(line, sizeof line, "t5777: closes %lu failed %lu delayed locks %lu\n",
                     closes, failed, delayed);

    if (write(2, line, (size_t)n) < 0)
        ;
}
