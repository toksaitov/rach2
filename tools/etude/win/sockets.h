/* sockets.h: nonblocking Windows sockets for the shared QEMU launcher. */
#ifndef ETUDE_WIN_SOCKETS_H
#define ETUDE_WIN_SOCKETS_H
#include <errno.h>
#include "winexec.h"
typedef intptr_t etude_socket;
/** Read a nonblocking socket, translating its error to errno. */
static inline int socket_read(etude_socket fd, char *buf, size_t size)
{
    int n = recv((SOCKET)fd, buf, (int)size, 0);
    if (n == SOCKET_ERROR)
        errno = WSAGetLastError() == WSAEWOULDBLOCK ? EAGAIN : EIO;
    return n;
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
