/*
 * hold.c: the POSIX half of `etude lock tree`, on Linux and macOS: the process
 * table, the record lock, the lock's directory and the command's process
 * group.
 */
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#include <libproc.h>
#include <sys/proc.h>
#endif

#include "../hold.h"

/* The first stop signal received, or 0. */
static volatile sig_atomic_t stop_signal;

/* The command's pid and process group once it has one, or 0, and whether
 * a stop has reached its group. */
static volatile sig_atomic_t child;
static volatile sig_atomic_t forwarded;

/* The write end of the pipe the held command waits on, or -1. */
static int go_fd = -1;

long hold_self(void)
{
    return (long)getpid();
}

#ifdef __APPLE__
int hold_read(long pid, struct hold_proc *p)
{
    struct proc_bsdinfo info;

    memset(p, 0, sizeof *p);
    errno = 0;
    if (proc_pidinfo((pid_t)pid, PROC_PIDTBSDINFO, 0, &info, sizeof info) != sizeof info)
        return errno == ESRCH || errno == 0 ? 0 : -1;
    if (info.pbi_status == SZOMB)
        return 0;
    p->pid = pid;
    p->group = (long)info.pbi_pgid;
    p->start = info.pbi_start_tvsec;
    p->fraction = info.pbi_start_tvusec;
    return 1;
}

long hold_parent(long pid)
{
    struct proc_bsdinfo info;

    if (proc_pidinfo((pid_t)pid, PROC_PIDTBSDINFO, 0, &info, sizeof info) != sizeof info)
        return 0;
    return (long)info.pbi_ppid;
}

size_t hold_members(long group, long *pids, size_t max)
{
    pid_t *list;
    int bytes, i;
    size_t n = 0;

    if (group <= 0)
        return 0;
    bytes = proc_listpids(PROC_PGRP_ONLY, (uint32_t)group, NULL, 0);
    if (bytes <= 0 || (list = malloc((size_t)bytes)) == NULL)
        return 0;
    bytes = proc_listpids(PROC_PGRP_ONLY, (uint32_t)group, list, bytes);
    for (i = 0; bytes > 0 && i < bytes / (int)sizeof(pid_t); i++) {
        struct hold_proc p;

        if (list[i] == 0 || list[i] == getpid() || hold_read(list[i], &p) != 1 || p.group != group)
            continue;
        if (n < max)
            pids[n] = (long)list[i];
        n++;
    }
    free(list);
    return n;
}
#else
/**
 * Read the fields of /proc/PID/stat after the command's closing parenthesis:
 * the state, the parent, the group and, the 20th after the state, the start.
 * 1, 0 for no such process, -1 when it cannot be read.
 */
static int stat_of(long pid, char *state, long *parent, long *group, unsigned long long *start)
{
    char path[64], buf[1024], *p;
    size_t n;
    FILE *f;
    int i;

    snprintf(path, sizeof path, "/proc/%ld/stat", pid);
    if ((f = fopen(path, "r")) == NULL)
        return errno == ENOENT || errno == ESRCH ? 0 : -1;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    p = strrchr(buf, ')');
    if (p == NULL || sscanf(p + 1, " %c %ld %ld", state, parent, group) != 3)
        return -1;
    for (p += 2, i = 0; i < 19 && p != NULL; i++)
        p = strchr(p + 1, ' ');
    if (p == NULL || sscanf(p, " %llu", start) != 1)
        return -1;
    return 1;
}

int hold_read(long pid, struct hold_proc *p)
{
    char state;
    long parent;
    int found;

    memset(p, 0, sizeof *p);
    found = stat_of(pid, &state, &parent, &p->group, &p->start);
    if (found != 1)
        return found;
    if (state == 'Z' || state == 'X')
        return 0;
    p->pid = pid;
    return 1;
}

long hold_parent(long pid)
{
    char state;
    long parent, group;
    unsigned long long start;

    return stat_of(pid, &state, &parent, &group, &start) == 1 ? parent : 0;
}

size_t hold_members(long group, long *pids, size_t max)
{
    DIR *d;
    struct dirent *e;
    size_t n = 0;

    if (group <= 0 || (d = opendir("/proc")) == NULL)
        return 0;
    while ((e = readdir(d)) != NULL) {
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        struct hold_proc p;

        if (*end != '\0' || pid <= 0 || pid == (long)getpid())
            continue;
        if (hold_read(pid, &p) != 1 || p.group != group)
            continue;
        if (n < max)
            pids[n] = pid;
        n++;
    }
    closedir(d);
    return n;
}
#endif

int hold_in_job(const struct hold_proc *owner)
{
    (void)owner;
    return 0;
}

int hold_guard(const char *path)
{
    struct flock lock;
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);

    if (fd < 0)
        return -1;
    memset(&lock, 0, sizeof lock);
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &lock) == 0)
        return 0;
    if (errno == EACCES || errno == EAGAIN) {
        close(fd);
        return 1;
    }
    close(fd);
    return -1;
}

int hold_mkdir(const char *path)
{
    return mkdir(path, 0777);
}

int hold_rename(const char *from, const char *to)
{
    return rename(from, to);
}

FILE *hold_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

int hold_remove(const char *path)
{
    struct stat st;
    DIR *d;
    struct dirent *e;
    int bad = 0;

    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
    if ((d = opendir(path)) != NULL) {
        while ((e = readdir(d)) != NULL) {
            char *sub;

            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                continue;
            if ((sub = malloc(strlen(path) + strlen(e->d_name) + 2)) == NULL) {
                bad = 1;
                continue;
            }
            sprintf(sub, "%s/%s", path, e->d_name);
            bad |= hold_remove(sub) != 0;
            free(sub);
        }
        closedir(d);
    }
    return rmdir(path) == 0 && !bad ? 0 : -1;
}

int hold_young(const char *path, double seconds)
{
    struct stat st;

    return stat(path, &st) == 0 && difftime(time(NULL), st.st_mtime) < seconds;
}

/** Note the first stop, and pass it to the command's group once as TERM. */
static void on_stop(int sig)
{
    if (stop_signal == 0)
        stop_signal = sig;
    if (child > 0 && !forwarded) {
        forwarded = 1;
        kill(-(pid_t)child, SIGTERM);
    }
}

/** Give the stop signals the handler `fn`, or their default with SIG_DFL. */
static void set_stops(void (*fn)(int))
{
    static const int stops[] = {SIGTERM, SIGINT, SIGHUP};
    struct sigaction sa;
    size_t i;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof stops / sizeof stops[0]; i++)
        sigaction(stops[i], &sa, NULL);
}

void hold_catch(void)
{
    set_stops(on_stop);
}

int hold_stopped(void)
{
    return stop_signal;
}

int hold_start(const char *name, char *const *argv, long *group)
{
    sigset_t stops, saved;
    int go[2], err;
    pid_t pid;
    char byte;

    if (pipe(go) != 0)
        return -1;
    fcntl(go[0], F_SETFD, FD_CLOEXEC);
    fcntl(go[1], F_SETFD, FD_CLOEXEC);
    sigemptyset(&stops);
    sigaddset(&stops, SIGTERM);
    sigaddset(&stops, SIGINT);
    sigaddset(&stops, SIGHUP);
    sigprocmask(SIG_BLOCK, &stops, &saved);
    pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        set_stops(SIG_DFL);
        sigprocmask(SIG_SETMASK, &saved, NULL);
        close(go[1]);
        while (read(go[0], &byte, 1) < 0 && errno == EINTR)
            ;
        execvp(argv[0], argv);
        fprintf(stderr, "%s: cannot run %s: %s\n", name, argv[0], strerror(errno));
        _exit(127);
    }
    err = errno;
    close(go[0]);
    if (pid < 0) {
        close(go[1]);
        sigprocmask(SIG_SETMASK, &saved, NULL);
        errno = err;
        return -1;
    }
    setpgid(pid, pid);
    child = (sig_atomic_t)pid;
    go_fd = go[1];
    if (stop_signal != 0 && !forwarded) {
        forwarded = 1;
        kill(-pid, SIGTERM);
    }
    sigprocmask(SIG_SETMASK, &saved, NULL);
    *group = (long)pid;
    return 0;
}

void hold_go(void)
{
    if (go_fd >= 0)
        close(go_fd);
    go_fd = -1;
}

void hold_cancel(void)
{
    if (child > 0) {
        kill((pid_t)child, SIGKILL);
        while (waitpid((pid_t)child, NULL, 0) < 0 && errno == EINTR)
            ;
    }
    hold_go();
}

int hold_wait(void)
{
    int status;

    while (waitpid((pid_t)child, &status, 0) < 0)
        if (errno != EINTR)
            return 127;
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return WEXITSTATUS(status);
}

void hold_exec(char *const *argv)
{
    execvp(argv[0], argv);
}
