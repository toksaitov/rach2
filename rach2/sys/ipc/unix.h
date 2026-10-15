/*
 * unix.h: AF_UNIX sockets, the local sockets of 4.2BSD as Linux has them. A
 * socket is SOCK_STREAM, a byte stream between two connected ends;
 * SOCK_SEQPACKET, the same connection with the boundaries of messages kept;
 * or SOCK_DGRAM, whose messages go to a name. A name is a path, where bind
 * makes a socket file, or a name in the abstract namespace, which begins
 * with a NUL byte and lasts as long as its socket.
 *
 * A message may have descriptors with it (SCM_RIGHTS), which the receiver
 * gets as new descriptors of the same open files, and the sender's
 * credentials (SCM_CREDENTIALS), which a receiver with SO_PASSCRED gets with
 * every message. See Readme.md.
 *
 * A socket is a descriptor of type FD_TYPE_UNIX whose `priv` is the socket;
 * read, write, poll, ioctl and the last close go through its file_ops, and
 * the socket calls of syscall_net.c through the functions below. Those on
 * a socket take its descriptor `e`, and those that wait or copy may sleep.
 */
#ifndef _UNIX_H_
#define _UNIX_H_

#include "../lib/kint.h"
#include "../fs/fd.h"

#define AF_UNIX        1
#define SOCK_SEQPACKET 5

/* The bytes of a sockaddr_un's sun_path. */
#define UNIX_PATH_MAX 108

/** Empty the lists of sockets, of messages whose descriptors are being
 * closed, and of receives in progress. They start empty, and the kernel
 * does not call this. */
void unix_init(void);

/**
 * socket(AF_UNIX, `type`, `protocol`): a new descriptor, or -errno. `flags`
 * are the SOCK_CLOEXEC and SOCK_NONBLOCK bits the caller took off the type.
 * SOCK_RAW is taken as SOCK_DGRAM, as Linux takes it. -ESOCKTNOSUPPORT for
 * a type other than SOCK_STREAM, SOCK_DGRAM and SOCK_SEQPACKET,
 * -EPROTONOSUPPORT for a protocol other than 0 and AF_UNIX, -ENOMEM, or the
 * error of `fd_alloc`.
 */
int32_t unix_socket(int type, int protocol, int flags);

/** socketpair(2): two connected sockets, made as `unix_socket` makes one,
 * their descriptors stored at `sv`; -EFAULT for `sv` leaves neither. */
int32_t unix_socketpair(int type, int protocol, int flags, uintptr_t sv);

/**
 * bind(2) to the sockaddr_un at `addr`: an empty name gives the automatic
 * abstract name of five hex digits, and a path makes its socket file.
 * Returns 0, or -EINVAL for a bad address or a socket already bound or
 * closed, -EADDRINUSE for a name in use, -ENOSPC when no automatic name is
 * free, -EINTR, -EFAULT, or the error of `fs_socket_bind`.
 */
int32_t unix_bind(fd_entry_t *e, uintptr_t addr, uint32_t addrlen);

/**
 * connect(2) to the name at `addr`. A stream or sequenced-packet socket is
 * joined to a new socket queued on the listener, and waits while the
 * listener's queue is full, unless non-blocking (-EAGAIN), for at most its
 * SO_SNDTIMEO. A datagram socket takes the target as the peer its sends
 * without a name go to, or none for the family AF_UNSPEC.
 *
 * Returns 0, or -EISCONN for a connected socket, -EINVAL for a listener, a
 * bad address or an empty name, -ECONNREFUSED when nothing listens or is
 * bound there, -EPROTOTYPE for a target of another type, -EPERM for a
 * datagram target connected to another socket, -EINTR, -ENOMEM, -EFAULT, or
 * the error of `fs_socket_lookup`.
 */
int32_t unix_connect(fd_entry_t *e, uintptr_t addr, uint32_t addrlen);

/**
 * listen(2): the bound socket takes connections, queueing at most
 * `backlog`, UNIX_SOMAXCONN for one negative or larger; a listening socket
 * may call it again to change the backlog. -EOPNOTSUPP for a datagram
 * socket, -EINVAL for one connected or not bound.
 */
int32_t unix_listen(fd_entry_t *e, int backlog);

/**
 * accept4(2): a descriptor of the first connection queued, waiting for one
 * unless non-blocking (-EAGAIN), for at most SO_RCVTIMEO; `flags` are
 * SOCK_CLOEXEC and SOCK_NONBLOCK for it. With `addr` set, the connecting
 * socket's name is stored there and its length at `addrlen_ptr`.
 * -EOPNOTSUPP for a datagram socket, -EINVAL for another flag or for a
 * socket that is not listening, or is shut down for reading, with nothing
 * queued; -EINTR, -EFAULT, or the error of `fd_alloc`.
 */
int32_t unix_accept(fd_entry_t *e, uintptr_t addr, uintptr_t addrlen_ptr, int flags);

/** getsockname(2), or getpeername(2) with `peer`: the name stored at `addr`
 * as far as the length at `addrlen_ptr` has room, and its full length
 * there. -ENOTCONN for the peer of a socket with none, -EINVAL for a
 * negative length, -EFAULT. */
int32_t unix_getname(fd_entry_t *e, uintptr_t addr, uintptr_t addrlen_ptr, int peer);

/** shutdown(2) with `how` SHUT_RD, SHUT_WR or SHUT_RDWR, 0 to 2; -EINVAL
 * for another. */
int32_t unix_shutdown(fd_entry_t *e, int how);

/**
 * setsockopt(2) at SOL_SOCKET: SO_PASSCRED; SO_SNDBUF and SO_RCVBUF, and
 * their FORCE forms, which keep twice the value asked, as Linux does; and
 * SO_RCVTIMEO and SO_SNDTIMEO, a struct timeval. SO_LINGER, SO_REUSEADDR,
 * SO_KEEPALIVE, SO_BROADCAST and the low marks are taken and change
 * nothing. -ENOPROTOOPT for another level or option, -EINVAL for a value
 * shorter than the option's, -EDOM for a timeval out of range, -EFAULT.
 */
int32_t unix_setsockopt(fd_entry_t *e, int level, int name, uintptr_t val, uint32_t len);

/**
 * getsockopt(2) at SOL_SOCKET: an int for SO_TYPE, SO_ERROR, which it
 * clears, SO_PASSCRED, SO_SNDBUF, SO_RCVBUF, SO_ACCEPTCONN, SO_DOMAIN and
 * SO_PROTOCOL, 0 for SO_REUSEADDR, SO_KEEPALIVE and SO_BROADCAST, and 1 for
 * the low marks; SO_PEERCRED's struct ucred, as the caller's namespaces see
 * it, pid 0 and ids -1 when the socket has none. The value is cut to the
 * length at `len_ptr`, where the length copied is stored. -ENOPROTOOPT for
 * another level or option, -EINVAL for a negative length, -EFAULT.
 */
int32_t unix_getsockopt(fd_entry_t *e, int level, int name, uintptr_t val, uintptr_t len_ptr);

/**
 * sendto(2), and send(2) and write(2) with no address: `len` bytes at
 * `buf`, to the name at `addr` when it is set. Returns the bytes sent, or
 * -errno.
 *
 * A datagram socket sends one message, to the name or to its peer, waiting
 * while the receiver's queue is full, unless non-blocking (-EAGAIN), for at
 * most its SO_SNDTIMEO. Its errors are -EMSGSIZE past its send buffer,
 * -EPIPE after a shutdown of its sending or the receiver's receiving,
 * -ENOTCONN with no name and no peer, -ECONNREFUSED for a name nothing is
 * bound to or a receiver that closed, -EPERM for a receiver connected to
 * another socket, -EPROTOTYPE for a path bound by a socket of another type,
 * and the errors of `fs_socket_lookup`.
 *
 * A stream or sequenced-packet socket sends to its peer. A stream's bytes
 * go in pieces as the peer's queue has room, and a send that stops after a
 * piece returns the count sent. Its errors are -ENOTCONN when not
 * connected, -EISCONN or -EOPNOTSUPP for a name, -EMSGSIZE for a packet
 * past the send buffer, the socket's pending error, -EPIPE when the
 * connection is gone or shut down, with SIGPIPE unless `flags` has
 * MSG_NOSIGNAL, -EAGAIN and -EINTR.
 *
 * Either kind: -EINVAL for a length past 2^31 - 1 or a bad address,
 * -EOPNOTSUPP for MSG_OOB, -ENOBUFS when the heap has no room, -EFAULT.
 */
int32_t unix_sendto(fd_entry_t *e, uintptr_t buf, uint32_t len, int flags, uintptr_t addr,
                    uint32_t addrlen);

/**
 * recvfrom(2), and recv(2) and read(2) with no address: up to `len` bytes
 * into `buf`, and with `addr` set the sender's name stored there and its
 * length at `addrlen_ptr`. Returns the bytes read, 0 at the end of input,
 * or -errno.
 *
 * A stream socket reads across messages, waiting for one byte, or with
 * MSG_WAITALL for `len`; it stops after a message that has descriptors, and
 * with SO_PASSCRED before a message of other credentials. A datagram or
 * sequenced-packet socket reads one message, cut to `len`, and with
 * MSG_TRUNC in `flags` returns its whole length. MSG_PEEK leaves what it
 * reads queued. The descriptors a message has are closed, since there is no
 * control buffer to give them in.
 *
 * -EINVAL for a length past 2^31 - 1 or a stream socket not connected,
 * -ENOTCONN for a sequenced-packet socket not connected, -EOPNOTSUPP for
 * MSG_OOB, the socket's pending error, -EAGAIN, -EINTR, -EFAULT.
 */
int32_t unix_recvfrom(fd_entry_t *e, uintptr_t buf, uint32_t len, int flags, uintptr_t addr,
                      uintptr_t addrlen_ptr);

/**
 * sendmsg(2) of the caller's struct msghdr at `msg`: the bytes of its
 * vectors, to its name when it has one, as `unix_sendto` sends them, with
 * its control messages: SCM_RIGHTS, at most UNIX_MAX_FDS descriptors in
 * all, and SCM_CREDENTIALS, which a caller that is not privileged may give
 * only with its own process id and one of its own user and group ids.
 * Besides `unix_sendto`'s errors, -EMSGSIZE for more than IOV_MAX_LOCAL
 * vectors, -EINVAL for a total past 2^31 - 1 or a bad control message,
 * -EBADF for a descriptor not open, -EPERM for credentials not the
 * caller's, -ENOMEM.
 */
int32_t unix_sendmsg(fd_entry_t *e, uintptr_t msg, int flags);

/**
 * recvmsg(2) of the caller's struct msghdr at `msg`: the bytes into its
 * vectors as `unix_recvfrom` reads them, the sender's name, and in its
 * control buffer the credentials when SO_PASSCRED is set and the
 * descriptors received, installed as new ones, as many as the buffer has
 * room for. The others are closed and MSG_CTRUNC is set; MSG_TRUNC marks a
 * message cut. The lengths of the name and the control messages and the
 * flags are written back. Errors as `unix_recvfrom`'s, and -EMSGSIZE and
 * -EINVAL for the vectors as `unix_sendmsg` has them.
 */
int32_t unix_recvmsg(fd_entry_t *e, uintptr_t msg, int flags);

/** The inode number of the socket of the descriptor `e`, for fstat and
 * /proc/PID/fd. */
uint32_t unix_inode(const fd_entry_t *e);

/** Whether an AF_UNIX socket not yet freed has the inode number `ino`. */
int unix_inode_used(uint32_t ino);

/* One AF_UNIX socket with a descriptor open, copied out for /proc/net/unix
 * and the socket diagnostics. `path` has `name_len` bytes of the name, an
 * abstract one beginning with a NUL byte, 0 for none, and a NUL after them.
 * `peer` is the connected socket's inode, 0 for none; a listener's queues
 * are the connections waiting for accept and its backlog, and another's the
 * bytes waiting to be read on it and on its peer. */
struct unix_row {
    uint32_t refs;
    int type;
    int listening;
    int connected;
    uint32_t inode;
    uint32_t uid;
    uint32_t peer;
    uint32_t rqueue;
    uint32_t wqueue;
    uint32_t name_len;
    char path[UNIX_PATH_MAX + 1];
};

/**
 * Copy the sockets of the caller's network namespace that have a
 * descriptor open into an array from the heap at `*rows`, which the caller
 * frees, and return the row count, or -1 when the heap has no room. `*rows`
 * is NULL when no socket of any namespace is open.
 */
int unix_snapshot(struct unix_row **rows);

/** How many AF_UNIX sockets have a descriptor open, in every network
 * namespace. */
int unix_count(void);

/**
 * Whether a descriptor in flight is a KRFFS file whose description has the
 * node `node`: one on a receive queue, on the list of those being closed,
 * or taken off its queue by a receive in progress. It holds the node as a
 * descriptor in a table does.
 */
int unix_inflight_holds(const void *node);

/*
 * The file system's part, in syscall_fs.c. A socket bound to a path holds
 * the socket file it made, and a connect or a send to a path finds the file
 * and the socket bound to it.
 */
struct vfs_ref;
struct vfs_mount;

/**
 * Make the socket file `path` with the permission bits `mode`, and hold it
 * in `ref`; `*ino` is its inode number. EADDRINUSE when the name exists.
 */
int32_t fs_socket_bind(const char *path, uint32_t mode, struct vfs_ref *ref, uint32_t *ino);

/**
 * Find the socket file `path`, which the caller must be able to write: its
 * mount and inode number. ECONNREFUSED when it is not a socket file.
 */
int32_t fs_socket_lookup(const char *path, struct vfs_mount **mnt, uint32_t *ino);

/** Let go of the file `fs_socket_bind` held. */
void fs_socket_unbind(struct vfs_ref *ref);

struct netns;
/** The end of the network namespace `ns`: its sockets move to a namespace
 * no process is in, so no lookup finds their abstract names. */
void unix_ns_gone(const struct netns *ns);

#endif /* _UNIX_H_ */
