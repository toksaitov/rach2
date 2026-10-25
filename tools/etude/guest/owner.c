/* Keep detached descendants below a distinct subreaper for each attempt. */
#define _POSIX_C_SOURCE 200809L
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#define _DARWIN_C_SOURCE
#include "owner.h"
#include "../posix/posix.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct owner_message {
    int kind, status, clean;
    pid_t child;
    unsigned long long start;
    struct rusage usage;
};

#define OWNER_START 1
#define OWNER_END   2
#define OWNER_CLEAN 3
#if defined(__linux__) || defined(__rach2__)
static volatile sig_atomic_t owner_stopping;

static void owner_stop(int signo)
{
    owner_stopping = signo;
}

static int message_write(int fd, const struct owner_message *message)
{
    ssize_t n;
    do
        n = write(fd, message, sizeof(*message));
    while (n < 0 && errno == EINTR);
    return n == (ssize_t)sizeof(*message) ? 0 : -1;
}

static int message_read(int fd, struct owner_message *message)
{
    size_t used = 0;
    ssize_t n;
    while (used < sizeof(*message)) {
        n = read(fd, (char *)message + used, sizeof(*message) - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        used += (size_t)n;
    }
    return 0;
}

static uint64_t owner_now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        return 0;
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}

static void owner_usage(struct rusage *sum, const struct rusage *value)
{
    sum->ru_utime.tv_sec += value->ru_utime.tv_sec;
    sum->ru_utime.tv_usec += value->ru_utime.tv_usec;
    sum->ru_utime.tv_sec += sum->ru_utime.tv_usec / 1000000;
    sum->ru_utime.tv_usec %= 1000000;
    sum->ru_stime.tv_sec += value->ru_stime.tv_sec;
    sum->ru_stime.tv_usec += value->ru_stime.tv_usec;
    sum->ru_stime.tv_sec += sum->ru_stime.tv_usec / 1000000;
    sum->ru_stime.tv_usec %= 1000000;
    if (value->ru_maxrss > sum->ru_maxrss)
        sum->ru_maxrss = value->ru_maxrss;
}

static void owner_signal(pid_t child, unsigned long long start, int signo)
{
    struct etude_pid now;
    long children[64];
    int n, i, state = etude_process_state(child, &now);
    if (state == 1 && now.start == start)
        kill(-child, signo);
    n = posix_children(children, 64);
    for (i = 0; i < n; i++) {
        kill((pid_t)children[i], signo);
        kill((pid_t)-children[i], signo);
    }
}
/* Close the inherited responder/other-attempt pipes in this owner only. */
static void owner_fds(int control, int events)
{
    int keep[2] = {control, events};
    posix_close_others(keep, 2);
}
struct owner_loop {
    pid_t child;
    unsigned long long start;
    int control, events, ended;
    uint64_t grace, kill_at;
    struct owner_message result;
    const char *directory;
};

static int owner_reap(struct owner_loop *o)
{
    struct rusage usage;
    pid_t pid;
    int status;
    while ((pid = wait4(-1, &status, WNOHANG, &usage)) > 0) {
        owner_usage(&o->result.usage, &usage);
        if (pid == o->child) {
            o->ended = 1;
            o->result.status = status;
            o->result.kind = OWNER_END;
            message_write(o->events, &o->result);
        }
    }
    return pid < 0 && errno == ECHILD;
}

static void owner_control(struct owner_loop *o)
{
    struct pollfd p = {o->control, POLLIN, 0};
    unsigned char command;
    ssize_t n;
    if (poll(&p, 1, 20) > 0) {
        n = read(o->control, &command, 1);
        if (!n || (n < 0 && errno != EINTR)) {
            close(o->control);
            o->control = -1;
            owner_stopping = SIGTERM;
        } else if (n == 1 && command == SIGTERM)
            owner_stopping = SIGTERM;
        else if (n == 1 && command == SIGKILL)
            owner_stopping = SIGKILL;
    }
    if (owner_stopping && !o->kill_at) {
        owner_signal(o->child, o->start, SIGTERM);
        owner_signal(o->child, o->start, SIGCONT);
        o->kill_at = owner_now() + o->grace;
        if (!owner_now())
            owner_stopping = SIGKILL;
    }
}
/* Publish only after wait4 has reaped every descendant. A later responder
 * needs this proof when the original responder died before reading CLEAN. */
static int owner_proof(const char *directory)
{
    struct etude_pid identity;
    char path[4096], text[128];
    int fd, n, ok;
    if (etude_process_state(getpid(), &identity) != 1 ||
        snprintf(path, sizeof(path), "%s/owned-cleaned", directory) >= (int)sizeof(path))
        return -1;
    n = snprintf(text, sizeof(text), "%ld %llu\n", (long)getpid(),
                 (unsigned long long)identity.start);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    ok = fchmod(fd, 0600) == 0 && write(fd, text, (size_t)n) == n && fsync(fd) == 0;
    if (close(fd) != 0)
        ok = 0;
    if (!ok)
        unlink(path);
    return ok ? 0 : -1;
}

static void owner_supervise(struct owner_loop *o)
{
    for (;;) {
        int empty = owner_reap(o);
        if (o->ended && empty)
            break;
        owner_control(o);
        if (o->ended || owner_stopping == SIGKILL || (o->kill_at && owner_now() >= o->kill_at))
            owner_signal(o->child, o->start, SIGKILL);
    }
    o->result.kind = OWNER_CLEAN;
    o->result.clean = owner_proof(o->directory) == 0;
    message_write(o->events, &o->result);
    _exit(0);
}
/* This process is dedicated to one attempt, so every adopted child is owned. */
static pid_t owner_child(int control[2], int events[2], uint64_t grace, const char *directory)
{
    struct owner_loop o;
    struct etude_pid identity;
    struct sigaction action;
    memset(&o, 0, sizeof(o));
    owner_stopping = 0;
    o.directory = directory;
    close(control[1]);
    close(events[0]);
    setsid();
    memset(&action, 0, sizeof(action));
    action.sa_handler = owner_stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    if (posix_subreaper() != 0)
        _exit(127);
    o.child = fork();
    if (!o.child) {
        close(control[0]);
        close(events[1]);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        return 0;
    }
    if (o.child < 0 || etude_process_state(o.child, &identity) != 1) {
        if (o.child > 0)
            kill(o.child, SIGKILL);
        while (waitpid(-1, NULL, 0) > 0 || errno == EINTR) {
        }
        _exit(127);
    }
    o.start = identity.start;
    o.control = control[0];
    o.events = events[1];
    o.grace = grace;
    o.result.kind = OWNER_START;
    o.result.child = o.child;
    o.result.start = o.start;
    if (message_write(o.events, &o.result) != 0)
        owner_stopping = SIGTERM;
    owner_fds(o.control, o.events);
    owner_supervise(&o);
    return -1;
}

#endif

pid_t guest_owner_fork(struct guest_owner *owner, uint64_t grace_us, const char *directory)
{
#if defined(__linux__) || defined(__rach2__)
    int control[2] = {-1, -1}, events[2] = {-1, -1};
    struct owner_message message;
    struct etude_pid identity;
    pid_t pid, child;
    memset(owner, 0, sizeof(*owner));
    owner->control = owner->events = -1;
    if (pipe(control) || pipe(events))
        goto failed;
    if (fcntl(control[1], F_SETFD, FD_CLOEXEC) < 0 || fcntl(events[0], F_SETFD, FD_CLOEXEC) < 0)
        goto failed;
    pid = fork();
    if (!pid)
        return owner_child(control, events, grace_us, directory);
    if (pid < 0)
        goto failed;
    close(control[0]);
    control[0] = -1;
    close(events[1]);
    events[1] = -1;
    if (message_read(events[0], &message) || message.kind != OWNER_START || message.child <= 0 ||
        !message.start || etude_process_state(pid, &identity) != 1) {
        close(control[1]);
        control[1] = -1;
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        }
        goto failed;
    }
    owner->enabled = 1;
    owner->pid = pid;
    owner->start = identity.start;
    owner->control = control[1];
    owner->events = events[0];
    if (fcntl(owner->events, F_SETFL, O_NONBLOCK) < 0) {
        guest_owner_signal(owner, SIGKILL);
        guest_owner_wait(owner, grace_us + 1000000);
        guest_owner_close(owner);
        return -1;
    }
    child = message.child;
    return child;
failed:
    if (control[0] >= 0)
        close(control[0]);
    if (control[1] >= 0)
        close(control[1]);
    if (events[0] >= 0)
        close(events[0]);
    if (events[1] >= 0)
        close(events[1]);
    return -1;
#else
    (void)grace_us;
    (void)directory;
    memset(owner, 0, sizeof(*owner));
    owner->control = owner->events = -1;
    return fork();
#endif
}

int guest_owner_read(struct guest_owner *owner)
{
    struct owner_message m;
    ssize_t n;
    while (owner->events >= 0) {
        n = read(owner->events, owner->message + owner->used, sizeof(m) - owner->used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        if (n <= 0) {
            if (n < 0 || !owner->complete || owner->used)
                owner->fault = 1;
            close(owner->events);
            owner->events = -1;
            break;
        }
        owner->used += (size_t)n;
        if (owner->used < sizeof(m))
            continue;
        memcpy(&m, owner->message, sizeof(m));
        owner->used = 0;
        if (m.kind == OWNER_END || m.kind == OWNER_CLEAN) {
            owner->status = m.status;
            owner->usage = m.usage;
            if (m.kind == OWNER_END)
                owner->ended = 1;
            if (m.kind == OWNER_CLEAN) {
                owner->complete = 1;
                owner->clean = m.clean;
            }
        } else
            owner->fault = 1;
    }
    return owner->complete;
}

void guest_owner_signal(struct guest_owner *owner, int signo)
{
    unsigned char command = (unsigned char)signo;
    ssize_t n;
    if (!owner->enabled || owner->reaped || owner->control < 0)
        return;
    do {
        n = write(owner->control, &command, 1);
    } while (n < 0 && errno == EINTR);
    /* An exited owner may close the pipe before wait4 observes its exit.
     * Its final CLEAN and exit status still have to pass verification. */
    if (n != 1 && !(n < 0 && errno == EPIPE))
        owner->fault = 1;
}

void guest_owner_reaped(struct guest_owner *owner, int status)
{
    guest_owner_read(owner);
    owner->reaped = 1;
    if (!owner->ended || !WIFEXITED(status) || WEXITSTATUS(status))
        owner->fault = 1;
}

void guest_owner_close(struct guest_owner *owner)
{
    if (owner->enabled) {
        if (owner->control >= 0)
            close(owner->control);
        if (owner->events >= 0)
            close(owner->events);
        owner->control = owner->events = -1;
    }
}

int guest_owner_wait(struct guest_owner *owner, uint64_t bound_us)
{
    uint64_t until = posix_now_us() + bound_us;
    int status;
    pid_t pid;
    if (owner->reaped)
        return owner->complete && owner->clean && !owner->fault ? 0 : -1;
    do {
        pid = waitpid(owner->pid, &status, WNOHANG);
        if (pid == owner->pid) {
            guest_owner_reaped(owner, status);
            return owner->complete && owner->clean && !owner->fault ? 0 : -1;
        }
        if (pid < 0 && errno != EINTR)
            return -1;
        /* interval: check the owned process for exit every 10 ms. */
        poll(NULL, 0, 10);
    } while (posix_now_us() < until);
    return -1;
}

int guest_owner_confirm(const char *directory, pid_t pid, unsigned long long start)
{
    char path[4096], text[128], expected[128];
    struct stat st;
    int fd, n, ok;
    if (snprintf(path, sizeof(path), "%s/owned-cleaned", directory) >= (int)sizeof(path))
        return 0;
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return 0;
    n = snprintf(expected, sizeof(expected), "%ld %llu\n", (long)pid, start);
    ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
         (st.st_mode & 0777) == 0600 && st.st_size == n && read(fd, text, sizeof(text)) == n &&
         memcmp(text, expected, (size_t)n) == 0;
    if (close(fd) != 0)
        ok = 0;
    return ok;
}
