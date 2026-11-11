/*
 * lockstressmusl: race the paths that run without the big lock, mode by
 * mode, and check what each returns; with -u, check that debugfs's
 * bkl/calls counts no hold of the big lock for the calls named.
 *
 * Usage: lockstressmusl [-n SCALE] [-t SECONDS] [-m MODE[,MODE]...]
 *                       [-C DIR] [-u CALL]...
 *
 * Every mode runs once, in the order below, or those -m names. -n
 * multiplies each mode's rounds (1 by default), and -t runs the modes again
 * until SECONDS have passed, at least once each. -C names the directory of
 * the share and stat modes' files, "." by default; Rach2 maps a file shared
 * and writable on tmpfs and ext2 alone, so its case gives /dev/shm. The
 * threads are pinned to the processors online in turn, so that on more than
 * one each race crosses processors. The modes, each an assertion:
 *
 *   switch   pairs of threads on two processors pass a count through a
 *            futex word and through a pipe, each seeing every value in
 *            order;
 *   poll     a reader polls 64 pipes, then waits on them in an epoll set,
 *            while writers on the other processors write them; every byte
 *            arrives once, from the pipe it was written to, and a pipe that
 *            poll or epoll gives as readable reads at least a byte;
 *   close    a thread blocked in a read of a pipe, or in a write of 256 KiB
 *            to a full one, has its descriptor closed, or another file
 *            put in its place by dup2, from another processor; the call
 *            ends as Linux's does, since it holds the file by its own
 *            reference (fdget_pos, fs/read_write.c:703), which close and
 *            dup2 leave (fs/open.c:1558-1583, fs/file.c:1205-1248): the
 *            read gets the byte of the file it began on and the write
 *            writes all of it, a call that began after fails with EBADF or
 *            uses the new file, and a write to a pipe left with no reader
 *            is EPIPE (fs/pipe.c:457-461);
 *   unmap    two processes at once unmap and map again a region that their
 *            threads on the other processors read; a read gives a value of
 *            the mapping it read or a later one, never an earlier one or
 *            another process's, or faults in the gap;
 *   fault    four threads write and read their own word of one page while
 *            a fifth maps the page afresh, protects it, opens it again and
 *            unmaps it, and a child process maps and touches half the
 *            memory available; a thread reads back what it wrote or the
 *            new page's 0, or faults, and no word changes while the page
 *            is read-only;
 *   share    two processes read a shared mapping of a file of 16 pages,
 *            one writing it too, while others write the file, truncate it
 *            and grow it again, and map and unmap it; every byte is 0 or a
 *            letter written, and at the end the mapping and the file agree
 *            and a page truncated away comes back as zeros;
 *   reap     children exit while a thread of the parent wakes the futex
 *            their own thread sleeps on, and the parent reaps each once
 *            with its own status; threads exit while the parent joins them;
 *   requeue  two threads requeue the waiters of two futex words onto each
 *            other with FUTEX_CMP_REQUEUE while the waiters wait with
 *            timeouts of 1 to 3 ms; every requeue gives a count or EAGAIN
 *            (kernel/futex/requeue.c:483-485), every wait 0, ETIMEDOUT,
 *            EAGAIN or EINTR, and every waiter returns;
 *   stat     threads on every processor make newfstatat, fstatat64 on
 *            i386, and openat of 16 warmed files and read them, each seeing
 *            the inode, size and data it had;
 *   drm      on vgem, a submitter attaches and signals write fences while
 *            two threads attach read fences, which are EBUSY while a write
 *            fence is pending, and a write fence while any is
 *            (drivers/gpu/drm/vgem/vgem_fence.c:154-158), and poll the
 *            buffer's dma-buf; with no fence the dma-buf is readable and
 *            writable, and a write fence leaves it neither until it is
 *            signalled (drivers/dma-buf/dma-buf.c:253-321). Not run without
 *            a vgem device, which the kernel makes with rach2.vgem.
 *
 * A CALL of -u is a system call by name in this program's ABI, a number as
 * syscall:N, an entry vector as vector:N, an ioctl request as ioctl:0xN, or
 * page_fault for vector 14. On i386, newfstatat names fstatat64 and mmap
 * mmap2, the calls the C library makes in their place. The assertion
 * `calls` holds when, over the modes run, bkl/calls counts the call and no
 * hold of the big lock that an entry of it began (debugfs(5)); the
 * program's own reading of the file uses lseek and readv, which -u
 * refuses. The report's group `calls` is `checked` with -u and
 * `unchecked`, without the assertion, otherwise; the group `vgem` is
 * `present`, with the drm mode's assertion, or `absent` without it.
 *
 * Prints "LOCKSTRESSMUSL OK" and exits 0, or "LOCKSTRESSMUSL FAIL: " and the
 * modes that failed and exits 1; 2 for a wrong usage.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../probe_report.h"

extern char **environ;

#define PAGE                  4096u
#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_CMP_REQUEUE     4
#define FUTEX_PRIVATE         128
#define MAX_THREADS           64
#define MAX_CALLS             32
#define CALLS_TEXT            (256u << 10)
#define POLL_PIPES            64
#define BIG_WRITE             (256u << 10)
#define SHARE_PAGES           16u
#define STAT_FILES            16
#define REAP_BATCH            8
#define VECTOR_PAGE_FAULT     14
#define VGEM_FENCE_WRITE      1u
#define DRM_IOCTL_VERSION     _IOWR('d', 0x00, struct drm_version)
#define DRM_IOCTL_PRIME_H2FD  _IOWR('d', 0x2d, struct drm_prime_handle)
#define DRM_IOCTL_DUMB        _IOWR('d', 0xb2, struct drm_mode_create_dumb)
#define DRM_IOCTL_VGEM_ATTACH _IOWR('d', 0x41, struct drm_vgem_fence_attach)
#define DRM_IOCTL_VGEM_SIGNAL _IOW('d', 0x42, struct drm_vgem_fence_signal)

/* The DRM structures this program passes, as Linux's uapi has them. */
struct drm_version {
    int version_major, version_minor, version_patchlevel;
    size_t name_len;
    char *name;
    size_t date_len;
    char *date;
    size_t desc_len;
    char *desc;
};
struct drm_prime_handle {
    uint32_t handle, flags;
    int32_t fd;
};
struct drm_mode_create_dumb {
    uint32_t height, width, bpp, flags, handle, pitch;
    uint64_t size;
};
struct drm_vgem_fence_attach {
    uint32_t handle, flags, out_fence, pad;
};
struct drm_vgem_fence_signal {
    uint32_t fence, flags;
};

/* A kernel timespec of 64-bit fields, which futex_time64 takes on i386. */
struct ts64 {
    int64_t sec, nsec;
};

enum {
    M_SWITCH,
    M_POLL,
    M_CLOSE,
    M_UNMAP,
    M_FAULT,
    M_SHARE,
    M_REAP,
    M_REQUEUE,
    M_STAT,
    M_DRM,
    NMODES
};

static const char *const mode_names[NMODES] = {"switch", "poll", "close",   "unmap", "fault",
                                               "share",  "reap", "requeue", "stat",  "drm"};

/* Each mode's first failure, its reason for not running, and whether it ran
 * whole at least once; `mode` is the mode under way. */
static char mode_why[NMODES][160];
static char mode_skip[NMODES][96];
static int mode_ran[NMODES], mode_sel[NMODES];
static int mode;
static pthread_mutex_t why_lock = PTHREAD_MUTEX_INITIALIZER;

static int ncpu = 1;
static long scale = 1;
static const char *dir = ".";

/* The calls -u names: the kind and number of the line of bkl/calls, and
 * the counts of the snapshots before and after the modes. */
struct call {
    const char *arg;
    const char *kind; /* "syscall", "vector" or "ioctl" */
    unsigned long number;
    unsigned long long calls[2], holds[2];
    int found[2];
};
static struct call ucalls[MAX_CALLS];
static int nucalls;
static int calls_fd = -1;
static char calls_text[CALLS_TEXT];

/* A guarded access: the handler of SIGSEGV and SIGBUS jumps back to the
 * thread's `guard_jb` when the fault's address is in [guard_lo, guard_hi). */
static __thread sigjmp_buf *guard_jb;
static char *volatile guard_lo, *volatile guard_hi;

/** Record that the mode under way failed, the first reason kept, and say
 * why on stderr; any thread may call it. */
static void fail(const char *fmt, ...)
{
    char buf[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "lockstressmusl: %s: %s\n", mode_names[mode], buf);
    pthread_mutex_lock(&why_lock);
    if (mode_why[mode][0] == '\0') {
        snprintf(mode_why[mode], sizeof(mode_why[mode]), "%s", buf);
    }
    pthread_mutex_unlock(&why_lock);
}

/** Run the caller on processor `cpu` modulo those online; 0 or -1. */
static int pin(int cpu)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu % ncpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

/** FUTEX_WAIT on `w` while it holds `val`, for `ns` nanoseconds or, when
 * `ns` is 0, without a timeout; `op` adds FUTEX_PRIVATE or not. The call's
 * result, with errno set on -1. */
static long futex_wait(uint32_t *w, uint32_t val, long ns, int op)
{
#ifdef SYS_futex_time64
    struct ts64 t;

    t.sec = 0;
    t.nsec = ns;
    return syscall(SYS_futex_time64, w, FUTEX_WAIT | op, val, ns ? &t : NULL, 0, 0);
#else
    struct timespec t;

    t.tv_sec = 0;
    t.tv_nsec = ns;
    return syscall(SYS_futex, w, FUTEX_WAIT | op, val, ns ? &t : NULL, 0, 0);
#endif
}

/** FUTEX_WAKE up to `n` waiters of `w`; the number woken or -1. */
static long futex_wake(uint32_t *w, int n, int op)
{
    return syscall(SYS_futex, w, FUTEX_WAKE | op, n, NULL, 0, 0);
}

/** Jump back to the faulting thread's guard, or end the process by the
 * signal's default action for a fault outside it. */
static void on_fault(int sig, siginfo_t *si, void *uc)
{
    char *a = (char *)si->si_addr;

    (void)uc;
    if (guard_jb && a >= guard_lo && a < guard_hi) {
        siglongjmp(*guard_jb, sig);
    }
    signal(sig, SIG_DFL);
}

/** Do nothing: SIGUSR1, without SA_RESTART, ends the call it interrupts. */
static void on_wake(int sig)
{
    (void)sig;
}

/** Start `n` threads of `fn`, each given its index; 0, or -1 with those
 * started joined. */
static int start_threads(pthread_t *t, int n, void *(*fn)(void *))
{
    int i;

    for (i = 0; i < n; i++) {
        if (pthread_create(&t[i], NULL, fn, (void *)(intptr_t)i) != 0) {
            fail("pthread_create: thread %d of %d", i, n);
            while (--i >= 0) {
                pthread_join(t[i], NULL);
            }
            return -1;
        }
    }
    return 0;
}

/** Join `n` threads. */
static void join_threads(pthread_t *t, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        pthread_join(t[i], NULL);
    }
}

/** Wait for the child `pid`; 0 when it exited with 0, -1 otherwise, said
 * as `what` in the mode's failure. */
static int reap_ok(pid_t pid, const char *what)
{
    int st;

    if (waitpid(pid, &st, 0) != pid) {
        fail("%s: waitpid: %s", what, strerror(errno));
        return -1;
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fail("%s: status 0x%x", what, st);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * switch: switches and wakes.
 */

static uint32_t sw_word[MAX_THREADS / 2];
static int sw_pipe[MAX_THREADS / 2][2][2];
static int sw_pairs;
static long sw_rounds;

/** Abandon pair `p` after a failure, so that neither side waits on. */
static void sw_abort(int p)
{
    __atomic_store_n(&sw_word[p], ~0u, __ATOMIC_SEQ_CST);
    futex_wake(&sw_word[p], INT_MAX, FUTEX_PRIVATE);
}

/** One side of a pair: thread 2p passes odd counts and thread 2p+1 even
 * ones, through the futex word and then through the pipes. */
static void *sw_thread(void *arg)
{
    int i = (int)(intptr_t)arg, p = i / 2, side = i % 2;
    uint32_t *w = &sw_word[p], v, k, want, got;
    long r;

    pin(i);
    for (r = 0; r < sw_rounds; r++) {
        k = 2u * (uint32_t)r;
        if (side == 0) {
            __atomic_store_n(w, k + 1, __ATOMIC_SEQ_CST);
            futex_wake(w, 1, FUTEX_PRIVATE);
            while ((v = __atomic_load_n(w, __ATOMIC_SEQ_CST)) == k + 1) {
                futex_wait(w, k + 1, 0, FUTEX_PRIVATE);
            }
            want = k + 2;
        } else {
            while ((v = __atomic_load_n(w, __ATOMIC_SEQ_CST)) == k) {
                futex_wait(w, k, 0, FUTEX_PRIVATE);
            }
            want = k + 1;
        }
        if (v == ~0u) {
            return NULL;
        }
        if (v != want) {
            fail("pair %d side %d: futex word %u, expected %u", p, side, v, want);
            sw_abort(p);
            return NULL;
        }
        if (side == 1) {
            __atomic_store_n(w, k + 2, __ATOMIC_SEQ_CST);
            futex_wake(w, 1, FUTEX_PRIVATE);
        }
        if ((r & 15) == 0) {
            sched_yield();
        }
    }
    for (r = 0; r < sw_rounds; r++) {
        if (side == 0) {
            k = (uint32_t)r;
            if (write(sw_pipe[p][0][1], &k, 4) != 4 || read(sw_pipe[p][1][0], &got, 4) != 4) {
                fail("pair %d: pipe: %s", p, strerror(errno));
                break;
            }
            want = k + 1;
        } else {
            if (read(sw_pipe[p][0][0], &got, 4) != 4) {
                fail("pair %d: pipe: %s", p, strerror(errno));
                break;
            }
            k = got + 1;
            if (write(sw_pipe[p][1][1], &k, 4) != 4) {
                fail("pair %d: pipe: %s", p, strerror(errno));
                break;
            }
            want = (uint32_t)r;
        }
        if (got != want) {
            fail("pair %d side %d: pipe gave %u, expected %u", p, side, got, want);
            break;
        }
    }
    /* A side that stopped early closes its pipe ends so the other ends. */
    if (r < sw_rounds) {
        close(sw_pipe[p][side][1]);
        close(sw_pipe[p][!side][0]);
        sw_pipe[p][side][1] = sw_pipe[p][!side][0] = -1;
    }
    return NULL;
}

/** The switch mode: a pair of threads for each processor, at least two. */
static void mode_switch(void)
{
    pthread_t t[MAX_THREADS];
    int p, s;

    sw_pairs = ncpu < 2 ? 2 : ncpu > MAX_THREADS / 2 ? MAX_THREADS / 2 : ncpu;
    sw_rounds = 2000 * scale;
    for (p = 0; p < sw_pairs; p++) {
        sw_word[p] = 0;
        for (s = 0; s < 2; s++) {
            if (pipe(sw_pipe[p][s]) != 0) {
                fail("pipe: %s", strerror(errno));
                return;
            }
        }
    }
    if (start_threads(t, 2 * sw_pairs, sw_thread) == 0) {
        join_threads(t, 2 * sw_pairs);
    }
    for (p = 0; p < sw_pairs; p++) {
        for (s = 0; s < 4; s++) {
            if (sw_pipe[p][s / 2][s % 2] >= 0) {
                close(sw_pipe[p][s / 2][s % 2]);
            }
        }
    }
    printf("lockstressmusl: switch: %d pairs, %ld rounds each way\n", sw_pairs, sw_rounds);
}

/* ------------------------------------------------------------------------
 * poll: poll and epoll with writers on other processors during the scan.
 */

static int pl_pipe[POLL_PIPES][2];
static int pl_writers;
static long pl_writes;

/** Writer `i`: write `pl_writes` bytes, each its pipe's index, to the
 * pipes in an order of its own, on its own processor. */
static void *pl_writer(void *arg)
{
    int i = (int)(intptr_t)arg;
    long k;
    unsigned char b;
    int p;

    pin(i + 1);
    for (k = 0; k < pl_writes; k++) {
        p = (int)((i * 7 + k * 13) % POLL_PIPES);
        b = (unsigned char)p;
        if (write(pl_pipe[p][1], &b, 1) != 1) {
            fail("writer %d: write: %s", i, strerror(errno));
            return NULL;
        }
    }
    return NULL;
}

/** Read what pipe `p` has into the totals; -1 after a failure. */
static int pl_drain(int p, long *got)
{
    unsigned char buf[512];
    ssize_t n, j;

    n = read(pl_pipe[p][0], buf, sizeof(buf));
    if (n <= 0) {
        fail("pipe %d was readable and read gave %ld (%s)", p, (long)n,
             n < 0 ? strerror(errno) : "end");
        return -1;
    }
    for (j = 0; j < n; j++) {
        if (buf[j] != (unsigned char)p) {
            fail("pipe %d gave the byte %u of pipe %u", p, buf[j], buf[j]);
            return -1;
        }
    }
    *got += n;
    return 0;
}

/** Wait by poll for the pipes and read those it gives; 0, or -1 after a
 * failure. */
static int pl_scan_poll(long *got)
{
    struct pollfd fds[POLL_PIPES];
    int i, n;

    for (i = 0; i < POLL_PIPES; i++) {
        fds[i].fd = pl_pipe[i][0];
        fds[i].events = POLLIN;
        fds[i].revents = 0;
    }
    n = poll(fds, POLL_PIPES, -1);
    if (n <= 0) {
        fail("poll gave %d (%s)", n, n < 0 ? strerror(errno) : "none");
        return -1;
    }
    for (i = 0; i < POLL_PIPES; i++) {
        if (fds[i].revents & ~POLLIN) {
            fail("poll gave revents 0x%x for pipe %d", fds[i].revents, i);
            return -1;
        }
        if (fds[i].revents) {
            n--;
            if (pl_drain(i, got) != 0) {
                return -1;
            }
        }
    }
    if (n != 0) {
        fail("poll counted %d more ready than it marked", n);
        return -1;
    }
    return 0;
}

/** Wait by epoll on the set `ep` and read the pipes it gives; 0, or -1
 * after a failure. */
static int pl_scan_epoll(int ep, long *got)
{
    struct epoll_event evs[16];
    int i, n;

    n = epoll_wait(ep, evs, 16, -1);
    if (n <= 0) {
        fail("epoll_wait gave %d (%s)", n, n < 0 ? strerror(errno) : "none");
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (evs[i].events != EPOLLIN || evs[i].data.u32 >= POLL_PIPES) {
            fail("epoll_wait gave events 0x%x for %u", evs[i].events, evs[i].data.u32);
            return -1;
        }
        if (pl_drain((int)evs[i].data.u32, got) != 0) {
            return -1;
        }
    }
    return 0;
}

/** An epoll set of every pipe's read end; its descriptor or -1. */
static int pl_epoll_open(void)
{
    struct epoll_event ev;
    int ep = epoll_create1(0), i;

    if (ep < 0) {
        fail("epoll_create1: %s", strerror(errno));
        return -1;
    }
    for (i = 0; i < POLL_PIPES; i++) {
        ev.events = EPOLLIN;
        ev.data.u32 = (uint32_t)i;
        if (epoll_ctl(ep, EPOLL_CTL_ADD, pl_pipe[i][0], &ev) != 0) {
            fail("epoll_ctl: %s", strerror(errno));
            close(ep);
            return -1;
        }
    }
    return ep;
}

/** One round of the writers against a reader by poll, `use_epoll` 0, or by
 * epoll. */
static void pl_round(int use_epoll)
{
    pthread_t t[MAX_THREADS];
    long want = (long)pl_writers * pl_writes, got = 0, waits = 0;
    int ep = -1, i, bad = 0;

    if (use_epoll && (ep = pl_epoll_open()) < 0) {
        return;
    }
    if (start_threads(t, pl_writers, pl_writer) != 0) {
        if (ep >= 0) {
            close(ep);
        }
        return;
    }
    while (got < want && !bad) {
        waits++;
        bad = (use_epoll ? pl_scan_epoll(ep, &got) : pl_scan_poll(&got)) != 0;
    }
    if (bad) {
        /* A writer blocked on a full pipe ends with EPIPE, SIGPIPE ignored. */
        for (i = 0; i < POLL_PIPES; i++) {
            close(pl_pipe[i][0]);
            pl_pipe[i][0] = -1;
        }
    }
    join_threads(t, pl_writers);
    if (!bad && got != want) {
        fail("%s read %ld bytes of %ld", use_epoll ? "epoll" : "poll", got, want);
    }
    printf("lockstressmusl: poll: %s: %ld bytes in %ld waits from %d writers\n",
           use_epoll ? "epoll" : "poll", got, waits, pl_writers);
    if (ep >= 0) {
        close(ep);
    }
}

/** The poll mode: a round by poll, then one by epoll. */
static void mode_poll(void)
{
    int i, j;

    pl_writers = ncpu < 2 ? 1 : ncpu - 1 > MAX_THREADS ? MAX_THREADS : ncpu - 1;
    pl_writes = 2000 * scale;
    for (i = 0; i < POLL_PIPES; i++) {
        if (pipe(pl_pipe[i]) != 0) {
            fail("pipe: %s", strerror(errno));
            while (--i >= 0) {
                close(pl_pipe[i][0]);
                close(pl_pipe[i][1]);
            }
            return;
        }
    }
    pin(0);
    pl_round(0);
    if (mode_why[M_POLL][0] == '\0') {
        pl_round(1);
    }
    for (i = 0; i < POLL_PIPES; i++) {
        for (j = 0; j < 2; j++) {
            if (pl_pipe[i][j] >= 0) {
                close(pl_pipe[i][j]);
            }
        }
    }
}

/* ------------------------------------------------------------------------
 * close: close and dup2 during a long operation.
 */

static pthread_barrier_t cl_three, cl_two;
static long cl_rounds;
static int cl_fd, cl_other, cl_variant, cl_delay, cl_err;
static long cl_res;
static unsigned char cl_byte;
static char *cl_big;

/** The thread in the long call: a read of one byte, or a write of
 * BIG_WRITE bytes to a full pipe, on `cl_fd`. */
static void *cl_caller(void *arg)
{
    long it;

    (void)arg;
    pin(0);
    for (it = 0; it < cl_rounds; it++) {
        pthread_barrier_wait(&cl_three);
        errno = 0;
        if (cl_variant < 2) {
            cl_res = (long)read(cl_fd, &cl_byte, 1);
        } else {
            cl_res = (long)write(cl_fd, cl_big, BIG_WRITE);
        }
        cl_err = errno;
        pthread_barrier_wait(&cl_three);
    }
    return NULL;
}

/** The thread that closes `cl_fd`, or puts `cl_other` in its place, after
 * a delay of its round's kind, from another processor. */
static void *cl_closer(void *arg)
{
    struct timespec ms = {0, 1000000L};
    volatile long spin;
    long it;

    (void)arg;
    pin(1);
    for (it = 0; it < cl_rounds; it++) {
        pthread_barrier_wait(&cl_three);
        if (cl_delay == 1) {
            sched_yield();
        } else if (cl_delay == 2) {
            for (spin = 0; spin < 20000; spin++) {
            }
        } else if (cl_delay == 3) {
            /* interval: the call is asleep when its descriptor goes */
            nanosleep(&ms, NULL);
        }
        if (cl_variant % 2 == 0) {
            if (close(cl_fd) != 0) {
                fail("close: %s", strerror(errno));
            }
        } else if (syscall(SYS_dup2, cl_other, cl_fd) != cl_fd) {
            /* Linux replaces a descriptor another thread's call holds
             * (fs/file.c:1205-1248); a refusal is closed over, so that the
             * round still ends. The call is made directly, since musl's
             * dup2 makes it again for as long as it is EBUSY. */
            fail("dup2 onto the descriptor of a %s in another thread: %s",
                 cl_variant < 2 ? "read" : "write", strerror(errno));
            close(cl_fd);
        }
        pthread_barrier_wait(&cl_two);
        pthread_barrier_wait(&cl_three);
    }
    return NULL;
}

/** Fill the pipe whose write end is `w` and give the bytes it took. */
static long cl_fill(int w)
{
    long n = 0;
    ssize_t k;

    fcntl(w, F_SETFL, O_NONBLOCK);
    while ((k = write(w, cl_big, PAGE)) > 0) {
        n += k;
    }
    fcntl(w, F_SETFL, 0);
    return n;
}

/* The descriptors of a round of the close mode: the pipe the call is on,
 * the pipe whose byte a dup2 puts in its place for a read, and what the
 * round saw; -1 for a descriptor not made. */
struct cl_round {
    int p[2], q[2];
    long filled, drained;
    int wrote; /* the byte 'P' written 1, refused with EPIPE -1, failed 0 */
};

/** Make the files of round `it` and set the call's descriptor; 0 or -1. */
static int cl_setup(struct cl_round *r, long it)
{
    cl_variant = (int)(it % 4);
    cl_delay = (int)(it / 4 % 4);
    r->q[0] = r->q[1] = -1;
    r->filled = r->drained = 0;
    r->wrote = 0;
    if (pipe(r->p) != 0) {
        fail("pipe: %s", strerror(errno));
        return -1;
    }
    if (cl_variant < 2) {
        if (pipe(r->q) != 0 || write(r->q[1], "Q", 1) != 1) {
            fail("pipe: %s", strerror(errno));
            return -1;
        }
        cl_fd = r->p[0];
        cl_other = r->q[0];
        return 0;
    }
    r->filled = cl_fill(r->p[1]);
    cl_fd = r->p[1];
    cl_other = open("/dev/null", O_WRONLY);
    if (cl_other < 0) {
        fail("/dev/null: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/** With the descriptor gone, end the call: write the read's byte, or drain
 * the pipe of the write until its end. */
static void cl_finish(struct cl_round *r)
{
    char buf[4096];
    ssize_t k;

    if (cl_variant < 2) {
        errno = 0;
        r->wrote = write(r->p[1], "P", 1) == 1 ? 1 : errno == EPIPE ? -1 : 0;
        if (r->wrote == 0) {
            fail("write: %s", strerror(errno));
        }
        return;
    }
    while ((k = read(r->p[0], buf, sizeof(buf))) > 0) {
        r->drained += k;
    }
}

/** Is the end of the round one of Linux's? A read or write that began on
 * the file finishes on it, and one that began after the close fails with
 * EBADF or, after dup2, uses the new file, whose byte or emptiness the
 * write or drain shows. */
static int cl_linux(const struct cl_round *r)
{
    long full = r->filled + (long)BIG_WRITE;

    switch (cl_variant) {
    case 0:
        return (cl_res == 1 && cl_byte == 'P' && r->wrote == 1) ||
               (cl_res == -1 && cl_err == EBADF && r->wrote == -1);
    case 1:
        return cl_res == 1 &&
               ((cl_byte == 'P' && r->wrote == 1) || (cl_byte == 'Q' && r->wrote == -1));
    case 2:
        return (cl_res == (long)BIG_WRITE && r->drained == full) ||
               (cl_res == -1 && cl_err == EBADF && r->drained == r->filled);
    default:
        return cl_res == (long)BIG_WRITE && (r->drained == full || r->drained == r->filled);
    }
}

/** Close what round `r` made: the descriptor's number was closed, or holds
 * `cl_other`'s file. */
static void cl_cleanup(const struct cl_round *r)
{
    close(r->p[0]);
    close(r->p[1]);
    if (r->q[0] >= 0) {
        close(r->q[0]);
        close(r->q[1]);
    }
    if (cl_variant >= 2) {
        close(cl_other);
    }
}

/** The close mode: rounds of the four kinds in turn, each kind with each
 * delay of the closer. */
static void mode_close(void)
{
    static const char *const names[4] = {"read-close", "read-dup2", "write-close", "write-dup2"};
    struct cl_round r;
    pthread_t t[2];
    long it, counts[4][2];

    cl_rounds = 400 * scale;
    cl_big = malloc(BIG_WRITE);
    if (!cl_big) {
        fail("malloc: %s", strerror(errno));
        return;
    }
    memset(cl_big, 'W', BIG_WRITE);
    memset(counts, 0, sizeof(counts));
    pthread_barrier_init(&cl_three, NULL, 3);
    pthread_barrier_init(&cl_two, NULL, 2);
    if (pthread_create(&t[0], NULL, cl_caller, NULL) != 0 ||
        pthread_create(&t[1], NULL, cl_closer, NULL) != 0) {
        /* Not reached with threads to spare; a lone thread waits forever. */
        fail("pthread_create");
        return;
    }
    for (it = 0; it < cl_rounds; it++) {
        if (cl_setup(&r, it) != 0) {
            /* Leave the threads waiting; the process ends with the run. */
            return;
        }
        pthread_barrier_wait(&cl_three);
        pthread_barrier_wait(&cl_two);
        cl_finish(&r);
        /* A pipe left with no reader, or drained to its end, has no call
         * asleep on it on Linux; one still asleep is woken by the signal,
         * which ends it with EINTR or a short count. */
        if (cl_variant >= 2 || r.wrote == -1) {
            pthread_kill(t[0], SIGUSR1);
        }
        pthread_barrier_wait(&cl_three);
        if (!cl_linux(&r)) {
            fail("%s round %ld: the call gave %ld (%s), byte %u, write %d, %ld bytes drained of "
                 "%ld filled",
                 names[cl_variant], it, cl_res, strerror(cl_err), cl_byte, r.wrote, r.drained,
                 r.filled);
        }
        /* In the call: the read got P, or the write reached the pipe. */
        counts[cl_variant][cl_variant < 2 ? cl_res == 1 && cl_byte == 'P' : r.drained > r.filled]++;
        cl_cleanup(&r);
    }
    join_threads(t, 2);
    pthread_barrier_destroy(&cl_three);
    pthread_barrier_destroy(&cl_two);
    free(cl_big);
    for (it = 0; it < 4; it++) {
        printf("lockstressmusl: close: %s: %ld in the call, %ld after\n", names[it], counts[it][1],
               counts[it][0]);
    }
}

/* ------------------------------------------------------------------------
 * unmap: two processes unmapping at once, each with readers on the other
 * processors, so that each unmap sends a shootdown while the other's does.
 */

#define UNMAP_PAGES 16u

static char *um_base;
static uint32_t um_gen, um_tag;
static volatile int um_stop, um_failed;
static long um_faults;

/** A reader of the region: each word read is 0, a fresh page's, or the tag
 * and a generation from the one it saw published to the one after the
 * latest; anything else came through a translation the unmap left. */
static void *um_reader(void *arg)
{
    int i = (int)(intptr_t)arg;
    sigjmp_buf jb;
    volatile unsigned int p;
    uint32_t g, g2, v;

    pin(i + 1);
    guard_jb = &jb;
    while (!um_stop) {
        p = 0;
        if (sigsetjmp(jb, 1)) {
            __atomic_add_fetch(&um_faults, 1, __ATOMIC_RELAXED);
            continue;
        }
        for (; p < UNMAP_PAGES; p++) {
            g = __atomic_load_n(&um_gen, __ATOMIC_ACQUIRE);
            v = *(volatile uint32_t *)(um_base + p * PAGE);
            g2 = __atomic_load_n(&um_gen, __ATOMIC_ACQUIRE);
            if (v != 0 && (v >> 24 != um_tag || (v & 0xffffffu) < g || (v & 0xffffffu) > g2 + 1)) {
                fprintf(stderr,
                        "lockstressmusl: unmap: process %u reader %d: page %u gave 0x%x "
                        "between generations %u and %u\n",
                        um_tag, i, p, v, g, g2);
                um_failed = 1;
                um_stop = 1;
            }
        }
    }
    guard_jb = NULL;
    return NULL;
}

/** One of the two processes: map, fill and publish the region `rounds`
 * times on processor 0 while its readers read it. Exits 0, or 1 after a
 * failure, which it has said on stderr. */
static void um_child(uint32_t tag, int gate, long rounds)
{
    pthread_t t[MAX_THREADS];
    int readers = ncpu < 2 ? 1 : ncpu - 1 > MAX_THREADS ? MAX_THREADS : ncpu - 1, i;
    unsigned int p;
    uint32_t gen;
    char c;

    um_tag = tag;
    um_base =
        mmap(NULL, UNMAP_PAGES * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (um_base == MAP_FAILED) {
        _exit(1);
    }
    guard_lo = um_base;
    guard_hi = um_base + UNMAP_PAGES * PAGE;
    pin(0);
    for (i = 0; i < readers; i++) {
        if (pthread_create(&t[i], NULL, um_reader, (void *)(intptr_t)i) != 0) {
            _exit(1);
        }
    }
    if (read(gate, &c, 1) < 0) {
        _exit(1);
    }
    for (gen = 1; gen <= (uint32_t)rounds && !um_stop; gen++) {
        if (munmap(um_base, UNMAP_PAGES * PAGE) != 0 ||
            mmap(um_base, UNMAP_PAGES * PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != um_base) {
            fprintf(stderr, "lockstressmusl: unmap: process %u: %s\n", tag, strerror(errno));
            um_failed = 1;
            break;
        }
        for (p = 0; p < UNMAP_PAGES; p++) {
            *(volatile uint32_t *)(um_base + p * PAGE) = tag << 24 | gen;
        }
        __atomic_store_n(&um_gen, gen, __ATOMIC_RELEASE);
    }
    um_stop = 1;
    for (i = 0; i < readers; i++) {
        pthread_join(t[i], NULL);
    }
    printf("lockstressmusl: unmap: process %u: %u maps, %ld reads faulted\n", tag, gen - 1,
           um_faults);
    fflush(stdout);
    _exit(um_failed);
}

/** The unmap mode: two processes, started together through one gate. */
static void mode_unmap(void)
{
    pid_t pid[2];
    int gate[2], i;

    if (pipe(gate) != 0) {
        fail("pipe: %s", strerror(errno));
        return;
    }
    fflush(stdout);
    for (i = 0; i < 2; i++) {
        pid[i] = fork();
        if (pid[i] == 0) {
            close(gate[1]);
            um_child((uint32_t)i + 1, gate[0], 500 * scale);
        }
        if (pid[i] < 0) {
            fail("fork: %s", strerror(errno));
        }
    }
    close(gate[0]);
    /* The gate's close starts both at once. */
    close(gate[1]);
    for (i = 0; i < 2; i++) {
        if (pid[i] > 0) {
            reap_ok(pid[i], i ? "process 2" : "process 1");
        }
    }
}

/* ------------------------------------------------------------------------
 * fault: four threads faulting one page under memory pressure while a
 * fifth maps, protects and unmaps it.
 */

#define FAULTERS 4

static char *fl_page;
static volatile int fl_stop;
static long fl_faults, fl_writes;

/** Faulter `i`: write a count to its word of the page and read it back,
 * which gives the count or, when the page was mapped afresh between, 0;
 * a fault ends the try. */
static void *fl_faulter(void *arg)
{
    int i = (int)(intptr_t)arg;
    volatile uint32_t *w = (volatile uint32_t *)(fl_page + 64 * i);
    sigjmp_buf jb;
    uint32_t c = 0, v;
    volatile long writes = 0;

    pin(i + 1);
    guard_jb = &jb;
    while (!fl_stop) {
        c = c % 0xFFFFFFFEu + 1;
        if (sigsetjmp(jb, 1)) {
            __atomic_add_fetch(&fl_faults, 1, __ATOMIC_RELAXED);
            continue;
        }
        __atomic_store_n(w, c, __ATOMIC_RELAXED);
        v = __atomic_load_n(w, __ATOMIC_RELAXED);
        if (v != c && v != 0) {
            fail("faulter %d wrote %u and read %u", i, c, v);
            break;
        }
        writes++;
    }
    guard_jb = NULL;
    __atomic_add_fetch(&fl_writes, writes, __ATOMIC_RELAXED);
    return NULL;
}

/** Spin a little, so that the faulters run between the steps. */
static void fl_pause(void)
{
    volatile int k;

    for (k = 0; k < 5000; k++) {
    }
}

/** Read MemAvailable of /proc/meminfo in bytes; 0 when it cannot be read. */
static unsigned long long mem_available(void)
{
    char line[128];
    unsigned long long kb = 0;
    FILE *f = fopen("/proc/meminfo", "r");

    if (!f) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
            break;
        }
    }
    fclose(f);
    return kb * 1024;
}

/** The child that keeps memory short: map and touch half of what was
 * available, unmap it and again, until its pipe `stop` reads its end. */
static void fl_pressure(int stop)
{
    unsigned long long avail = mem_available() / 2;
    size_t size, off;
    struct pollfd pf;
    char *m;

    if (avail > (1ull << 30)) {
        avail = 1ull << 30;
    }
    size = (size_t)avail;
    pf.fd = stop;
    pf.events = POLLIN;
    while (size >= (16u << 20) && poll(&pf, 1, 0) == 0) {
        m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) {
            size /= 2;
            continue;
        }
        for (off = 0; off < size; off += PAGE) {
            m[off] = 1;
        }
        munmap(m, size);
    }
    _exit(0);
}

/** The fault mode: the fifth thread's rounds on processor 0 beside the
 * faulters and the memory child. */
static void mode_fault(void)
{
    pthread_t t[FAULTERS];
    int stop[2], st = 0, i;
    long k, rounds = 500 * scale, ro_checks = 0;
    uint32_t seen[FAULTERS];
    pid_t hog;

    if (pipe(stop) != 0) {
        fail("pipe: %s", strerror(errno));
        return;
    }
    fflush(stdout);
    hog = fork();
    if (hog == 0) {
        close(stop[1]);
        fl_pressure(stop[0]);
    }
    close(stop[0]);
    fl_page = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (hog < 0 || fl_page == MAP_FAILED) {
        fail("fork or mmap: %s", strerror(errno));
        close(stop[1]);
        if (hog > 0) {
            waitpid(hog, NULL, 0);
        }
        return;
    }
    guard_lo = fl_page;
    guard_hi = fl_page + PAGE;
    fl_stop = 0;
    pin(0);
    if (start_threads(t, FAULTERS, fl_faulter) == 0) {
        for (k = 0; k < rounds; k++) {
            if (mmap(fl_page, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                     -1, 0) != fl_page) {
                fail("mmap: %s", strerror(errno));
                break;
            }
            fl_pause();
            if (mprotect(fl_page, PAGE, PROT_READ) != 0) {
                fail("mprotect: %s", strerror(errno));
                break;
            }
            for (i = 0; i < FAULTERS; i++) {
                seen[i] = *(volatile uint32_t *)(fl_page + 64 * i);
            }
            fl_pause();
            sched_yield();
            for (i = 0; i < FAULTERS; i++) {
                if (*(volatile uint32_t *)(fl_page + 64 * i) != seen[i]) {
                    fail("round %ld: faulter %d's word went from %u to %u while the page was "
                         "read-only",
                         k, i, seen[i], *(volatile uint32_t *)(fl_page + 64 * i));
                }
            }
            ro_checks++;
            if (mprotect(fl_page, PAGE, PROT_READ | PROT_WRITE) != 0) {
                fail("mprotect: %s", strerror(errno));
                break;
            }
            fl_pause();
            if (munmap(fl_page, PAGE) != 0) {
                fail("munmap: %s", strerror(errno));
                break;
            }
            fl_pause();
        }
        fl_stop = 1;
        join_threads(t, FAULTERS);
    }
    munmap(fl_page, PAGE);
    close(stop[1]);
    if (waitpid(hog, &st, 0) != hog) {
        fail("waitpid: %s", strerror(errno));
    } else if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0) &&
               !(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL)) {
        fail("the memory child ended with status 0x%x", st);
    }
    printf("lockstressmusl: fault: %ld rounds, %ld writes read back, %ld faults, memory child %s\n",
           ro_checks, fl_writes, fl_faults, WIFSIGNALED(st) ? "killed" : "exited");
}

/* ------------------------------------------------------------------------
 * share: processes sharing a file mapping while others write, truncate and
 * unmap it.
 */

/* The stop word the mapping readers watch, in a page all share. */
static volatile uint32_t *sh_stop;

/** Is every 64th byte of the page at `p` 0 or a letter? A fault, past the
 * file's end, counts as yes. */
static int sh_page_ok(volatile unsigned char *p)
{
    unsigned int off;
    unsigned char b;

    for (off = 0; off < PAGE; off += 64) {
        b = p[off];
        if (b != 0 && (b < 'a' || b > 'z')) {
            fprintf(stderr, "lockstressmusl: share: byte %u of a page is 0x%x\n", off, b);
            return 0;
        }
    }
    return 1;
}

/** A process of the share mode: 0 and 1 read the shared mapping `map`, 1
 * writing it too, until the stop word is set; 2 writes the file, 3
 * truncates it and grows it again, and 4 maps it, writes and unmaps.
 * Exits 0, or 1 after a failure, which it has said on stderr. */
static void sh_child(int role, int fd, unsigned char *map, long rounds)
{
    unsigned char buf[PAGE], *m;
    volatile unsigned int p;
    volatile long k;
    sigjmp_buf jb;

    guard_jb = &jb;
    guard_lo = (char *)map;
    guard_hi = (char *)map + SHARE_PAGES * PAGE;
    pin(role);
    if (role <= 1) {
        p = 0;
        if (sigsetjmp(jb, 1)) {
            p++;
        }
        while (!*sh_stop) {
            for (; p < SHARE_PAGES; p++) {
                if (!sh_page_ok(map + p * PAGE)) {
                    _exit(1);
                }
                if (role == 1) {
                    map[p * PAGE + 128] = 'm';
                }
            }
            p = 0;
        }
        _exit(0);
    }
    for (k = 0; k < rounds; k++) {
        if (role == 2) {
            memset(buf, 'a' + (int)(k % 26), PAGE);
            if (pwrite(fd, buf, PAGE, (off_t)(k % SHARE_PAGES) * PAGE) != (ssize_t)PAGE) {
                fprintf(stderr, "lockstressmusl: share: pwrite: %s\n", strerror(errno));
                _exit(1);
            }
        } else if (role == 3) {
            if (ftruncate(fd, (off_t)(k * 5 % (SHARE_PAGES + 1)) * PAGE) != 0 ||
                ftruncate(fd, (off_t)SHARE_PAGES * PAGE) != 0) {
                fprintf(stderr, "lockstressmusl: share: ftruncate: %s\n", strerror(errno));
                _exit(1);
            }
        } else {
            m = mmap(NULL, SHARE_PAGES * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (m == MAP_FAILED) {
                fprintf(stderr, "lockstressmusl: share: mmap: %s\n", strerror(errno));
                _exit(1);
            }
            guard_lo = (char *)m;
            guard_hi = (char *)m + SHARE_PAGES * PAGE;
            p = 0;
            if (sigsetjmp(jb, 1)) {
                p++;
            }
            for (; p < SHARE_PAGES; p++) {
                m[p * PAGE + 256] = 'u';
                if (!sh_page_ok(m + p * PAGE)) {
                    _exit(1);
                }
            }
            munmap(m, SHARE_PAGES * PAGE);
        }
    }
    _exit(0);
}

/** Run the five processes on the file `fd`, mapped shared at `map`, and
 * reap them; 0 when each exited 0, -1 otherwise. */
static int sh_run(int fd, unsigned char *map)
{
    static const char *const roles[5] = {"the mapping reader", "the mapping writer", "the writer",
                                         "the truncator", "the mapper"};
    pid_t pid[5];
    int i, bad = 0;

    *sh_stop = 0;
    fflush(stdout);
    for (i = 0; i < 5; i++) {
        pid[i] = fork();
        if (pid[i] == 0) {
            sh_child(i, fd, map, 300 * scale);
        }
        if (pid[i] < 0) {
            fail("fork: %s", strerror(errno));
            bad = 1;
        }
    }
    /* The readers of the mapping stop once the others have ended. */
    for (i = 2; i < 7; i++) {
        if (i == 5) {
            *sh_stop = 1;
        }
        if (pid[i % 5] > 0 && reap_ok(pid[i % 5], roles[i % 5]) != 0) {
            bad = 1;
        }
    }
    return bad ? -1 : 0;
}

/** Check that the mapping `map` and the file `fd` agree both ways, and that
 * a page truncated away and grown again reads as zeros. */
static void sh_final(int fd, unsigned char *map)
{
    char buf[PAGE];
    unsigned char c;
    unsigned int p, off;

    memset(buf, 'z', PAGE);
    for (p = 0; p < SHARE_PAGES; p++) {
        if (pwrite(fd, buf, PAGE, (off_t)p * PAGE) != (ssize_t)PAGE) {
            fail("pwrite: %s", strerror(errno));
            return;
        }
    }
    for (off = 0; off < SHARE_PAGES * PAGE; off++) {
        if (map[off] != 'z') {
            fail("byte %u of the mapping is 0x%x after the file was written 'z'", off, map[off]);
            return;
        }
    }
    for (p = 0; p < SHARE_PAGES; p++) {
        map[p * PAGE + 7] = 'y';
        if (pread(fd, &c, 1, (off_t)p * PAGE + 7) != 1 || c != 'y') {
            fail("page %u: the file did not read the mapping's 'y'", p);
            return;
        }
    }
    if (ftruncate(fd, (off_t)(SHARE_PAGES / 2) * PAGE) != 0 ||
        ftruncate(fd, (off_t)SHARE_PAGES * PAGE) != 0) {
        fail("ftruncate: %s", strerror(errno));
        return;
    }
    for (off = SHARE_PAGES / 2 * PAGE; off < SHARE_PAGES * PAGE; off++) {
        if (map[off] != 0) {
            fail("byte %u of the mapping is 0x%x after truncation and growth", off, map[off]);
            return;
        }
    }
}

/** The share mode, on a file of its own in `dir`. */
static void mode_share(void)
{
    char path[PATH_MAX];
    unsigned char *map = MAP_FAILED;
    int fd;

    snprintf(path, sizeof(path), "%s/lockstress.%d.share", dir, (int)getpid());
    sh_stop = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (sh_stop == MAP_FAILED || fd < 0 || ftruncate(fd, (off_t)SHARE_PAGES * PAGE) != 0 ||
        (map = mmap(NULL, SHARE_PAGES * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) ==
            MAP_FAILED) {
        fail("%s: %s", path, strerror(errno));
    } else if (sh_run(fd, map) == 0) {
        sh_final(fd, map);
    }
    if (map != MAP_FAILED) {
        munmap(map, SHARE_PAGES * PAGE);
    }
    if (fd >= 0) {
        close(fd);
        unlink(path);
    }
    if (sh_stop != MAP_FAILED) {
        munmap((void *)sh_stop, PAGE);
    }
}

/* ------------------------------------------------------------------------
 * reap: exit and reap during a wake.
 */

/* The futex words the children's threads sleep on, in a page the parent
 * shares with them. */
static uint32_t *rp_words;

/** A child's thread: sleep on its word until the parent wakes it. */
static void *rp_sleeper(void *arg)
{
    uint32_t *w = arg;

    while (__atomic_load_n(w, __ATOMIC_SEQ_CST) == 0) {
        futex_wait(w, 0, 0, 0);
    }
    return NULL;
}

/** The parent's waker: wake each child's word of the batch. */
static void *rp_waker(void *arg)
{
    int i;

    (void)arg;
    pin(1);
    for (i = 0; i < REAP_BATCH; i++) {
        __atomic_store_n(&rp_words[i], 1, __ATOMIC_SEQ_CST);
        futex_wake(&rp_words[i], INT_MAX, 0);
    }
    return NULL;
}

/** A thread that ends at once with its index. */
static void *rp_quick(void *arg)
{
    return arg;
}

/** The exit code of child `i` of batch `batch`, from 1 to 100. */
static int rp_code(long batch, int i)
{
    return (int)((batch * REAP_BATCH + i) % 100) + 1;
}

/** Fork the REAP_BATCH children of batch `batch`, each with a thread asleep
 * on its word and its main thread waiting at its gate; their pids into
 * `pid`, -1 for one not made, and the gates' write ends into `gate`. */
static void rp_fork(long batch, pid_t pid[REAP_BATCH], int gate[REAP_BATCH])
{
    int g[2], i;
    pthread_t s;
    char c;

    fflush(stdout);
    for (i = 0; i < REAP_BATCH; i++) {
        rp_words[i] = 0;
        pid[i] = -1;
        gate[i] = -1;
        if (pipe(g) != 0) {
            fail("pipe: %s", strerror(errno));
            continue;
        }
        pid[i] = fork();
        if (pid[i] == 0) {
            pin(i + 2);
            if (pthread_create(&s, NULL, rp_sleeper, &rp_words[i]) != 0 || read(g[0], &c, 1) != 1) {
                _exit(126);
            }
            syscall(SYS_exit_group, rp_code(batch, i));
        }
        if (pid[i] < 0) {
            fail("fork: %s", strerror(errno));
        }
        close(g[0]);
        gate[i] = g[1];
    }
}

/** Run batch `batch`: open the children's gates while the waker wakes their
 * sleepers, and reap each child once; the children reaped. */
static long rp_batch(long batch)
{
    pthread_t waker;
    pid_t pid[REAP_BATCH], got;
    int gate[REAP_BATCH], seen[REAP_BATCH], i, j, st, woke;
    long reaped = 0;

    rp_fork(batch, pid, gate);
    memset(seen, 0, sizeof(seen));
    woke = pthread_create(&waker, NULL, rp_waker, NULL) == 0;
    if (!woke) {
        fail("pthread_create: %s", strerror(errno));
        rp_waker(NULL);
    }
    for (i = 0; i < REAP_BATCH; i++) {
        if (gate[i] >= 0 && write(gate[i], "x", 1) != 1) {
            fail("write: %s", strerror(errno));
        }
        close(gate[i]);
    }
    for (i = 0; i < REAP_BATCH; i++) {
        if (pid[i] < 0) {
            continue;
        }
        got = wait4(-1, &st, 0, NULL);
        for (j = 0; j < REAP_BATCH && pid[j] != got; j++) {
        }
        if (got < 0 || j == REAP_BATCH) {
            fail("wait4 gave %d (%s), no child of the batch", (int)got, strerror(errno));
        } else if (seen[j]++) {
            fail("child %d reaped twice", (int)got);
        } else if (!WIFEXITED(st) || WEXITSTATUS(st) != rp_code(batch, j)) {
            fail("child %d ended with status 0x%x, expected exit %d", (int)got, st,
                 rp_code(batch, j));
        } else {
            reaped++;
        }
    }
    if (woke) {
        pthread_join(waker, NULL);
    }
    return reaped;
}

/** The reap mode: batches of children, then threads joined as they end. */
static void mode_reap(void)
{
    pthread_t t;
    long batch, k, children = 0;
    void *rc = NULL;

    rp_words = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (rp_words == MAP_FAILED) {
        fail("mmap: %s", strerror(errno));
        return;
    }
    for (batch = 0; batch < 8 * scale; batch++) {
        children += rp_batch(batch);
    }
    munmap(rp_words, PAGE);
    for (k = 0; k < 200 * scale; k++) {
        if (pthread_create(&t, NULL, rp_quick, (void *)(intptr_t)(k + 1)) != 0) {
            fail("pthread_create: %s", strerror(errno));
            break;
        }
        if ((k & 3) == 0) {
            sched_yield();
        }
        if (pthread_join(t, &rc) != 0 || rc != (void *)(intptr_t)(k + 1)) {
            fail("thread %ld: join gave %p", k + 1, rc);
            break;
        }
    }
    printf("lockstressmusl: reap: %ld children reaped, %ld threads joined\n", children, k);
}

/* ------------------------------------------------------------------------
 * requeue: opposite-direction futex requeues with timeouts.
 */

#define RQ_WAITERS 4

static uint32_t rq_word[2];
static volatile int rq_stop;
static long rq_rounds, rq_woken, rq_timeouts, rq_moved;

/** Waiter `i`: wait on word i % 2 with a timeout of 1 to 3 ms, over and
 * over; each wait ends woken, timed out, or refused for a changed word. */
static void *rq_waiter(void *arg)
{
    int i = (int)(intptr_t)arg;
    uint32_t *w = &rq_word[i % 2];
    long r, n = 0;

    pin(i + 2);
    while (!rq_stop) {
        r = futex_wait(w, __atomic_load_n(w, __ATOMIC_SEQ_CST), 1000000L * (1 + n % 3),
                       FUTEX_PRIVATE);
        n++;
        if (r == 0) {
            __atomic_add_fetch(&rq_woken, 1, __ATOMIC_RELAXED);
        } else if (errno == ETIMEDOUT) {
            __atomic_add_fetch(&rq_timeouts, 1, __ATOMIC_RELAXED);
        } else if (errno != EAGAIN && errno != EINTR) {
            fail("waiter %d: FUTEX_WAIT gave %ld (%s)", i, r, strerror(errno));
            break;
        }
    }
    return NULL;
}

/** Requeuer `i`: move the waiters of word i to word !i, waking one, with
 * the word's value as the comparison; now and then change the word. */
static void *rq_requeuer(void *arg)
{
    int i = (int)(intptr_t)arg;
    uint32_t *from = &rq_word[i], *to = &rq_word[!i];
    long k, r;

    pin(i);
    for (k = 0; k < rq_rounds; k++) {
        r = syscall(SYS_futex, from, FUTEX_CMP_REQUEUE | FUTEX_PRIVATE, 1, INT_MAX, to,
                    __atomic_load_n(from, __ATOMIC_SEQ_CST));
        if (r >= 0) {
            __atomic_add_fetch(&rq_moved, r, __ATOMIC_RELAXED);
        } else if (errno != EAGAIN) {
            fail("requeuer %d: FUTEX_CMP_REQUEUE gave %ld (%s)", i, r, strerror(errno));
            break;
        }
        if (k % 7 == 0) {
            __atomic_add_fetch(from, 1, __ATOMIC_SEQ_CST);
        }
    }
    return NULL;
}

/** The requeue mode: the requeuers' rounds, then every waiter woken and
 * joined. */
static void mode_requeue(void)
{
    pthread_t w[RQ_WAITERS], q[2];
    int i;

    rq_stop = 0;
    rq_rounds = 4000 * scale;
    rq_woken = rq_timeouts = rq_moved = 0;
    if (start_threads(w, RQ_WAITERS, rq_waiter) != 0) {
        return;
    }
    if (start_threads(q, 2, rq_requeuer) == 0) {
        join_threads(q, 2);
    }
    rq_stop = 1;
    for (i = 0; i < 2; i++) {
        __atomic_add_fetch(&rq_word[i], 1, __ATOMIC_SEQ_CST);
        futex_wake(&rq_word[i], INT_MAX, FUTEX_PRIVATE);
    }
    /* Each waiter's timeout ends its last wait. */
    join_threads(w, RQ_WAITERS);
    printf("lockstressmusl: requeue: %ld woken or requeued, %ld waits woken, %ld timed out\n",
           rq_moved, rq_woken, rq_timeouts);
}

/* ------------------------------------------------------------------------
 * stat: warmed newfstatat and openat.
 */

/* The call that stats a name: newfstatat on x86-64, whose structure is
 * musl's struct stat there, and fstatat64 on i386, whose structure is
 * Linux's struct stat64. */
#ifdef SYS_newfstatat
#define FSTATAT_NR   SYS_newfstatat
#define FSTATAT_NAME "newfstatat"
#else
#define FSTATAT_NR   SYS_fstatat64
#define FSTATAT_NAME "fstatat64"
#endif

/* The fields of a stat that the stat mode reads, in 64 bits. */
struct kstat {
    uint64_t st_ino;
    int64_t st_size;
    uint32_t st_mode;
};

/** The system call newfstatat, or fstatat64 on i386, of `name` from `at`,
 * made directly, since musl's fstatat makes stat or statx in its place;
 * its fields into `k`. 0, or -1 with errno set. */
static int raw_fstatat(int at, const char *name, struct kstat *k)
{
#ifdef SYS_newfstatat
    struct stat st;

    if (syscall(FSTATAT_NR, at, name, &st, 0) != 0) {
        return -1;
    }
    k->st_ino = st.st_ino;
    k->st_size = st.st_size;
    k->st_mode = st.st_mode;
#else
    unsigned char st[96];
    uint32_t raw_mode;

    if (syscall(FSTATAT_NR, at, name, st, 0) != 0) {
        return -1;
    }
    /* struct stat64: st_mode at 16, st_size at 44, st_ino at 88. */
    memcpy(&raw_mode, st + 16, 4);
    memcpy(&k->st_size, st + 44, 8);
    memcpy(&k->st_ino, st + 88, 8);
    k->st_mode = raw_mode;
#endif
    return 0;
}

static char st_dir[PATH_MAX - 16];
static int st_dirfd;
static ino_t st_ino[STAT_FILES];
static long st_rounds;

/** Thread `i`: stat, open and read each file, through the directory's
 * descriptor and by its path from the working directory in turn. */
static void *st_thread(void *arg)
{
    int i = (int)(intptr_t)arg, f, fd, at;
    char name[PATH_MAX], want[8], got[8];
    struct kstat ks;
    long r;

    pin(i);
    for (r = 0; r < st_rounds; r++) {
        for (f = 0; f < STAT_FILES; f++) {
            snprintf(want, sizeof(want), "f%02d", f);
            if ((r + f) % 2) {
                at = st_dirfd;
                snprintf(name, sizeof(name), "%s", want);
            } else {
                at = AT_FDCWD;
                snprintf(name, sizeof(name), "%s/%s", st_dir, want);
            }
            if (raw_fstatat(at, name, &ks) != 0) {
                fail("thread %d: %s %s: %s", i, FSTATAT_NAME, name, strerror(errno));
                return NULL;
            }
            if (ks.st_ino != (uint64_t)st_ino[f] || ks.st_size != 3 || !S_ISREG(ks.st_mode)) {
                fail("thread %d: %s: inode %llu size %lld mode 0%o, expected inode %lu size 3", i,
                     name, (unsigned long long)ks.st_ino, (long long)ks.st_size,
                     (unsigned)ks.st_mode, (unsigned long)st_ino[f]);
                return NULL;
            }
            fd = openat(at, name, O_RDONLY);
            if (fd < 0) {
                fail("thread %d: openat %s: %s", i, name, strerror(errno));
                return NULL;
            }
            memset(got, 0, sizeof(got));
            if (read(fd, got, sizeof(got)) != 3 || memcmp(got, want, 3) != 0) {
                fail("thread %d: %s read other than its name", i, name);
                close(fd);
                return NULL;
            }
            close(fd);
        }
    }
    return NULL;
}

/** The stat mode, on a directory of its own in `dir`: the files made and
 * stat'd once, then the threads' rounds. */
static void mode_stat(void)
{
    pthread_t t[MAX_THREADS];
    char name[PATH_MAX];
    struct stat st;
    int f, fd, n = ncpu < 2 ? 2 : ncpu > MAX_THREADS ? MAX_THREADS : ncpu;

    snprintf(st_dir, sizeof(st_dir), "%s/lockstress.%d.d", dir, (int)getpid());
    st_rounds = 200 * scale;
    if (mkdir(st_dir, 0700) != 0) {
        fail("mkdir %s: %s", st_dir, strerror(errno));
        return;
    }
    st_dirfd = open(st_dir, O_RDONLY | O_DIRECTORY);
    for (f = 0; f < STAT_FILES && st_dirfd >= 0; f++) {
        snprintf(name, sizeof(name), "f%02d", f);
        fd = openat(st_dirfd, name, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0 || write(fd, name, 3) != 3 || close(fd) != 0 ||
            fstatat(st_dirfd, name, &st, 0) != 0) {
            fail("%s/%s: %s", st_dir, name, strerror(errno));
            break;
        }
        st_ino[f] = st.st_ino;
    }
    if (st_dirfd < 0) {
        fail("%s: %s", st_dir, strerror(errno));
    } else if (f == STAT_FILES && start_threads(t, n, st_thread) == 0) {
        join_threads(t, n);
        printf("lockstressmusl: stat: %d threads, %ld lookups each of fstatat and openat\n", n,
               st_rounds * STAT_FILES);
    }
    for (f = 0; f < STAT_FILES && st_dirfd >= 0; f++) {
        snprintf(name, sizeof(name), "f%02d", f);
        unlinkat(st_dirfd, name, 0);
    }
    if (st_dirfd >= 0) {
        close(st_dirfd);
    }
    rmdir(st_dir);
}

/* ------------------------------------------------------------------------
 * drm: DRM BUSY against submission on vgem.
 */

static int dr_fd = -1, dr_buf = -1;
static uint32_t dr_handle;
static long dr_rounds, dr_attached[3], dr_busy[3], dr_polls;

/** Open the first primary DRM node whose driver is vgem, since a render
 * node refuses MODE_CREATE_DUMB, which has no DRM_RENDER_ALLOW
 * (drivers/gpu/drm/drm_ioctl.c:636); its descriptor or -1. */
static int dr_open(void)
{
    struct drm_version v;
    char path[32], name[16];
    int i, fd;

    for (i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", i);
        fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        memset(&v, 0, sizeof(v));
        memset(name, 0, sizeof(name));
        v.name = name;
        v.name_len = sizeof(name) - 1;
        if (ioctl(fd, DRM_IOCTL_VERSION, &v) == 0 && strcmp(name, "vgem") == 0) {
            printf("lockstressmusl: drm: %s\n", path);
            return fd;
        }
        close(fd);
    }
    return -1;
}

/** Attach a fence, a writer's when `write` is set, and signal it at once;
 * 1 when attached, 0 when EBUSY, -1 after a failure. */
static int dr_attach_signal(int write)
{
    struct drm_vgem_fence_attach a;
    struct drm_vgem_fence_signal s;

    memset(&a, 0, sizeof(a));
    a.handle = dr_handle;
    a.flags = write ? VGEM_FENCE_WRITE : 0;
    if (ioctl(dr_fd, DRM_IOCTL_VGEM_ATTACH, &a) != 0) {
        if (errno == EBUSY) {
            return 0;
        }
        fail("VGEM_FENCE_ATTACH: %s", strerror(errno));
        return -1;
    }
    if (write) {
        sched_yield();
    }
    memset(&s, 0, sizeof(s));
    s.fence = a.out_fence;
    if (ioctl(dr_fd, DRM_IOCTL_VGEM_SIGNAL, &s) != 0) {
        fail("VGEM_FENCE_SIGNAL of fence %u: %s", a.out_fence, strerror(errno));
        return -1;
    }
    return 1;
}

/** Thread 0 submits write fences; threads 1 and 2 attach read fences,
 * which a pending write fence makes EBUSY, and poll the dma-buf, whose
 * answer is in Linux's set. */
static void *dr_thread(void *arg)
{
    int i = (int)(intptr_t)arg, r;
    struct pollfd pf;
    long k;

    pin(i);
    for (k = 0; k < dr_rounds; k++) {
        r = dr_attach_signal(i == 0);
        if (r < 0) {
            return NULL;
        }
        if (r) {
            dr_attached[i]++;
        } else {
            dr_busy[i]++;
        }
        if (i != 0) {
            pf.fd = dr_buf;
            pf.events = POLLIN | POLLOUT;
            pf.revents = 0;
            r = poll(&pf, 1, 0);
            if (r < 0 || (pf.revents & ~(POLLIN | POLLOUT))) {
                fail("poll of the dma-buf gave %d, revents 0x%x", r, pf.revents);
                return NULL;
            }
            __atomic_add_fetch(&dr_polls, 1, __ATOMIC_RELAXED);
        }
    }
    return NULL;
}

/** Poll the dma-buf without waiting and check its answer against `want`;
 * 0 or -1. */
static int dr_poll_is(short want, const char *when)
{
    struct pollfd pf;

    pf.fd = dr_buf;
    pf.events = POLLIN | POLLOUT;
    pf.revents = 0;
    if (poll(&pf, 1, 0) < 0 || pf.revents != want) {
        fail("%s: the dma-buf polled 0x%x, expected 0x%x", when, pf.revents, want);
        return -1;
    }
    return 0;
}

/** The drm mode: the dma-buf's answers with and without a write fence,
 * then the three threads, then the buffer idle again. */
static void mode_drm(void)
{
    struct drm_mode_create_dumb d;
    struct drm_prime_handle h;
    struct drm_vgem_fence_attach a;
    struct drm_vgem_fence_signal s;
    pthread_t t[3];
    int i;

    dr_fd = dr_open();
    if (dr_fd < 0) {
        snprintf(mode_skip[M_DRM], sizeof(mode_skip[M_DRM]),
                 "no vgem device; the kernel makes one with rach2.vgem");
        return;
    }
    memset(&d, 0, sizeof(d));
    d.width = 64;
    d.height = 64;
    d.bpp = 32;
    memset(&h, 0, sizeof(h));
    if (ioctl(dr_fd, DRM_IOCTL_DUMB, &d) != 0) {
        fail("MODE_CREATE_DUMB: %s", strerror(errno));
        goto out;
    }
    dr_handle = d.handle;
    h.handle = d.handle;
    h.flags = O_RDWR | O_CLOEXEC;
    if (ioctl(dr_fd, DRM_IOCTL_PRIME_H2FD, &h) != 0) {
        fail("PRIME_HANDLE_TO_FD: %s", strerror(errno));
        goto out;
    }
    dr_buf = h.fd;
    /* No fence: readable and writable; a write fence: neither. */
    if (dr_poll_is(POLLIN | POLLOUT, "no fence") != 0) {
        goto out;
    }
    memset(&a, 0, sizeof(a));
    a.handle = dr_handle;
    a.flags = VGEM_FENCE_WRITE;
    if (ioctl(dr_fd, DRM_IOCTL_VGEM_ATTACH, &a) != 0) {
        fail("VGEM_FENCE_ATTACH: %s", strerror(errno));
        goto out;
    }
    dr_poll_is(0, "a write fence pending");
    if (dr_attach_signal(0) != 0) {
        fail("a read fence was attached beside a pending write fence");
    }
    memset(&s, 0, sizeof(s));
    s.fence = a.out_fence;
    if (ioctl(dr_fd, DRM_IOCTL_VGEM_SIGNAL, &s) != 0) {
        fail("VGEM_FENCE_SIGNAL: %s", strerror(errno));
        goto out;
    }
    if (dr_poll_is(POLLIN | POLLOUT, "the write fence signalled") != 0) {
        goto out;
    }
    dr_rounds = 2000 * scale;
    memset(dr_attached, 0, sizeof(dr_attached));
    memset(dr_busy, 0, sizeof(dr_busy));
    dr_polls = 0;
    if (start_threads(t, 3, dr_thread) == 0) {
        join_threads(t, 3);
    }
    /* Every fence signalled: a write fence attaches, and the buffer is idle. */
    if (dr_attach_signal(1) != 1) {
        fail("a write fence could not be attached after every fence was signalled");
    }
    dr_poll_is(POLLIN | POLLOUT, "every fence signalled");
    for (i = 0; i < 3; i++) {
        printf("lockstressmusl: drm: thread %d: %ld %s fences, %ld EBUSY\n", i, dr_attached[i],
               i ? "read" : "write", dr_busy[i]);
    }
    printf("lockstressmusl: drm: %ld polls; ioctl requests: attach 0x%lx, signal 0x%lx\n", dr_polls,
           (unsigned long)DRM_IOCTL_VGEM_ATTACH, (unsigned long)DRM_IOCTL_VGEM_SIGNAL);
out:
    if (dr_buf >= 0) {
        close(dr_buf);
        dr_buf = -1;
    }
    close(dr_fd);
    dr_fd = -1;
}

/* ------------------------------------------------------------------------
 * The calls of -u, and the run.
 */

/* The system calls -u names, by this ABI's number. */
static const struct {
    const char *name;
    long nr;
} call_names[] = {
    {"read", SYS_read},
    {"write", SYS_write},
    {"close", SYS_close},
    {"openat", SYS_openat},
    {"ioctl", SYS_ioctl},
    {"poll", SYS_poll},
    {"ppoll", SYS_ppoll},
    {"epoll_wait", SYS_epoll_wait},
    {"epoll_pwait", SYS_epoll_pwait},
    {"epoll_ctl", SYS_epoll_ctl},
    {"dup2", SYS_dup2},
    {"dup3", SYS_dup3},
    {"pipe2", SYS_pipe2},
    {"fcntl", SYS_fcntl},
    {"munmap", SYS_munmap},
    {"mprotect", SYS_mprotect},
    {"madvise", SYS_madvise},
    {"futex", SYS_futex},
    {"sched_yield", SYS_sched_yield},
    {"sched_setaffinity", SYS_sched_setaffinity},
    {"wait4", SYS_wait4},
    {"exit", SYS_exit},
    {"exit_group", SYS_exit_group},
    {"clone", SYS_clone},
    {"pread64", SYS_pread64},
    {"pwrite64", SYS_pwrite64},
    {"ftruncate", SYS_ftruncate},
    {"getpid", SYS_getpid},
    {FSTATAT_NAME, FSTATAT_NR},
#ifdef SYS_mmap2
    {"mmap", SYS_mmap2},
    {"mmap2", SYS_mmap2},
#else
    {"mmap", SYS_mmap},
#endif
#ifndef SYS_newfstatat
    {"newfstatat", FSTATAT_NR},
#endif
#ifdef SYS_futex_time64
    {"futex_time64", SYS_futex_time64},
#endif
#ifdef SYS_ftruncate64
    {"ftruncate64", SYS_ftruncate64},
#endif
#ifdef SYS_fcntl64
    {"fcntl64", SYS_fcntl64},
#endif
#ifdef SYS_pipe
    {"pipe", SYS_pipe},
#endif
};

/** Add the call `arg` of -u to `ucalls`; 0, or -1 when it names none or one
 * the program's reading of bkl/calls makes. */
static int add_call(const char *arg)
{
    struct call *c;
    char *end;
    unsigned int i;

    if (nucalls == MAX_CALLS) {
        return -1;
    }
    c = &ucalls[nucalls];
    memset(c, 0, sizeof(*c));
    c->arg = arg;
    end = NULL;
    if (strncmp(arg, "syscall:", 8) == 0) {
        c->kind = "syscall";
        c->number = strtoul(arg + 8, &end, 0);
    } else if (strncmp(arg, "vector:", 7) == 0) {
        c->kind = "vector";
        c->number = strtoul(arg + 7, &end, 0);
    } else if (strncmp(arg, "ioctl:", 6) == 0) {
        c->kind = "ioctl";
        c->number = strtoul(arg + 6, &end, 16);
    } else if (strcmp(arg, "page_fault") == 0) {
        c->kind = "vector";
        c->number = VECTOR_PAGE_FAULT;
    } else {
        for (i = 0; i < sizeof(call_names) / sizeof(call_names[0]); i++) {
            if (strcmp(arg, call_names[i].name) == 0) {
                c->kind = "syscall";
                c->number = (unsigned long)call_names[i].nr;
                break;
            }
        }
        if (!c->kind) {
            return -1;
        }
    }
    if (end && (end == strchr(arg, ':') + 1 || *end != '\0')) {
        return -1;
    }
    if (strcmp(c->kind, "syscall") == 0 &&
        (c->number == (unsigned long)SYS_lseek || c->number == (unsigned long)SYS_readv)) {
        return -1;
    }
    /* bkl/calls counts a call numbered 512 or more as 512. */
    if (strcmp(c->kind, "syscall") == 0 && c->number > 512) {
        c->number = 512;
    }
    nucalls++;
    return 0;
}

/** Read bkl/calls through its descriptor, with lseek and readv alone, into
 * snapshot `at` of each call of -u; 0, or -1 when it cannot be read. */
static int calls_snapshot(int at)
{
    struct iovec iov;
    ssize_t n, got = 0;
    char *line, *next, kind[16];
    unsigned long long num, calls, holds;
    int i;

    if (lseek(calls_fd, 0, SEEK_SET) != 0) {
        return -1;
    }
    do {
        iov.iov_base = calls_text + got;
        iov.iov_len = sizeof(calls_text) - 1 - (size_t)got;
        n = readv(calls_fd, &iov, 1);
        got += n > 0 ? n : 0;
    } while (n > 0 && (size_t)got < sizeof(calls_text) - 1);
    if (n < 0) {
        return -1;
    }
    calls_text[got] = '\0';
    for (line = calls_text; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) {
            *next++ = '\0';
        }
        if (sscanf(line, "%15s %lli %llu %*u %llu", kind, (long long *)&num, &calls, &holds) != 4) {
            continue;
        }
        for (i = 0; i < nucalls; i++) {
            if (strcmp(kind, ucalls[i].kind) == 0 && num == ucalls[i].number) {
                ucalls[i].calls[at] = calls;
                ucalls[i].holds[at] = holds;
                ucalls[i].found[at] = 1;
            }
        }
    }
    return 0;
}

/** Judge `calls`: each call of -u counted, and with no hold, between the
 * snapshots; 0 when it held, -1 otherwise. */
static int calls_judge(void)
{
    char why[160];
    unsigned long long dc, dh;
    int i;

    why[0] = '\0';
    for (i = 0; i < nucalls; i++) {
        struct call *c = &ucalls[i];

        if (!c->found[1]) {
            /* An ioctl request takes its line at its first call. */
            dc = dh = 0;
        } else {
            dc = (c->calls[1] - c->calls[0]) & 0xffffffffull;
            dh = (c->holds[1] - c->holds[0]) & 0xffffffffull;
        }
        printf(strcmp(c->kind, "ioctl") == 0
                   ? "lockstressmusl: calls: %s (%s %#lx): %llu calls, %llu holds\n"
                   : "lockstressmusl: calls: %s (%s %lu): %llu calls, %llu holds\n",
               c->arg, c->kind, c->number, dc, dh);
        if (why[0] == '\0' && dc == 0) {
            snprintf(why, sizeof(why), "%s was not called", c->arg);
        } else if (why[0] == '\0' && dh != 0) {
            snprintf(why, sizeof(why), "%s took the big lock %llu times in %llu calls", c->arg, dh,
                     dc);
        }
    }
    if (why[0]) {
        report_fail("calls", why);
        return -1;
    }
    report_pass("calls");
    return 0;
}

static void (*const mode_fn[NMODES])(void) = {mode_switch, mode_poll,  mode_close, mode_unmap,
                                              mode_fault,  mode_share, mode_reap,  mode_requeue,
                                              mode_stat,   mode_drm};

/** Set the modes -m names, a list separated by commas; 0 or -1. */
static int select_modes(char *list)
{
    char *name, *save = NULL;
    int m;

    for (name = strtok_r(list, ",", &save); name; name = strtok_r(NULL, ",", &save)) {
        for (m = 0; m < NMODES && strcmp(name, mode_names[m]) != 0; m++) {
        }
        if (m == NMODES) {
            return -1;
        }
        mode_sel[m] = 1;
    }
    return 0;
}

/** Say how the program is used; 2, its exit status. */
static int usage(void)
{
    fprintf(stderr, "usage: lockstressmusl [-n SCALE] [-t SECONDS] [-m MODE[,MODE]...] "
                    "[-C DIR] [-u CALL]...\n");
    return 2;
}

/** Read the options into the globals and `*seconds`; 0, or 2 after a
 * wrong usage, which it has said. */
static int parse_args(int argc, char **argv, long *seconds)
{
    int opt, m, any = 0;

    while ((opt = getopt(argc, argv, "n:t:m:C:u:")) != -1) {
        if (opt == 'n' && (scale = strtol(optarg, NULL, 10)) >= 1) {
            continue;
        }
        if (opt == 't' && (*seconds = strtol(optarg, NULL, 10)) >= 1) {
            continue;
        }
        if (opt == 'C') {
            dir = optarg;
            continue;
        }
        if (opt == 'm' && select_modes(optarg) == 0) {
            any = 1;
            continue;
        }
        if (opt == 'u' && add_call(optarg) == 0) {
            continue;
        }
        if (opt == 'm' || opt == 'u') {
            fprintf(stderr, "lockstressmusl: -%c %s: no such %s\n", opt, optarg,
                    opt == 'm' ? "mode" : "call, or one the program's reading of bkl/calls makes");
        }
        return usage();
    }
    if (optind != argc) {
        return usage();
    }
    for (m = 0; m < NMODES; m++) {
        mode_sel[m] |= !any;
    }
    return 0;
}

/** Run the modes -m selects once, and again until `seconds` have passed. */
static void run_modes(long seconds)
{
    struct timespec t0, t1;
    long passes = 0;
    int m, steps = 0;

    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        for (m = 0; m < NMODES; m++) {
            if (!mode_sel[m]) {
                continue;
            }
            mode = m;
            mode_skip[m][0] = '\0';
            mode_fn[m]();
            fflush(stdout);
            if (mode_skip[m][0] == '\0') {
                mode_ran[m] = 1;
            }
            report_progress("modes", ++steps);
        }
        passes++;
        clock_gettime(CLOCK_MONOTONIC, &t1);
    } while (t1.tv_sec - t0.tv_sec < seconds);
    printf("lockstressmusl: %ld passes\n", passes);
}

/** Report the branch of the group `vgem` and the drm mode's assertion,
 * which only a machine with a vgem device has; -1 when it failed. */
static int report_drm(void)
{
    int fd, present;

    if (mode_sel[M_DRM]) {
        present = mode_skip[M_DRM][0] == '\0';
    } else {
        fd = dr_open();
        present = fd >= 0;
        if (present) {
            close(fd);
        }
    }
    report_branch("vgem", present ? "present" : "absent");
    if (!present) {
        return 0;
    }
    if (mode_why[M_DRM][0]) {
        report_fail("drm", mode_why[M_DRM]);
        return -1;
    }
    if (mode_ran[M_DRM]) {
        report_pass("drm");
    } else {
        report_notrun("drm", "not selected by -m");
    }
    return 0;
}

/** Report each mode, and `calls`, which `calls_ok` says could be read, in
 * the branch of the group `calls` that -u chose; 0 when every assertion run
 * held, -1 otherwise. */
static int report_results(int calls_ok)
{
    int m, failed = 0;

    for (m = 0; m < M_DRM; m++) {
        if (mode_why[m][0]) {
            report_fail(mode_names[m], mode_why[m]);
            failed = 1;
        } else if (mode_ran[m]) {
            report_pass(mode_names[m]);
        } else {
            report_notrun(mode_names[m], "not selected by -m");
        }
    }
    failed |= report_drm() != 0;
    report_branch("calls", nucalls ? "checked" : "unchecked");
    if (nucalls && !calls_ok) {
        report_fail("calls", "/sys/kernel/debug/bkl/calls cannot be read");
    } else if (nucalls && calls_judge() != 0) {
        calls_ok = 0;
    }
    report_end();
    if (!failed && calls_ok) {
        return 0;
    }
    printf("LOCKSTRESSMUSL FAIL:");
    for (m = 0; m < NMODES; m++) {
        if (mode_why[m][0]) {
            printf(" %s", mode_names[m]);
        }
    }
    printf("%s\n", calls_ok ? "" : " calls");
    return -1;
}

/** Run the modes and report each. */
int main(int argc, char **argv)
{
    struct sigaction sa;
    long seconds = 0;
    int calls_ok = 1;

    if (parse_args(argc, argv, &seconds) != 0) {
        return 2;
    }
    ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) {
        ncpu = 1;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_wake;
    sigaction(SIGUSR1, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdout, NULL, _IOLBF, 0);

    report_begin(report_case(environ, "guest.ia32.lockstressmusl"));
    printf("lockstressmusl: %d processors, scale %ld\n", ncpu, scale);
    if (nucalls) {
        calls_fd = open("/sys/kernel/debug/bkl/calls", O_RDONLY);
        if (calls_fd < 0 || calls_snapshot(0) != 0) {
            fprintf(stderr, "lockstressmusl: /sys/kernel/debug/bkl/calls: %s\n", strerror(errno));
            calls_ok = 0;
        }
    }
    run_modes(seconds);
    if (nucalls && calls_ok && calls_snapshot(1) != 0) {
        calls_ok = 0;
    }
    if (report_results(calls_ok) != 0) {
        return 1;
    }
    puts("LOCKSTRESSMUSL OK");
    return 0;
}
