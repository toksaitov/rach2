/*
 * inodechurnmusl: churn small files in one directory from several processes
 * and threads at once, as a program that keeps a cache of small files does
 * (piglit's shader_runner through Mesa's disk cache), so that inodes are
 * made, renamed over, unlinked while open and given again within moments.
 *
 * Usage: inodechurnmusl [-t SECONDS] [-p PROCS] [-j THREADS] [-C DIR]
 *
 * PROCS processes (4 by default), each with THREADS threads (4), work in
 * DIR/churn, DIR "." by default, for SECONDS (10; 0 runs until killed).
 * Each thread keeps a file of its own open for writing, appends a little to
 * it at each step, renames it over one of 32 shared names while it is still
 * open and writing, and closes it a few steps later, so that a file is
 * replaced or unlinked while its writes wait in the kernel's batch and its
 * last put comes under another file's lock. Beside that, at each step, by a
 * seeded choice: write a file under a name of its own, fsync it now and
 * then, close it and rename it over a shared name; unlink a shared name;
 * open one, read it and hold it open over a few more steps before it closes
 * it; stat one; create one in place with O_TRUNC; link one to a second name
 * and unlink that. ENOENT and EEXIST are expected, since the threads race
 * for the same names; any other failure ends the run with status 1 and a
 * line on stderr that names the call.
 *
 * The parent prints `inodechurnmusl: progress: N s: M operations` once a
 * second. At the end of the time, at SIGINT or SIGTERM, when a process ends
 * early or when a fork fails, it stops every process it started, reaps
 * them, and removes DIR/churn with every name in it, since a process that
 * failed leaves the files it was writing. A signal ends a run of SECONDS 0
 * as its time would; it fails a timed run. Then it reads the
 * kernel's log (klogctl), where an inode freed twice shows as `ext2: free of
 * inode N, whose bit is clear`, and a block as `ext2: free of block N, whose
 * bit is clear`, and prints `inodechurnmusl: result: ok`, or `failed` with
 * status 1 after a failed call or such a line. A node freed while in use
 * panics the kernel instead, and the result never comes.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define SHARED   32     /* the shared names, c0 to c31 */
#define HELD     4      /* the files a thread keeps open at once */
#define MAXJOBS  64     /* the most processes, and threads in each */
#define PATH_LEN 4200   /* a path under DIR/churn */
#define RING     131072 /* the kernel's log ring, KLOG_RING_SIZE */

static int seconds = 10;
static int procs = 4;
static int threads = 4;
static const char *base = ".";
static char dir[4096];
static volatile long *counts;             /* each process's operations, shared */
static volatile int *stop;                /* set by the parent when the time is up */
static volatile sig_atomic_t interrupted; /* the parent took SIGINT or SIGTERM */
static char ring[RING + 1];

/* A thread's state: its numbers, its seed, the files it holds open to read,
 * and the file it writes, `wfd` and `wname`, with the steps it has taken. */
struct job {
    int proc, thread;
    unsigned int seed;
    int held[HELD];
    int wfd, wsteps;
    char wname[PATH_LEN];
};

/** The next number of a thread's xorshift sequence. */
static unsigned int next_rand(unsigned int *s)
{
    unsigned int x = *s;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/** Report call `what` on `name` that failed with `err`, and end the run. */
static void fail(const char *what, const char *name, int err)
{
    fprintf(stderr, "inodechurnmusl: %s: %s: %s\n", what, name, strerror(err));
    _exit(1);
}

/** Fail unless `rc` is 0 or a race's ENOENT or EEXIST. */
static void expect(int rc, const char *what, const char *name)
{
    if (rc < 0 && errno != ENOENT && errno != EEXIST) {
        fail(what, name, errno);
    }
}

/** Write `len` bytes, at most 512, of `c` to descriptor `fd` of `name`. */
static void fill(int fd, const char *name, size_t len, char c)
{
    char buf[512];

    memset(buf, c, sizeof(buf));
    if (len > sizeof(buf)) {
        len = sizeof(buf);
    }
    if (write(fd, buf, len) != (ssize_t)len) {
        fail("write", name, errno);
    }
}

/**
 * Take one step of the thread's open writer by the random `r`: a new file
 * when it has none, then an append; at the second step it is renamed over a
 * shared name, still open, and a few steps later it is closed, fsynced
 * first now and then.
 */
static void writer_step(struct job *j, unsigned int r, long n)
{
    char other[PATH_LEN];

    if (j->wfd < 0) {
        snprintf(j->wname, sizeof(j->wname), "%s/w%d_%d_%ld", dir, j->proc, j->thread, n);
        j->wfd = open(j->wname, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (j->wfd < 0) {
            fail("open", j->wname, errno);
        }
        j->wsteps = 0;
    }
    fill(j->wfd, j->wname, 64 + (r >> 20) % 200, 'w');
    if (++j->wsteps == 2) {
        snprintf(other, sizeof(other), "%s/c%u", dir, (r >> 3) % SHARED);
        if (rename(j->wname, other) < 0) {
            fail("rename", j->wname, errno);
        }
    } else if (j->wsteps > 3 + (int)((r >> 12) % 4)) {
        if ((r >> 28) == 0 && fsync(j->wfd) < 0) {
            fail("fsync", j->wname, errno);
        }
        close(j->wfd);
        j->wfd = -1;
    }
}

/** Write a file under a name of the thread's own, fsync it now and then,
 * close it and rename it over the shared `name`. */
static void replace_step(struct job *j, unsigned int r, long n, const char *name)
{
    char other[PATH_LEN];
    int fd;

    snprintf(other, sizeof(other), "%s/t%d_%d_%ld", dir, j->proc, j->thread, n);
    fd = open(other, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        fail("open", other, errno);
    }
    fill(fd, other, (r >> 16) % 400, 'r');
    if ((r >> 24) % 10 == 0 && fsync(fd) < 0) {
        fail("fsync", other, errno);
    }
    close(fd);
    if (rename(other, name) < 0) {
        fail("rename", other, errno);
    }
}

/** Open the shared `name`, read it and keep it open in a slot of the
 * thread's, closing what the slot held. */
static void hold_step(struct job *j, unsigned int r, const char *name)
{
    char buf[512];
    int i = (int)((r >> 16) % HELD);
    int fd;

    if (j->held[i] >= 0) {
        close(j->held[i]);
        j->held[i] = -1;
    }
    fd = open(name, O_RDONLY);
    if (fd < 0) {
        expect(fd, "open", name);
        return;
    }
    if (read(fd, buf, sizeof(buf)) < 0) {
        fail("read", name, errno);
    }
    j->held[i] = fd;
}

/** Create the shared `name` in place, emptied, and write a little to it. */
static void truncate_step(unsigned int r, const char *name)
{
    int fd = open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0) {
        expect(fd, "open", name);
        return;
    }
    fill(fd, name, (r >> 16) % 300, 't');
    close(fd);
}

/** Give the shared name `k` a second name of the thread's, and take it away. */
static void link_step(const struct job *j, unsigned int k, const char *name)
{
    char other[PATH_LEN];

    snprintf(other, sizeof(other), "%s/l%u_%d_%d", dir, k, j->proc, j->thread);
    expect(link(name, other), "link", name);
    expect(unlink(other), "unlink", other);
}

/** Close what the thread holds; a writer not yet renamed is unlinked. */
static void job_end(struct job *j)
{
    int i;

    for (i = 0; i < HELD; i++) {
        if (j->held[i] >= 0) {
            close(j->held[i]);
        }
    }
    if (j->wfd >= 0) {
        close(j->wfd);
        if (j->wsteps < 2 && unlink(j->wname) < 0) {
            fail("unlink", j->wname, errno);
        }
    }
}

/** One thread's work, until the parent sets `*stop`. */
static void *churn(void *arg)
{
    struct job *j = arg;
    struct stat st;
    long n;

    for (n = 0; !*stop; n++) {
        char name[PATH_LEN];
        unsigned int r = next_rand(&j->seed);
        unsigned int k = (r >> 8) % SHARED;
        unsigned int op = r % 100;

        snprintf(name, sizeof(name), "%s/c%u", dir, k);
        writer_step(j, r, n);
        if (op < 20) {
            replace_step(j, r, n, name);
        } else if (op < 40) {
            expect(unlink(name), "unlink", name);
        } else if (op < 55) {
            hold_step(j, r, name);
        } else if (op < 65) {
            expect(stat(name, &st), "stat", name);
        } else if (op < 80) {
            truncate_step(r, name);
        } else {
            link_step(j, k, name);
        }
        if ((n & 63) == 63) {
            __atomic_fetch_add(&counts[j->proc], 64, __ATOMIC_RELAXED);
        }
    }
    job_end(j);
    return 0;
}

/** Process `p`'s work: its threads, joined when the parent stops them. */
static void proc_run(int p)
{
    static struct job jobs[MAXJOBS];
    pthread_t t[MAXJOBS];
    int i, rc;

    for (i = 0; i < threads; i++) {
        memset(&jobs[i], 0, sizeof(jobs[i]));
        memset(jobs[i].held, 0xff, sizeof(jobs[i].held));
        jobs[i].proc = p;
        jobs[i].thread = i;
        jobs[i].seed = 2463534242u ^ (unsigned int)(p * 7919 + i * 104729 + 1);
        jobs[i].wfd = -1;
        rc = pthread_create(&t[i], 0, churn, &jobs[i]);
        if (rc != 0) {
            fail("pthread_create", dir, rc);
        }
    }
    for (i = 0; i < threads; i++) {
        pthread_join(t[i], 0);
    }
    _exit(0);
}

/** Remove every name in `dir`, whatever process left it, then `dir`
 * itself; 0, or 1, with a line on stderr, when one could not be removed. */
static int cleanup(void)
{
    char name[sizeof(dir) + 257];
    struct dirent *e;
    DIR *d;
    int bad = 0;

    d = opendir(dir);
    if (d == NULL) {
        fprintf(stderr, "inodechurnmusl: opendir: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        snprintf(name, sizeof(name), "%s/%s", dir, e->d_name);
        if (unlink(name) < 0 && errno != ENOENT) {
            fprintf(stderr, "inodechurnmusl: unlink: %s: %s\n", name, strerror(errno));
            bad = 1;
        }
    }
    closedir(d);
    if (rmdir(dir) < 0) {
        fprintf(stderr, "inodechurnmusl: rmdir: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    return bad;
}

/** 0 when the kernel's log has no free of an inode or block whose bit was
 * clear, and 1, with each such line on stderr, when it has. */
static int log_clean(void)
{
    int n = klogctl(3, ring, RING);
    char *p, *eol;
    int bad = 0;

    if (n < 0) {
        fprintf(stderr, "inodechurnmusl: klogctl: read all: %s\n", strerror(errno));
        return 1;
    }
    ring[n] = '\0';
    for (p = ring; (p = strstr(p, "whose bit is clear")) != NULL; p = eol) {
        eol = p + strcspn(p, "\n");
        while (p > ring && p[-1] != '\n') {
            p--;
        }
        fprintf(stderr, "inodechurnmusl: kernel: %.*s\n", (int)(eol - p), p);
        bad = 1;
    }
    return bad;
}

/** Note SIGINT or SIGTERM, which ends the parent's watch. */
static void on_signal(int signo)
{
    (void)signo;
    interrupted = 1;
}

/**
 * Make the shared page the processes' counts and stop flag are in, and
 * `dir`, and start the processes, whose ids go to `pids`. The parent's
 * signal handler is set first, so that no signal ends it with a process
 * running; a process puts SIGTERM back to its default and ignores SIGINT,
 * which a terminal sends the whole group and the parent answers. Returns
 * the number of processes started, fewer than `procs`, with a line on
 * stderr, when a fork failed.
 */
static int start(pid_t *pids)
{
    struct sigaction sa;
    void *shared;
    int i;

    shared = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) {
        fail("mmap", dir, errno);
    }
    stop = shared;
    counts = (volatile long *)((char *)shared + 64);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, 0) < 0 || sigaction(SIGTERM, &sa, 0) < 0) {
        fail("sigaction", dir, errno);
    }
    if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
        fail("mkdir", dir, errno);
    }
    for (i = 0; i < procs; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            fprintf(stderr, "inodechurnmusl: fork: %s: %s\n", dir, strerror(errno));
            return i;
        }
        if (pids[i] == 0) {
            signal(SIGTERM, SIG_DFL);
            signal(SIGINT, SIG_IGN);
            proc_run(i);
        }
    }
    return procs;
}

/** Print the count of operations once a second for the run's seconds, or
 * until a signal when they are 0; 1 when a process ended or a signal came
 * before they were up, and 0 otherwise. */
static int watch(void)
{
    long total;
    int c, i, status;

    for (c = 1; seconds == 0 || c <= seconds; c++) {
        sleep(1); /* interval: the load runs for its seconds, its count printed each second */
        if (interrupted) {
            if (seconds == 0) {
                return 0;
            }
            fprintf(stderr, "inodechurnmusl: stopped by a signal before the time was up\n");
            return 1;
        }
        total = 0;
        for (i = 0; i < procs; i++) {
            total += counts[i];
        }
        printf("inodechurnmusl: progress: %d s: %ld operations\n", c, total);
        fflush(stdout);
        if (waitpid(-1, &status, WNOHANG) > 0) {
            fprintf(stderr, "inodechurnmusl: a process ended before the time was up\n");
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    pid_t pids[MAXJOBS], w;
    int i, c, status, failed, started;

    while ((c = getopt(argc, argv, "t:p:j:C:")) != -1) {
        switch (c) {
        case 't':
            seconds = atoi(optarg);
            break;
        case 'p':
            procs = atoi(optarg);
            break;
        case 'j':
            threads = atoi(optarg);
            break;
        case 'C':
            base = optarg;
            break;
        default:
            fprintf(stderr,
                    "usage: inodechurnmusl [-t SECONDS] [-p PROCS] [-j THREADS] [-C DIR]\n");
            return 2;
        }
    }
    if (procs < 1 || procs > MAXJOBS || threads < 1 || threads > MAXJOBS || seconds < 0) {
        fprintf(stderr, "inodechurnmusl: counts out of range\n");
        return 2;
    }
    snprintf(dir, sizeof(dir), "%s/churn", base);
    started = start(pids);
    failed = started < procs || watch();
    *stop = 1;
    for (i = 0; i < started; i++) {
        while ((w = waitpid(pids[i], &status, 0)) < 0 && errno == EINTR) {
        }
        if (w == pids[i] && (!WIFEXITED(status) || WEXITSTATUS(status) != 0)) {
            failed = 1;
        }
    }
    failed |= cleanup();
    failed |= log_clean();
    printf("inodechurnmusl: result: %s\n", failed ? "failed" : "ok");
    return failed;
}
