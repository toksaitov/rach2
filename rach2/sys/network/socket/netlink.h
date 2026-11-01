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
 * read and write, the IPv4 and IPv6 route tables, and the ARP and neighbour
 * caches.
 *
 * The socket answers the dump requests RTM_GETLINK, RTM_GETADDR, RTM_GETROUTE,
 * RTM_GETNEIGH, RTM_GETRULE (Linux's three rules), RTM_GETQDISC (`noqueue` on
 * each interface) and RTM_GETNETCONF; and, for one object, RTM_GETLINK by an
 * interface's index or name, RTM_GETROUTE for a destination (`ip route get`)
 * and RTM_GETNETCONF by an index. The changes RTM_NEWLINK and RTM_SETLINK (an
 * interface up or down, its MTU), RTM_NEWADDR, RTM_DELADDR, RTM_NEWROUTE,
 * RTM_DELROUTE, RTM_NEWNEIGH and RTM_DELNEIGH are made for a privileged caller
 * through the operations the ioctls use, and refused with EPERM for any other;
 * an MTU out of the interface's range is EINVAL, and a new name, which an
 * interface here cannot take, EOPNOTSUPP. Another request is answered with an
 * NLMSG_ERROR of EOPNOTSUPP. In a network namespace other than the first, a
 * dump is empty and any other request finds no interface.
 *
 * A socket joins multicast groups by its bind's `nl_groups`, groups 1 to
 * 32, and by NETLINK_ADD_MEMBERSHIP, and hears of the first namespace's
 * changes as Linux tells them, each in a datagram of its own with sequence
 * number 0: RTM_NEWLINK in RTNLGRP_LINK when an interface's flags, link or
 * MTU change; RTM_NEWADDR and RTM_DELADDR in RTNLGRP_IPV4_IFADDR and
 * RTNLGRP_IPV6_IFADDR, an IPv6 address's again when its state or lifetimes
 * change; RTM_NEWROUTE and RTM_DELROUTE in RTNLGRP_IPV4_ROUTE and
 * RTNLGRP_IPV6_ROUTE; and RTM_NEWNDUSEROPT in RTNLGRP_ND_USEROPT with each
 * RDNSS and DNSSL option of a router advertisement. No interface is ever
 * removed, so no RTM_DELLINK is sent, and neighbours are not announced. A
 * socket whose queue of notifications would pass NL_NOTIFY_QUEUE_MAX bytes
 * misses the next one and its next receive fails with ENOBUFS, unless it
 * set NETLINK_NO_ENOBUFS.
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

/** Bind `sock` to the port id `pid` and the multicast `groups`, a bit for
 * each of groups 1 to 32. With `pid` 0 the kernel picks one: the
 * calling process's id when no other netlink socket has it, else the next
 * free one of a count from 2^31, where no process id is. Returns 0, or
 * -EADDRINUSE when another netlink socket has the port id asked for. Takes
 * `net_lock` itself. */
int netlink_bind(struct socket *sock, uint32_t pid, uint32_t groups);

/** Set the SOL_NETLINK option `optname` of `sock` to `val`, the int the
 * caller gave in `optlen` bytes: NETLINK_ADD_MEMBERSHIP and
 * NETLINK_DROP_MEMBERSHIP a group, of which those past 32 are taken and
 * hear nothing, and NETLINK_NO_ENOBUFS. Another option is accepted with no
 * effect. Returns 0, or -EINVAL for a value shorter than an int or a group
 * of 0 or less. Takes net_lock. */
int32_t netlink_setsockopt(struct socket *sock, int optname, int val, int optlen);

/** Compare the state notifications are made from with the last, ten times
 * a second, and send the differences. Called every tick from the timer
 * interrupt. */
void netlink_tick(void);

struct netdev;

/** Send the option of `len` bytes at `opt`, of a router advertisement from
 * `src` received on `nd`, to RTNLGRP_ND_USEROPT as RTM_NEWNDUSEROPT, as
 * Linux sends RDNSS and DNSSL options for a program to take the name
 * servers from. Interrupts off. */
void netlink_nd_useropt(const struct netdev *nd, const uint8_t *src, const uint8_t *opt,
                        uint32_t len);

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

/** Note a read of `len` bytes on `sock`: the largest read so far sizes the
 * answers built after it, as Linux sizes a dump's buffers by its reader's
 * (NL_DGRAM_MAX in netlink.c). Called under `net_lock`. */
void netlink_note_recv(struct socket *sock, uint32_t len);

#endif /* _NETLINK_H_ */
