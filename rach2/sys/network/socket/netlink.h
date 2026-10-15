#ifndef _NETLINK_H_
#define _NETLINK_H_

#include "../../lib/kint.h"

struct socket;

/*
 * netlink: AF_NETLINK sockets of the NETLINK_ROUTE protocol, the way a Linux
 * program asks the kernel what interfaces, addresses and routes it has, and
 * changes them.
 *
 * BusyBox's `ip` works through such a socket, and Chromium watches the
 * machine's addresses through one. A request is a netlink message, a
 * `struct nlmsghdr` and a body, sent to the kernel; the answer is queued on
 * the socket as datagrams of netlink messages, which the caller reads as a
 * datagram socket's. The answers come from the interface records the ioctls
 * read and write, the route table and the ARP cache, IPv4 only.
 *
 * The socket answers the dump requests RTM_GETLINK, RTM_GETADDR,
 * RTM_GETROUTE, RTM_GETNEIGH, RTM_GETRULE (Linux's three rules),
 * RTM_GETQDISC (`noqueue` on each interface) and RTM_GETNETCONF; and, for
 * one object, RTM_GETLINK by an interface's index or name, RTM_GETROUTE for
 * a destination (`ip route get`) and RTM_GETNETCONF by an index. The changes
 * RTM_NEWLINK and RTM_SETLINK (an interface up or down), RTM_NEWADDR,
 * RTM_DELADDR, RTM_NEWROUTE, RTM_DELROUTE, RTM_NEWNEIGH and RTM_DELNEIGH are
 * made for a privileged caller through the operations the ioctls use, and
 * refused with EPERM for any other; what an interface here cannot hold, an
 * MTU other than its own or a new name, is refused with EOPNOTSUPP. Another
 * request is answered with an NLMSG_ERROR of EOPNOTSUPP. In a network
 * namespace other than the first, a dump is empty and any other request
 * finds no interface. The socket sends no notifications: a socket bound to
 * multicast groups is taken and hears nothing.
 *
 * A socket of NETLINK_SOCK_DIAG answers the dump SOCK_DIAG_BY_FAMILY, which
 * `ss` lists sockets with: for AF_INET the TCP, UDP or raw sockets in the
 * states asked for, as `struct inet_diag_msg`, the raw ones with their
 * protocol; for AF_UNIX the local sockets, as `struct unix_diag_msg` with the
 * name, the peer, the queues and the owner when asked; and for AF_INET6 none.
 * Another family or protocol is answered with ENOENT, which is what Linux
 * answers when no module handles it, and a filter's bytecode is not run:
 * `ss` judges each socket by its filter again.
 *
 * A socket of NETLINK_KOBJECT_UEVENT, which udev's monitors open to hear of
 * devices that come and go, is made, bound and polled like the other and
 * hears nothing: the kernel sends no uevents. What is sent on it is dropped.
 *
 * Opening one is anyone's, as on Linux; a change needs privilege.
 */

#define AF_NETLINK             16
#define NETLINK_ROUTE          0
#define NETLINK_SOCK_DIAG      4
#define NETLINK_KOBJECT_UEVENT 15

/* The sockaddr_nl: the family, two bytes of padding, the port id and the
 * multicast groups, twelve bytes in all. */
#define SOCKADDR_NL_LEN 12

/** Give `sock` the state of a netlink socket of `protocol`; 0, -ENOMEM, or
 * -EPROTONOSUPPORT for a protocol other than NETLINK_ROUTE,
 * NETLINK_SOCK_DIAG and NETLINK_KOBJECT_UEVENT. */
int netlink_open(struct socket *sock, int protocol);

/** Free the state of a netlink socket and what is queued on it. */
void netlink_close(struct socket *sock);

/** Bind `sock` to the port id `pid` and the multicast `groups`, which are
 * kept and never sent to. With `pid` 0 the kernel picks one: the
 * calling process's id when no other netlink socket has it, else the next
 * free one of a count from 2^31, where no process id is. Returns 0, or
 * -EADDRINUSE when another netlink socket has the port id asked for. Takes
 * `net_lock` itself. */
int netlink_bind(struct socket *sock, uint32_t pid, uint32_t groups);

/** The socket's own sockaddr_nl, SOCKADDR_NL_LEN bytes at `sa`, binding it
 * first when it is not bound, as Linux does. */
void netlink_name(struct socket *sock, uint8_t *sa);

/** Take the netlink messages of `buf`, `len` bytes the caller copied in, and
 * queue the answers of those with NLM_F_REQUEST; returns `len`, or -ENOMEM.
 * The messages end at one shorter than a header or longer than the bytes
 * left, and what follows is ignored, as Linux ignores it: iproute2's ip sends
 * a request in a buffer longer than the request, with zeros after it. An
 * unbound socket is bound first, as by `netlink_bind` with 0. On a socket of
 * NETLINK_KOBJECT_UEVENT the messages are dropped. */
int32_t netlink_send(struct socket *sock, const uint8_t *buf, uint32_t len);

/** Bytes in the oldest queued datagram, or -1 when none is queued. */
int netlink_pending(struct socket *sock);

/** Copy part of the oldest datagram from `offset` without taking it; the
 * count copied, 0 past its end, or -1 when none is queued. */
int netlink_peek(struct socket *sock, uint32_t offset, uint8_t *buf, int len);

/** Drop the oldest queued datagram. */
void netlink_consume(struct socket *sock);

/** Whether a datagram is queued. */
int netlink_ready(struct socket *sock);

#endif /* _NETLINK_H_ */
