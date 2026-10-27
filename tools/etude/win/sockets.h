/*
 * sockets.h: nonblocking Windows sockets for the shared QEMU launcher, read
 * through a receive that is kept posted.
 */
#ifndef ETUDE_WIN_SOCKETS_H
#define ETUDE_WIN_SOCKETS_H
#include <errno.h>
#include <string.h>
#include "winexec.h"
typedef intptr_t etude_socket;

/* The bytes one posted receive takes. */
#define SOCKET_POSTED 4096

/* The most bytes taken from completed receives and not yet read. */
#define SOCKET_HELD_MAX 65536

/*
 * The reading side of a connection to QEMU. A Windows process that exits
 * resets its loopback connections, and the reset discards the bytes that have
 * arrived but that no receive has taken. Bytes that arrive while a receive is
 * posted go into its buffer and survive the reset, so a receive stays posted
 * from the connection's accept to its end, and what it takes waits in `held`
 * for the launcher to read.
 */
struct socket_in {
    WSAOVERLAPPED ov;
    char buf[SOCKET_POSTED];
    char held[SOCKET_HELD_MAX];
    size_t held_at, held_len; /* the unread bytes are held[held_at..held_len) */
    int posted;               /* a receive into buf is outstanding */
    int end;                  /* 0, 1 after the peer's end, or -1 after an error */
};

/** Post a receive into `in->buf`, or mark the connection ended. */
static inline void socket_in_post(struct socket_in *in, etude_socket fd)
{
    WSABUF b;
    DWORD flags = 0;

    b.buf = in->buf;
    b.len = sizeof(in->buf);
    WSAResetEvent(in->ov.hEvent);
    if (WSARecv((SOCKET)fd, &b, 1, NULL, &flags, &in->ov, NULL) == 0 ||
        WSAGetLastError() == WSA_IO_PENDING)
        in->posted = 1;
    else
        in->end = -1;
}

/**
 * Take what the posted receives have completed into `held`, posting the next
 * after each, until one is outstanding or `held` is full. A receive that
 * completes at once still reports through its overlapped structure.
 */
static inline void socket_in_pump(struct socket_in *in, etude_socket fd)
{
    DWORD got, flags;

    if (in->held_at > 0) {
        memmove(in->held, in->held + in->held_at, in->held_len - in->held_at);
        in->held_len -= in->held_at;
        in->held_at = 0;
    }
    while (in->posted && in->held_len + SOCKET_POSTED <= SOCKET_HELD_MAX) {
        if (!WSAGetOverlappedResult((SOCKET)fd, &in->ov, &got, FALSE, &flags)) {
            if (WSAGetLastError() == WSA_IO_INCOMPLETE)
                return;
            in->posted = 0;
            in->end = -1;
            return;
        }
        in->posted = 0;
        if (got == 0) {
            in->end = 1;
            return;
        }
        memcpy(in->held + in->held_len, in->buf, got);
        in->held_len += got;
        socket_in_post(in, fd);
    }
    if (!in->posted && in->end == 0 && in->held_len + SOCKET_POSTED <= SOCKET_HELD_MAX)
        socket_in_post(in, fd);
}

/**
 * Start reading connection `fd` into `in`. Without an event for the receive
 * the connection reads as one that failed.
 */
static inline void socket_in_start(struct socket_in *in, etude_socket fd)
{
    memset(in, 0, sizeof(*in));
    in->ov.hEvent = WSACreateEvent();
    if (in->ov.hEvent == WSA_INVALID_EVENT) {
        in->ov.hEvent = NULL;
        in->end = -1;
        return;
    }
    socket_in_post(in, fd);
}

/** Cancel the receive posted on `fd` and wait until it has ended. */
static inline void socket_in_stop(struct socket_in *in, etude_socket fd)
{
    DWORD got, flags;

    if (in->posted) {
        CancelIoEx((HANDLE)fd, (LPOVERLAPPED)&in->ov);
        WSAGetOverlappedResult((SOCKET)fd, &in->ov, &got, TRUE, &flags);
        in->posted = 0;
    }
    if (in->ov.hEvent != NULL)
        WSACloseEvent(in->ov.hEvent);
    in->ov.hEvent = NULL;
}

/** Say whether `in` has bytes or an end that the launcher has not yet read. */
static inline int socket_in_ready(struct socket_in *in, etude_socket fd)
{
    socket_in_pump(in, fd);
    return in->held_at < in->held_len || (!in->posted && in->end != 0);
}

/**
 * Read connection `fd` through `in`: the bytes held first, then 0 after the
 * peer's end, or -1 with errno EIO after an error and EAGAIN while the
 * receive waits.
 */
static inline int socket_read(struct socket_in *in, etude_socket fd, char *buf, size_t size)
{
    size_t n;

    socket_in_pump(in, fd);
    n = in->held_len - in->held_at;
    if (n > 0) {
        if (n > size)
            n = size;
        memcpy(buf, in->held + in->held_at, n);
        in->held_at += n;
        return (int)n;
    }
    if (in->end > 0)
        return 0;
    errno = in->end < 0 ? EIO : EAGAIN;
    return -1;
}

/** Write a nonblocking socket, translating its error to errno. */
static inline int socket_write(etude_socket fd, const char *buf, size_t size)
{
    int n = send((SOCKET)fd, buf, (int)size, 0);
    if (n == SOCKET_ERROR)
        errno = WSAGetLastError() == WSAEWOULDBLOCK ? EAGAIN : EPIPE;
    return n;
}
#define socket_close(fd) closesocket((SOCKET)(fd))
#endif
