/*
 * process.c: the process helpers of the POSIX executor: the identity and
 * state of a process, the children and the descriptors of this one, and the
 * messages between the controller and a keeper.
 *
 * Linux and Rach2 share one adapter, since Rach2 has Linux's /proc files and
 * its prctl(PR_SET_CHILD_SUBREAPER). Rach2's TinyCC defines no __linux__, so
 * a build for Rach2 defines __rach2__, as bootstrap.sh's recipe does.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__) || defined(__rach2__)
#define ETUDE_PROC_LINUX 1
#include <sys/prctl.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

#include "posix.h"

extern char **environ;

void posix_untrack_runner(void)
{
    static const char key[] = "RUNNER_TRACKING_ID=";
    char **p;

    for (p = environ; *p != NULL; p++)
        if (strncmp(*p, key, sizeof(key) - 1) == 0) {
            char *value = *p + sizeof(key) - 1;
            memset(value, 0, strlen(value));
        }
}

uint64_t posix_now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

int keeper_send(int fd, int type, int64_t a, int64_t b, int64_t c)
{
    struct keeper_msg m;
    ssize_t n;

    memset(&m, 0, sizeof(m));
    m.type = type;
    m.a = a;
    m.b = b;
    m.c = c;
    do {
        n = write(fd, &m, sizeof(m));
    } while (n < 0 && errno == EINTR);
    return n == (ssize_t)sizeof(m) ? 0 : -1;
}

int keeper_recv(int fd, struct keeper_msg *m)
{
    size_t got = 0;
    ssize_t n;

    while (got < sizeof(*m)) {
        n = read(fd, (char *)m + got, sizeof(*m) - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        got += (size_t)n;
    }
    return 0;
}

int posix_fd_high(int fd)
{
    int high = fcntl(fd, F_DUPFD_CLOEXEC, 3);

    if (high >= 0)
        close(fd);
    return high;
}

/** Say whether `fd` is one of the `n` descriptors in `keep`. */
static int kept(const int *keep, int n, int fd)
{
    int i;

    for (i = 0; i < n; i++)
        if (keep[i] == fd)
            return 1;
    return 0;
}

/**
 * Close the descriptors that /dev/fd lists, except those kept.
 *
 * @return 0, or -1 when the system has no such directory.
 */
static int close_listed(const int *keep, int n)
{
    int fds[1024];
    int count, more = 1, i;
    struct dirent *e;
    DIR *d;

    while (more) {
        d = opendir("/dev/fd");
        if (d == NULL)
            return -1;
        count = 0;
        more = 0;
        while ((e = readdir(d)) != NULL) {
            int fd = atoi(e->d_name);
            if (e->d_name[0] < '0' || e->d_name[0] > '9' || fd == dirfd(d) || kept(keep, n, fd))
                continue;
            if (count == (int)(sizeof(fds) / sizeof(fds[0])))
                more = 1;
            else
                fds[count++] = fd;
        }
        closedir(d);
        for (i = 0; i < count; i++)
            close(fds[i]);
    }
    return 0;
}

void posix_close_others(const int *keep, int n)
{
    struct rlimit rl;
    long max = 65536, fd;

    if (close_listed(keep, n) == 0)
        return;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY &&
        rl.rlim_cur < (rlim_t)max)
        max = (long)rl.rlim_cur;
    for (fd = 0; fd < max; fd++)
        if (!kept(keep, n, (int)fd))
            close((int)fd);
}

/** Close a proc stream, preserving read errors across fclose. */
int posix_proc_read(FILE *f, char *buf, size_t size)
{
    size_t n;
    int error, failed;

    errno = 0;
    n = fread(buf, 1, size - 1, f);
    error = errno;
    failed = ferror(f);
    if (fclose(f) != 0 && !failed)
        return -1;
    if (failed) {
        errno = error ? error : EIO;
        return errno == ENOENT || errno == ESRCH ? 0 : -1;
    }
    buf[n] = '\0';
    return 1;
}

#if defined(ETUDE_PROC_LINUX)

int posix_subreaper(void)
{
    return prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
}

int posix_children(long *pids, int max)
{
    char path[64];
    FILE *f;
    long pid;
    int n = 0;

    snprintf(path, sizeof(path), "/proc/self/task/%ld/children", (long)getpid());
    f = fopen(path, "r");
    if (f == NULL)
        return -1;
    while (n < max && fscanf(f, "%ld", &pid) == 1)
        pids[n++] = pid;
    fclose(f);
    return n;
}

/**
 * Read the state and start of `pid` from /proc/PID/stat: the state is the
 * field after the command's closing parenthesis, and the start the 20th
 * after it.
 */
int etude_process_state(long pid, struct etude_pid *id)
{
    char path[64], buf[1024], *p;
    unsigned long long start;
    FILE *f;
    int i, read;

    snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
    f = fopen(path, "r");
    if (f == NULL)
        return errno == ENOENT || errno == ESRCH ? 0 : -1;
    read = posix_proc_read(f, buf, sizeof(buf));
    if (read != 1)
        return read;
    p = strrchr(buf, ')');
    if (p == NULL || p[1] != ' ') {
        errno = EINVAL;
        return -1;
    }
    p += 2;
    if (*p == 'Z' || *p == 'X')
        return 0;
    for (i = 0; i < 19 && p != NULL; i++)
        p = strchr(p + 1, ' ');
    if (p == NULL || sscanf(p, " %llu", &start) != 1) {
        errno = EINVAL;
        return -1;
    }
    id->pid = pid;
    id->start = start;
    id->start_known = 1;
    return 1;
}

/**
 * Read the processor time and resident memory of process `id` from
 * /proc/PID/stat: its user and system time and those of the children it
 * has reaped, fields 14 to 17 of proc(5), and its resident pages, field 24,
 * once its start, field 22, is still the one `id` gives.
 */
int posix_usage(const struct etude_pid *id, uint64_t *cpu_us, uint64_t *rss_bytes)
{
    unsigned long long v[21];
    long tick = sysconf(_SC_CLK_TCK), page = sysconf(_SC_PAGESIZE);
    char path[64], buf[1024], *p;
    FILE *f;
    int i, read;

    snprintf(path, sizeof(path), "/proc/%ld/stat", id->pid);
    f = fopen(path, "r");
    if (f == NULL)
        return errno == ENOENT || errno == ESRCH ? 0 : -1;
    read = posix_proc_read(f, buf, sizeof(buf));
    if (read != 1)
        return read;
    p = strrchr(buf, ')');
    if (p == NULL || p[1] != ' ' || tick <= 0 || page <= 0)
        return -1;
    p += 2;
    for (i = 0; i < 21; i++) {
        p = strchr(p, ' ');
        if (p == NULL || sscanf(p, " %llu", &v[i]) != 1)
            return -1;
        p++;
    }
    if (!id->start_known || v[18] != id->start)
        return 0;
    *cpu_us = (uint64_t)(v[10] + v[11] + v[12] + v[13]) * 1000000u / (uint64_t)tick;
    *rss_bytes = (uint64_t)v[20] * (uint64_t)page;
    return 1;
}

#else

int posix_usage(const struct etude_pid *id, uint64_t *cpu_us, uint64_t *rss_bytes)
{
    (void)id;
    (void)cpu_us;
    (void)rss_bytes;
    errno = ENOSYS;
    return -1;
}

int posix_subreaper(void)
{
    errno = ENOSYS;
    return -1;
}

int posix_children(long *pids, int max)
{
    (void)pids;
    (void)max;
    return -1;
}

#if defined(__APPLE__)

int etude_process_state(long pid, struct etude_pid *id)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, 0};
    struct kinfo_proc kp;
    size_t len = sizeof(kp);

    mib[3] = (int)pid;
    if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0)
        return -1;
    if (len == 0 || kp.kp_proc.p_stat == SZOMB)
        return 0;
    id->pid = pid;
    id->start = (uint64_t)kp.kp_proc.p_starttime.tv_sec * 1000000u +
                (uint64_t)kp.kp_proc.p_starttime.tv_usec;
    id->start_known = 1;
    return 1;
}

#else

int etude_process_state(long pid, struct etude_pid *id)
{
    if (kill((pid_t)pid, 0) != 0 && errno == ESRCH)
        return 0;
    (void)id;
    return -1;
}

#endif
#endif
