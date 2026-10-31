/*
 * pty.c: the child's pty, its start, its size and the queue of its input.
 * Task 16 writes it; until then each call fails with ENOSYS.
 */

#include <errno.h>

#include "term.h"

int term_pty_open(struct term_pty *pty, unsigned cols, unsigned rows, char *const *argv, bool login)
{
    (void)cols;
    (void)rows;
    (void)argv;
    (void)login;
    pty->master = -1;
    pty->child = -1;
    pty->slave_open = false;
    pty->queue = NULL;
    pty->queued = 0;
    pty->queue_size = 0;
    return -ENOSYS;
}

int term_pty_resize(struct term_pty *pty, unsigned cols, unsigned rows)
{
    (void)pty;
    (void)cols;
    (void)rows;
    return -ENOSYS;
}

ssize_t term_pty_read(struct term_pty *pty, char *buf, size_t len)
{
    (void)pty;
    (void)buf;
    (void)len;
    return -ENOSYS;
}

int term_pty_write(struct term_pty *pty, const char *buf, size_t len)
{
    (void)pty;
    (void)buf;
    (void)len;
    return -ENOSYS;
}

int term_pty_flush(struct term_pty *pty)
{
    (void)pty;
    return 0;
}

int term_pty_reap(struct term_pty *pty, int *status)
{
    (void)pty;
    (void)status;
    return -ENOSYS;
}

void term_pty_close(struct term_pty *pty)
{
    (void)pty;
}
