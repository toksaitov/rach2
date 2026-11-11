/*
 * threadwatch.c: log each change of the scheduler state, wait channel and
 * system call of the threads of named programs, and of chosen files, at an
 * interval of about a millisecond, to place a stall of a graphics program.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_THREADS  2048
#define MAX_FILES    16
#define MAX_PROGRAMS 16
#define TEXT         4096

/* One watched thread and what it showed at the last pass. */
struct thread {
    int pid, tid;
    unsigned gen;  /* the scan that last found it */
    char comm[32]; /* the thread's own name */
    char state;    /* stat's state letter, 0 before the first read */
    long cpu;      /* utime + stime, in clock ticks */
    char wchan[64];
    char call[160]; /* the syscall file's text */
    size_t key;     /* the length of call that a change is judged by */
};

/* One watched file; at `@word` only its lines holding the word. */
struct file {
    char path[256];
    const char *word;
    char last[TEXT];
};

/* A program to watch: threads of processes of this name, and, when arg is
 * not empty, whose command line holds arg. */
struct program {
    char comm[32];
    const char *arg;
};

static struct thread threads[MAX_THREADS];
static int nthreads;
static struct file files[MAX_FILES];
static int nfiles;
static struct program programs[MAX_PROGRAMS];
static int nprograms;
static volatile sig_atomic_t stop;

/* The log, kept in memory and written at the end so that the sampler does
 * not write to a disk while it samples. */
static char *logbuf;
static size_t loglen, logcap;
static int logfull;

/**
 * Append one formatted line to the log, dropping lines once 1 GiB is held.
 */
static void note(const char *fmt, ...)
{
    char line[TEXT + 512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0 || logfull)
        return;
    if ((size_t)n >= sizeof line)
        n = sizeof line - 1;
    if (loglen + (size_t)n > logcap) {
        size_t cap = logcap ? logcap * 2 : (size_t)16 << 20;
        char *p;

        if (cap > (size_t)1 << 30 || (p = realloc(logbuf, cap)) == NULL) {
            logfull = 1;
            return;
        }
        logbuf = p;
        logcap = cap;
    }
    memcpy(logbuf + loglen, line, (size_t)n);
    loglen += (size_t)n;
}

/** Ask the sampling loop to end and write its log. */
static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

/** Return CLOCK_MONOTONIC in nanoseconds. */
static long long now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/**
 * Read up to `cap - 1` bytes of a file into `buf`, NUL-terminated, and
 * return the length, or -1 when it cannot be opened or read.
 */
static int slurp(const char *path, char *buf, size_t cap)
{
    ssize_t n;
    size_t got = 0;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return -1;
    while (got + 1 < cap && (n = read(fd, buf + got, cap - 1 - got)) > 0)
        got += (size_t)n;
    close(fd);
    buf[got] = 0;
    return (int)got;
}

/** Cut trailing white space and turn newlines into " ; ". */
static void flatten(char *s, size_t cap)
{
    char out[TEXT];
    size_t i, j = 0;
    size_t n = strlen(s);

    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = 0;
    for (i = 0; i < n && j + 4 < sizeof out; i++) {
        if (s[i] == '\n') {
            memcpy(out + j, " ; ", 3);
            j += 3;
        } else {
            out[j++] = s[i];
        }
    }
    out[j] = 0;
    snprintf(s, cap, "%s", out);
}

/** Say whether process `pid` is one of the programs to watch. */
static int wanted(int pid)
{
    char path[64], comm[64], cmd[TEXT];
    int i, n, cmdlen = -1;

    snprintf(path, sizeof path, "/proc/%d/comm", pid);
    if (slurp(path, comm, sizeof comm) <= 0)
        return 0;
    flatten(comm, sizeof comm);
    for (i = 0; i < nprograms; i++) {
        if (strcmp(comm, programs[i].comm) != 0)
            continue;
        if (!programs[i].arg[0])
            return 1;
        if (cmdlen < 0) {
            snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
            cmdlen = slurp(path, cmd, sizeof cmd);
            for (n = 0; n < cmdlen; n++)
                if (!cmd[n])
                    cmd[n] = ' ';
        }
        if (cmdlen > 0 && strstr(cmd, programs[i].arg))
            return 1;
    }
    return 0;
}

/**
 * Find the threads of the watched programs, adding new ones and dropping
 * those gone, and log each arrival and departure at time `t`.
 */
static void scan(unsigned gen, long long t)
{
    DIR *proc, *task;
    struct dirent *p, *e;
    char path[64];
    int i, pid, tid;

    if ((proc = opendir("/proc")) == NULL)
        return;
    while ((p = readdir(proc)) != NULL) {
        if (!isdigit((unsigned char)p->d_name[0]))
            continue;
        pid = atoi(p->d_name);
        if (!wanted(pid))
            continue;
        snprintf(path, sizeof path, "/proc/%d/task", pid);
        if ((task = opendir(path)) == NULL)
            continue;
        while ((e = readdir(task)) != NULL) {
            if (!isdigit((unsigned char)e->d_name[0]))
                continue;
            tid = atoi(e->d_name);
            for (i = 0; i < nthreads; i++)
                if (threads[i].tid == tid && threads[i].pid == pid)
                    break;
            if (i == nthreads) {
                if (nthreads == MAX_THREADS)
                    continue;
                memset(&threads[i], 0, sizeof threads[i]);
                threads[i].pid = pid;
                threads[i].tid = tid;
                snprintf(path, sizeof path, "/proc/%d/task/%d/comm", pid, tid);
                slurp(path, threads[i].comm, sizeof threads[i].comm);
                flatten(threads[i].comm, sizeof threads[i].comm);
                nthreads++;
                note("N %lld.%09lld %d %d %s\n", t / 1000000000LL, t % 1000000000LL, pid, tid,
                     threads[i].comm);
            }
            threads[i].gen = gen;
        }
        closedir(task);
    }
    closedir(proc);
    for (i = 0; i < nthreads; i++) {
        if (threads[i].gen == gen)
            continue;
        note("X %lld.%09lld %d %d %s\n", t / 1000000000LL, t % 1000000000LL, threads[i].pid,
             threads[i].tid, threads[i].comm);
        threads[i--] = threads[--nthreads];
    }
}

/**
 * Return how much of a syscall file's text a change is judged by: all of a
 * call with its arguments, and none of the stack and instruction pointers,
 * which move while a thread runs outside a call.
 */
static size_t callkey(const char *s)
{
    const char *sp;
    size_t n = strlen(s);

    if (strncmp(s, "running", 7) == 0)
        return n;
    /* Drop the last two fields, SP and PC, in both Linux forms. */
    if ((sp = strrchr(s, ' ')) == NULL)
        return n;
    n = (size_t)(sp - s);
    while (n && s[n - 1] != ' ')
        n--;
    return n ? n - 1 : 0;
}

/** Read one thread's three files and log a change at time `t`. */
static void sample(struct thread *th, long long t)
{
    char path[64], buf[TEXT], wchan[64], call[160];
    char *s, state;
    long utime = 0, stime = 0;
    int field;
    size_t key;

    snprintf(path, sizeof path, "/proc/%d/task/%d/stat", th->pid, th->tid);
    if (slurp(path, buf, sizeof buf) <= 0 || (s = strrchr(buf, ')')) == NULL)
        return;
    state = s[2];
    /* After the name: state is field 3, utime 14 and stime 15. */
    for (field = 3, s += 2; *s && field <= 15; field++) {
        if (field == 14)
            utime = strtol(s, NULL, 10);
        if (field == 15)
            stime = strtol(s, NULL, 10);
        while (*s && *s != ' ')
            s++;
        while (*s == ' ')
            s++;
    }
    snprintf(path, sizeof path, "/proc/%d/task/%d/wchan", th->pid, th->tid);
    if (slurp(path, wchan, sizeof wchan) < 0)
        strcpy(wchan, "?");
    flatten(wchan, sizeof wchan);
    snprintf(path, sizeof path, "/proc/%d/task/%d/syscall", th->pid, th->tid);
    if (slurp(path, call, sizeof call) < 0)
        strcpy(call, "?");
    flatten(call, sizeof call);
    key = callkey(call);
    if (state == th->state && strcmp(wchan, th->wchan) == 0 && key == th->key &&
        strncmp(call, th->call, key) == 0)
        return;
    th->state = state;
    th->cpu = utime + stime;
    snprintf(th->wchan, sizeof th->wchan, "%s", wchan);
    snprintf(th->call, sizeof th->call, "%s", call);
    th->key = key;
    note("T %lld.%09lld %d %d %s %c %ld %s | %s\n", t / 1000000000LL, t % 1000000000LL, th->pid,
         th->tid, th->comm, state, th->cpu, wchan, call);
}

/** Read one watched file and log it at time `t` when it changed. */
static void watch_file(struct file *f, long long t)
{
    char buf[TEXT], out[TEXT];
    char *line, *next;
    size_t j = 0;

    if (slurp(f->path, buf, sizeof buf) < 0)
        strcpy(buf, "?");
    if (f->word) {
        out[0] = 0;
        for (line = buf; line && *line; line = next) {
            if ((next = strchr(line, '\n')) != NULL)
                *next++ = 0;
            if (strstr(line, f->word) && j + strlen(line) + 2 < sizeof out) {
                j += (size_t)sprintf(out + j, "%s\n", line);
            }
        }
        snprintf(buf, sizeof buf, "%s", out);
    }
    flatten(buf, sizeof buf);
    if (strcmp(buf, f->last) == 0)
        return;
    snprintf(f->last, sizeof f->last, "%s", buf);
    note("F %lld.%09lld %s %s\n", t / 1000000000LL, t % 1000000000LL, f->path, buf);
}

/** Print the usage line and exit 2. */
static void usage(void)
{
    fprintf(stderr, "usage: threadwatch [-i USEC] [-r MSEC] [-f FILE[@WORD]]... "
                    "OUT SECONDS NAME[:ARG]...\n");
    exit(2);
}

/**
 * Sample the named programs' threads for SECONDS or until a signal, then
 * write the log to OUT; exit 0 once it is written, 1 when it cannot be.
 */
int main(int argc, char **argv)
{
    long interval = 1000, rescan = 20;
    long long t0, t, end, last_scan = 0, pass, longest = 0, longest_at = 0;
    long long total = 0, passes = 0;
    struct timespec mono, real, nap;
    struct sigaction sa;
    unsigned gen = 0;
    const char *out;
    char *colon;
    FILE *fp;
    int opt, i;

    while ((opt = getopt(argc, argv, "i:r:f:")) != -1) {
        switch (opt) {
        case 'i':
            interval = atol(optarg);
            break;
        case 'r':
            rescan = atol(optarg);
            break;
        case 'f':
            if (nfiles == MAX_FILES)
                usage();
            snprintf(files[nfiles].path, sizeof files[nfiles].path, "%s", optarg);
            if ((colon = strchr(files[nfiles].path, '@')) != NULL) {
                *colon = 0;
                files[nfiles].word = optarg + (colon - files[nfiles].path) + 1;
            }
            nfiles++;
            break;
        default:
            usage();
        }
    }
    if (argc - optind < 3 || argc - optind - 2 > MAX_PROGRAMS || interval <= 0)
        usage();
    out = argv[optind];
    end = atol(argv[optind + 1]) * 1000000000LL;
    for (i = optind + 2; i < argc; i++) {
        snprintf(programs[nprograms].comm, sizeof programs[nprograms].comm, "%s", argv[i]);
        programs[nprograms].arg = "";
        if ((colon = strchr(programs[nprograms].comm, ':')) != NULL) {
            *colon = 0;
            programs[nprograms].arg = argv[i] + (colon - programs[nprograms].comm) + 1;
        }
        nprograms++;
    }
    if ((fp = fopen(out, "w")) == NULL) {
        fprintf(stderr, "threadwatch: %s: %s\n", out, strerror(errno));
        return 1;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    note("# threadwatch: monotonic %lld.%09ld realtime %lld.%09ld interval %ld us\n",
         (long long)mono.tv_sec, mono.tv_nsec, (long long)real.tv_sec, real.tv_nsec, interval);
    t0 = now_ns();
    end += t0;
    nap.tv_sec = interval / 1000000;
    nap.tv_nsec = (interval % 1000000) * 1000;
    while (!stop && (t = now_ns()) < end) {
        if (t - last_scan >= rescan * 1000000LL) {
            scan(++gen, t);
            last_scan = t;
        }
        for (i = 0; i < nthreads; i++)
            sample(&threads[i], t);
        for (i = 0; i < nfiles; i++)
            watch_file(&files[i], t);
        pass = now_ns() - t;
        total += pass;
        passes++;
        if (pass > longest) {
            longest = pass;
            longest_at = t;
        }
        /* A pass much longer than the interval is a stall of the sampler
         * itself, which a stall of the whole machine also shows. */
        if (pass > 20LL * interval * 1000)
            note("P %lld.%09lld %lld us\n", t / 1000000000LL, t % 1000000000LL, pass / 1000);
        nanosleep(&nap, NULL);
    }
    t = now_ns();
    note("# threadwatch: %lld passes in %lld ms, mean pass %lld us, longest %lld us "
         "at %lld.%09lld, %d threads%s\n",
         passes, (t - t0) / 1000000, passes ? total / passes / 1000 : 0, longest / 1000,
         longest_at / 1000000000LL, longest_at % 1000000000LL, nthreads,
         logfull ? ", log full" : "");
    if (loglen && fwrite(logbuf, 1, loglen, fp) != loglen) {
        fprintf(stderr, "threadwatch: %s: %s\n", out, strerror(errno));
        return 1;
    }
    if (fclose(fp) != 0) {
        fprintf(stderr, "threadwatch: %s: %s\n", out, strerror(errno));
        return 1;
    }
    return 0;
}
