#ifndef _UNIX_H_
#define _UNIX_H_

#include "../lib/kint.h"
#include "../fs/fd.h"

/*
 * AF_UNIX sockets, the local sockets of 4.2BSD as Linux has them. A socket is
 * SOCK_STREAM, a byte stream between two connected ends; SOCK_SEQPACKET, the
 * same connection with the boundaries of messages kept; or SOCK_DGRAM, whose
 * messages go to a name. A name is a path, where bind makes a socket file, or
 * a name in the abstract namespace, which begins with a NUL byte and lasts as
 * long as its socket.
 *
 * A message may carry descriptors (SCM_RIGHTS), which the receiver gets as
 * new descriptors of the same open files, and the sender's credentials
 * (SCM_CREDENTIALS), which a receiver with SO_PASSCRED gets with every
 * message. See Readme.md.
 *
 * A socket is a descriptor of type FD_TYPE_UNIX whose `priv` is the socket;
 * read, write, poll, ioctl and the last close go through its file_ops, and
 * the socket calls of syscall_net.c through the functions below.
 */

#define AF_UNIX        1
#define SOCK_SEQPACKET 5

/* The bytes of a sockaddr_un's sun_path. */
#define UNIX_PATH_MAX 108

/** Start with no sockets; called once at boot. */
void unix_init(void);

/**
 * socket(AF_UNIX, `type`, `protocol`): a new descriptor, or -errno. `flags`
 * are the SOCK_CLOEXEC and SOCK_NONBLOCK bits the caller took off the type.
 */
int32_t unix_socket(int type, int protocol, int flags);

/** socketpair(2): two connected sockets, their descriptors stored at `sv`. */
int32_t unix_socketpair(int type, int protocol, int flags, uint32_t sv);

int32_t unix_bind(fd_entry_t *e, uint32_t addr, uint32_t addrlen);
int32_t unix_connect(fd_entry_t *e, uint32_t addr, uint32_t addrlen);
int32_t unix_listen(fd_entry_t *e, int backlog);

/** accept4(2); `flags` are SOCK_CLOEXEC and SOCK_NONBLOCK for the new
 * descriptor. */
int32_t unix_accept(fd_entry_t *e, uint32_t addr, uint32_t addrlen_ptr, int flags);

/** getsockname(2), or getpeername(2) with `peer`. */
int32_t unix_getname(fd_entry_t *e, uint32_t addr, uint32_t addrlen_ptr, int peer);

int32_t unix_shutdown(fd_entry_t *e, int how);
int32_t unix_setsockopt(fd_entry_t *e, int level, int name, uint32_t val, uint32_t len);
int32_t unix_getsockopt(fd_entry_t *e, int level, int name, uint32_t val, uint32_t len_ptr);
int32_t unix_sendto(fd_entry_t *e, uint32_t buf, uint32_t len, int flags, uint32_t addr,
                    uint32_t addrlen);
int32_t unix_recvfrom(fd_entry_t *e, uint32_t buf, uint32_t len, int flags, uint32_t addr,
                      uint32_t addrlen_ptr);

/** sendmsg(2) and recvmsg(2), with `msg` the caller's struct msghdr. */
int32_t unix_sendmsg(fd_entry_t *e, uint32_t msg, int flags);
int32_t unix_recvmsg(fd_entry_t *e, uint32_t msg, int flags);

/** The socket's inode number, for fstat and /proc/PID/fd. */
uint32_t unix_inode(const fd_entry_t *e);

/** Whether a live AF_UNIX socket has the inode number `ino`. */
int unix_inode_used(uint32_t ino);

/* One AF_UNIX socket with a descriptor open, copied out for /proc/net/unix.
 * `path` has `name_len` bytes of the name, an abstract one beginning with a
 * NUL byte, and 0 for none. */
struct unix_row {
    uint32_t refs;
    int type;
    int listening;
    int connected;
    uint32_t inode;
    uint32_t name_len;
    char path[UNIX_PATH_MAX + 1];
};

/**
 * Copy the sockets with a descriptor open into a caller-owned array and
 * return the row count: -1 when the array cannot be had, and a non-NULL
 * result to free even with no rows.
 */
int unix_snapshot(struct unix_row **rows);

/** How many AF_UNIX sockets have a descriptor open. */
int unix_count(void);

/**
 * Whether a descriptor in flight, sent and not yet received, is a KRFFS file
 * with the node `node`: it holds the node as a descriptor in a table does.
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

#endif /* _UNIX_H_ */
