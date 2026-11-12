/* load.c: small loads, each one kind of kernel work, run alike on Rach2 and
 * Linux so that the VM exits each costs can be compared.
 * Usage: load MODE N */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static long n;
static volatile int word;
static volatile int stop;

static void fwait(volatile int *w, int v)
{
    syscall(SYS_futex, w, FUTEX_WAIT_PRIVATE, v, NULL, NULL, 0);
}

static void fwake(volatile int *w)
{
    syscall(SYS_futex, w, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

static void *pong(void *a)
{
    long i;

    (void)a;
    for (i = 0; i < n; i++) {
        while (word != 1) {
            fwait(&word, 0);
        }
        word = 0;
        fwake(&word);
    }
    return NULL;
}

static void *spin(void *a)
{
    volatile unsigned long x = 0;

    (void)a;
    while (!stop) {
        x++;
    }
    return NULL;
}

static void *sleeper(void *a)
{
    (void)a;
    while (!stop) {
        usleep(100000);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t t[8];
    long i;
    int k;

    if (argc < 3) {
        fprintf(stderr, "usage: load MODE N\n");
        return 2;
    }
    n = atol(argv[2]);
    if (strcmp(argv[1], "sleep") == 0) { /* N sleeps of 1 ms */
        struct timespec ts = {0, 1000000};
        for (i = 0; i < n; i++) {
            clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
        }
    } else if (strcmp(argv[1], "pingpong") == 0) { /* N hand-offs */
        pthread_create(&t[0], NULL, pong, NULL);
        for (i = 0; i < n; i++) {
            word = 1;
            fwake(&word);
            while (word != 0) {
                fwait(&word, 1);
            }
        }
        pthread_join(t[0], NULL);
    } else if (strcmp(argv[1], "spin") == 0) { /* 4 threads, N ms */
        for (k = 0; k < 4; k++) {
            pthread_create(&t[k], NULL, spin, NULL);
        }
        usleep(n * 1000);
        stop = 1;
        for (k = 0; k < 4; k++) {
            pthread_join(t[k], NULL);
        }
    } else if (strcmp(argv[1], "munmap") == 0) { /* N maps of 64 KiB, 3 threads */
        for (k = 0; k < 3; k++) {
            pthread_create(&t[k], NULL, sleeper, NULL);
        }
        for (i = 0; i < n; i++) {
            char *p = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            int j;
            for (j = 0; j < 65536; j += 4096) {
                p[j] = 1;
            }
            munmap(p, 65536);
        }
        stop = 1;
        for (k = 0; k < 3; k++) {
            pthread_join(t[k], NULL);
        }
    } else if (strcmp(argv[1], "munmapspin") == 0) { /* the same, 3 busy threads */
        for (k = 0; k < 3; k++) {
            pthread_create(&t[k], NULL, spin, NULL);
        }
        for (i = 0; i < n; i++) {
            char *p = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            int j;
            for (j = 0; j < 65536; j += 4096) {
                p[j] = 1;
            }
            munmap(p, 65536);
        }
        stop = 1;
        for (k = 0; k < 3; k++) {
            pthread_join(t[k], NULL);
        }
    } else if (strcmp(argv[1], "yield") == 0) { /* N getrusage and yields */
        struct rusage ru;
        for (i = 0; i < n; i++) {
            getrusage(RUSAGE_THREAD, &ru);
            sched_yield();
        }
    } else if (strcmp(argv[1], "syscall") == 0) { /* N getppid */
        for (i = 0; i < n; i++) {
            syscall(SYS_getppid);
        }
    } else if (strcmp(argv[1], "fault") == 0) { /* N MiB touched and freed */
        for (i = 0; i < n; i++) {
            char *p = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            int j;
            for (j = 0; j < (1 << 20); j += 4096) {
                p[j] = 1;
            }
            munmap(p, 1 << 20);
        }
    } else {
        fprintf(stderr, "load: unknown mode %s\n", argv[1]);
        return 2;
    }
    return 0;
}
