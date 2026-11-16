/*
 * stall.c: the backend's report of a stall, written to the VM's console:
 * each thread's state, kernel stack and frames, and the VM's kernel log,
 * when the serving thread stops turning or its fences stop retiring.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <time.h>
#include <unistd.h>

#include "backend.h"

/* The frames a thread prints, and the kernel log's lines the report keeps. */
#define FRAMES    48
#define LOG_LINES 60

/* The serving thread's last turn and where it was, which stall_beat sets
 * and the watching thread reads. */
static const struct vd *dev;
static const char *volatile where = "start";
static volatile long beat;

/* The thread whose frames are asked for, and whether it has printed them. */
static volatile pid_t want;
static volatile sig_atomic_t done;

/** Return CLOCK_MONOTONIC's seconds. */
static long now_s(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec;
}

/** Print the frames of the thread the report asked, from its own stack. */
static void on_frames(int sig)
{
    void *pc[FRAMES];
    int n;

    (void)sig;
    if (gettid() != want) {
        return;
    }
    n = backtrace(pc, FRAMES);
    backtrace_symbols_fd(pc, n, STDERR_FILENO);
    done = 1;
}

/** Read a small file of /proc into buf, without its last newline. */
static void read_line(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    size_t n = f ? fread(buf, 1, size - 1, f) : 0;

    if (f) {
        fclose(f);
    }
    buf[n] = 0;
    if (n && buf[n - 1] == '\n') {
        buf[n - 1] = 0;
    }
}

/**
 * Print one thread: its name, state and wait channel, its kernel stack,
 * which /proc gives root, and its frames, which it prints itself from a
 * signal's handler. A thread that blocks signals, as Mesa's do, or sleeps
 * uninterruptibly in the kernel prints none within the second it is given.
 */
static void thread(const char *tid)
{
    char path[64], comm[64], stat[512], wchan[128], line[256];
    const char *state;
    FILE *f;
    int i;

    snprintf(path, sizeof(path), "/proc/self/task/%s/comm", tid);
    read_line(path, comm, sizeof(comm));
    snprintf(path, sizeof(path), "/proc/self/task/%s/stat", tid);
    read_line(path, stat, sizeof(stat));
    snprintf(path, sizeof(path), "/proc/self/task/%s/wchan", tid);
    read_line(path, wchan, sizeof(wchan));
    state = strrchr(stat, ')');
    fprintf(stderr, "backend: stall: thread %s (%s): state %c, wchan %s\n", tid, comm,
            state && state[1] ? state[2] : '?', wchan[0] ? wchan : "0");
    snprintf(path, sizeof(path), "/proc/self/task/%s/stack", tid);
    if ((f = fopen(path, "r")) != 0) {
        while (fgets(line, sizeof(line), f)) {
            fprintf(stderr, "backend: stall: thread %s: kernel %s", tid, line);
        }
        fclose(f);
    }
    if (atoi(tid) == gettid()) {
        return;
    }
    fprintf(stderr, "backend: stall: thread %s: frames:\n", tid);
    done = 0;
    want = (pid_t)atoi(tid);
    if (tgkill(getpid(), want, SIGRTMIN) == 0) {
        for (i = 0; i < 100 && !done; i++) {
            nanosleep(&(struct timespec){0, 10000000}, 0);
        }
    }
    if (!done) {
        fprintf(stderr, "backend: stall: thread %s: no frames: signals blocked or asleep\n", tid);
    }
    want = 0;
}

/** Print the last LOG_LINES lines of the VM's kernel log. */
static void kernel_log(void)
{
    int size = klogctl(10, 0, 0), n, lines = 0;
    char *buf, *p, *nl;

    if (size <= 0 || (buf = malloc((size_t)size + 1)) == 0) {
        return;
    }
    if ((n = klogctl(3, buf, size)) < 0) {
        fprintf(stderr, "backend: stall: no kernel log: %s\n", strerror(errno));
        free(buf);
        return;
    }
    buf[n] = 0;
    for (p = buf + n; p > buf; p--) {
        if (p[-1] == '\n' && p < buf + n && ++lines >= LOG_LINES) {
            break;
        }
    }
    for (; *p; p = nl + 1) {
        if ((nl = strchr(p, '\n')) == 0) {
            fprintf(stderr, "backend: stall: kernel: %s\n", p);
            break;
        }
        fprintf(stderr, "backend: stall: kernel: %.*s\n", (int)(nl - p), p);
    }
    free(buf);
}

/**
 * Print the stall report: why, each thread of the backend, and the VM's
 * kernel log, whose i915 hangs and resets the console's loglevel hides.
 * Any thread may call it; the serving thread and the watching thread do.
 */
void stall_report(const char *why)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    struct dirent *e;
    DIR *dir;

    pthread_mutex_lock(&lock);
    fprintf(stderr, "backend: stall: %s\n", why);
    if ((dir = opendir("/proc/self/task")) != 0) {
        while ((e = readdir(dir)) != 0) {
            if (e->d_name[0] != '.') {
                thread(e->d_name);
            }
        }
        closedir(dir);
    }
    kernel_log();
    fprintf(stderr, "backend: stall: end\n");
    pthread_mutex_unlock(&lock);
}

/** Note that the serving thread turns at `at`, a static string. */
void stall_beat(const char *at)
{
    where = at;
    beat = now_s();
}

/**
 * Watch the serving thread: when it has not turned for STALL_S seconds,
 * report once, and again every STALL_AGAIN_S while it stays still.
 */
static void *watch(void *arg)
{
    long seen = -1, next = 0;
    char why[160];

    (void)arg;
    for (;;) {
        long b = beat, t = now_s();

        sleep(1);
        if (b != seen) {
            seen = b;
            next = b + STALL_S;
            continue;
        }
        if (t < next) {
            continue;
        }
        if (strcmp(where, "step") == 0 && dev->cmd) {
            struct vd_hdr h;

            memcpy(&h, dev->cmd, sizeof(h));
            snprintf(why, sizeof(why), "the serving thread has been in %s for %ld s, command 0x%x",
                     where, t - b, (unsigned)h.type);
        } else {
            snprintf(why, sizeof(why), "the serving thread has been in %s for %ld s", where, t - b);
        }
        stall_report(why);
        next = t + STALL_AGAIN_S;
    }
    return 0;
}

/**
 * Start the watching thread over d, whose command the report names. Its
 * frames' handler is set first, and backtrace is called once, since its
 * first call loads libgcc_s, which a signal's handler may not do.
 */
void stall_start(const struct vd *d)
{
    struct sigaction sa;
    pthread_t t;
    sigset_t all, old;
    void *pc[1];

    dev = d;
    beat = now_s();
    backtrace(pc, 1);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_frames;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGRTMIN, &sa, 0);
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, &old);
    if (pthread_create(&t, 0, watch, 0) == 0) {
        pthread_detach(t);
    } else {
        fprintf(stderr, "backend: stall: no watching thread\n");
    }
    pthread_sigmask(SIG_SETMASK, &old, 0);
}
