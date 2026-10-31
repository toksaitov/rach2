/*
 * pty.c: the child's pty, its start, its size and the queue of its input.
 */

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700 /* posix_openpt, grantpt, unlockpt, ptsname */
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE /* TIOCSCTTY beside _XOPEN_SOURCE on macOS */
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "term.h"

/** Set `flag`, FD_CLOEXEC or O_NONBLOCK, on `fd`. Returns 0 or -errno. */
static int fd_set_flag(int fd, int flag)
{
    int cmd_get = flag == FD_CLOEXEC ? F_GETFD : F_GETFL;
    int cmd_set = flag == FD_CLOEXEC ? F_SETFD : F_SETFL;
    int v = fcntl(fd, cmd_get);

    if (v < 0 || fcntl(fd, cmd_set, v | flag) < 0)
        return -errno;
    return 0;
}

/**
 * The child's side of term_pty_open(): become a session leader with the
 * slave `name` as its terminal on descriptors 0 to 2 and run the program.
 *
 * Writes the errno of the step that failed to `report`, which is
 * close-on-exec, so that the parent reads end of file when the exec
 * succeeds. Never returns.
 */
static void child_start(const char *name, int report, char *const *argv, bool login)
{
    static const int sigs[] = {SIGCHLD, SIGTERM, SIGHUP, SIGINT, SIGQUIT, SIGPIPE};
    char *const login_argv[] = {"login", NULL};
    char *shell_argv[2];
    const char *path;
    sigset_t none;
    size_t i;
    int slave;
    int err;

    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        signal(sigs[i], SIG_DFL);
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);

    if (setsid() < 0)
        goto fail;
    slave = open(name, O_RDWR | O_NOCTTY);
    if (slave < 0)
        goto fail;
    if (ioctl(slave, TIOCSCTTY, 0) < 0)
        goto fail;
    if (dup2(slave, 0) < 0 || dup2(slave, 1) < 0 || dup2(slave, 2) < 0)
        goto fail;
    if (slave > 2)
        close(slave);
    if (setenv("TERM", "xterm-256color", 1) < 0 || setenv("COLORTERM", "truecolor", 1) < 0)
        goto fail;

    if (argv) {
        execvp(argv[0], argv);
    } else if (login) {
        execv("/bin/login", login_argv);
    } else {
        path = getenv("SHELL");
        if (!path || path[0] != '/')
            path = "/bin/sh";
        shell_argv[0] = strrchr(path, '/') + 1;
        shell_argv[1] = NULL;
        execv(path, shell_argv);
    }
fail:
    err = errno;
    (void)!write(report, &err, sizeof(err));
    _exit(127);
}

int term_pty_open(struct term_pty *pty, unsigned cols, unsigned rows, char *const *argv, bool login)
{
    struct winsize ws;
    char name[128];
    const char *n;
    int report[2];
    int err = 0;
    ssize_t got;
    int status;
    int r;

    pty->master = -1;
    pty->child = -1;
    pty->slave_open = false;
    pty->queue = NULL;
    pty->queued = 0;
    pty->queue_size = 0;

    pty->master = posix_openpt(O_RDWR | O_NOCTTY);
    if (pty->master < 0)
        return -errno;
    if ((r = fd_set_flag(pty->master, FD_CLOEXEC)) < 0)
        goto fail;
    if (grantpt(pty->master) < 0 || unlockpt(pty->master) < 0 || !(n = ptsname(pty->master))) {
        r = -errno;
        goto fail;
    }
    if (strlen(n) >= sizeof(name)) {
        r = -ENAMETOOLONG;
        goto fail;
    }
    strcpy(name, n);
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    if (ioctl(pty->master, TIOCSWINSZ, &ws) < 0) {
        r = -errno;
        goto fail;
    }

    if (pipe(report) < 0) {
        r = -errno;
        goto fail;
    }
    if ((r = fd_set_flag(report[0], FD_CLOEXEC)) < 0 ||
        (r = fd_set_flag(report[1], FD_CLOEXEC)) < 0) {
        close(report[0]);
        close(report[1]);
        goto fail;
    }
    pty->child = fork();
    if (pty->child < 0) {
        r = -errno;
        close(report[0]);
        close(report[1]);
        goto fail;
    }
    if (pty->child == 0) {
        close(report[0]);
        child_start(name, report[1], argv, login);
    }
    close(report[1]);
    do
        got = read(report[0], &err, sizeof(err));
    while (got < 0 && errno == EINTR);
    close(report[0]);
    if (got != 0) {
        r = got == (ssize_t)sizeof(err) ? -err : -EIO;
        while (waitpid(pty->child, &status, 0) < 0 && errno == EINTR)
            ;
        pty->child = -1;
        goto fail;
    }
    pty->slave_open = true;
    if ((r = fd_set_flag(pty->master, O_NONBLOCK)) < 0) {
        term_pty_close(pty);
        return r;
    }
    return 0;

fail:
    close(pty->master);
    pty->master = -1;
    return r;
}

int term_pty_resize(struct term_pty *pty, unsigned cols, unsigned rows)
{
    struct winsize ws;

    if (pty->master < 0)
        return -EBADF;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    if (ioctl(pty->master, TIOCSWINSZ, &ws) < 0)
        return -errno;
    return 0;
}

ssize_t term_pty_read(struct term_pty *pty, char *buf, size_t len)
{
    ssize_t n;

    if (pty->master < 0 || !pty->slave_open)
        return -EAGAIN;
    n = read(pty->master, buf, len);
    if (n >= 0)
        return n;
    if (errno == EIO)
        return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
        return -EAGAIN;
    return -errno;
}

int term_pty_flush(struct term_pty *pty)
{
    size_t done = 0;
    int err = 0;
    ssize_t n;

    if (pty->master < 0)
        return pty->queued ? -EBADF : 0;
    while (done < pty->queued) {
        n = write(pty->master, pty->queue + done, pty->queued - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            err = n < 0 ? errno : EAGAIN;
            break;
        }
        done += (size_t)n;
    }
    memmove(pty->queue, pty->queue + done, pty->queued - done);
    pty->queued -= done;
    if (pty->queued == 0)
        return 0;
    return err == EAGAIN || err == EWOULDBLOCK ? -EAGAIN : -err;
}

int term_pty_write(struct term_pty *pty, const char *buf, size_t len)
{
    size_t size;
    char *q;
    int r;

    if (pty->master < 0)
        return -EBADF;
    if (len > TERM_PTY_QUEUE_MAX - pty->queued)
        return -ENOBUFS;
    if (pty->queued + len > pty->queue_size) {
        size = pty->queue_size ? pty->queue_size : 4096;
        while (size < pty->queued + len)
            size *= 2;
        q = realloc(pty->queue, size);
        if (!q)
            return -ENOMEM;
        pty->queue = q;
        pty->queue_size = size;
    }
    memcpy(pty->queue + pty->queued, buf, len);
    pty->queued += len;
    r = term_pty_flush(pty);
    return r == -EAGAIN ? 0 : r;
}

int term_pty_reap(struct term_pty *pty, int *status)
{
    pid_t p;

    if (pty->child < 0)
        return 0;
    do
        p = waitpid(pty->child, status, WNOHANG);
    while (p < 0 && errno == EINTR);
    if (p < 0)
        return -errno;
    if (p == 0)
        return 0;
    pty->child = -1;
    return 1;
}

void term_pty_close(struct term_pty *pty)
{
    if (pty->master >= 0)
        close(pty->master);
    pty->master = -1;
    pty->slave_open = false;
    free(pty->queue);
    pty->queue = NULL;
    pty->queued = 0;
    pty->queue_size = 0;
}
