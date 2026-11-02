/*
 * The socket calls of the system call layer: socketcall, the direct socket
 * calls and the operations they share, over the socket layer. Each
 * architecture's `arch_syscall_init` registers them; what this file shares
 * with the rest of the layer is in syscall_internal.h.
 */
#include "../kernel.h"
#include "mm/kheap.h"
#include "klog.h"
#include "syscall.h"
#include "power.h"
#include "serial.h"
#include "panic.h"
#include "kio.h"
#include "kbd.h"
#include "input.h"
#include "mouse.h"
#include "proc/process.h"
#include "proc/ns.h"
#include "proc/sched.h"
#include "proc/elf.h"
#include "proc/uimage.h"
#include "lib/krandom.h"
#include "lib/kerrno.h"
#include "lib/ksignal.h"
#include "fs/fd.h"
#include "fs/file.h"
#include "ipc/pipe.h"
#include "io/poll.h"
#include "tty/tty.h"
#include "tty/pty.h"
#include "rtc.h"
#include "block/loop.h"
#include "fs/krffs.h"
#include "fs/devfs.h"
#include "fs/synthfs.h"
#include "fs/vfs.h"
#include "block/usbdisk.h"
#include "fs/procfs.h"
#include "mm/paging.h"
#include "mm/pressure.h"
#include "mm/pagecache.h"
#include "mm/heap.h"
#include "mm/usercopy.h"
#include "mm/vm.h"
#include "lib/kuser.h"
#include "lib/kstring.h"
#include "display/fb.h"
#include "speaker.h"
#include "audio/audio.h"
#include "network/socket/socket.h"
#include "network/socket/netlink.h"
#include "network/socket/packet.h"
#include "network/socket/raw.h"
#include "network/ethernet/ethernet.h"
#include "network/netif.h"
#include "network/driver/netdev.h"
#include "network/udp/udp.h"
#include "network/tcp/tcp.h"
#include "network/ip/ip.h"
#include "network/ip/icmp.h"
#include "network/ip6/ip6.h"
#include "network/ip6/icmp6.h"
#include "network/net.h"
#include "network/netconf.h" /* netconf_is_broadcast */
#include "cmdline.h"         /* kcmdline_has, for the ENOSYS trace flag */
#include "ipc/unix.h"
#include "syscall_internal.h"

/* Read and write the little-endian words of a caller's own structures (a
 * sockaddr_nl's port id and groups, a sockaddr_ll's interface index), which
 * are in host order rather than wire order. */
#define SC_LE32(p)                                                            \
    (((uint32_t)((uint8_t *)(p))[0]) | ((uint32_t)((uint8_t *)(p))[1] << 8) | \
     ((uint32_t)((uint8_t *)(p))[2] << 16) | ((uint32_t)((uint8_t *)(p))[3] << 24))

#define SC_WLE32(p, v)                              \
    do {                                            \
        ((uint8_t *)(p))[0] = (uint8_t)(v);         \
        ((uint8_t *)(p))[1] = (uint8_t)((v) >> 8);  \
        ((uint8_t *)(p))[2] = (uint8_t)((v) >> 16); \
        ((uint8_t *)(p))[3] = (uint8_t)((v) >> 24); \
    } while (0)

/* Read and write big-endian sockaddr fields (sin_port, sin_addr). */
#define SC_BE16(p) ((uint16_t)(((uint8_t *)(p))[0] << 8 | ((uint8_t *)(p))[1]))

#define SC_BE32(p)                                                                   \
    (((uint32_t)((uint8_t *)(p))[0] << 24) | ((uint32_t)((uint8_t *)(p))[1] << 16) | \
     ((uint32_t)((uint8_t *)(p))[2] << 8) | ((uint32_t)((uint8_t *)(p))[3]))

#define SC_WBE16(p, v)                             \
    do {                                           \
        ((uint8_t *)(p))[0] = (uint8_t)((v) >> 8); \
        ((uint8_t *)(p))[1] = (uint8_t)(v);        \
    } while (0)

#define SC_WBE32(p, v)                              \
    do {                                            \
        ((uint8_t *)(p))[0] = (uint8_t)((v) >> 24); \
        ((uint8_t *)(p))[1] = (uint8_t)((v) >> 16); \
        ((uint8_t *)(p))[2] = (uint8_t)((v) >> 8);  \
        ((uint8_t *)(p))[3] = (uint8_t)(v);         \
    } while (0)

/* socketcall sub-function numbers (Linux standard). */
#define SC_SOCKET 1

#define SC_BIND 2

#define SC_CONNECT 3

#define SC_LISTEN 4

#define SC_ACCEPT 5

#define SC_GETSOCKNAME 6

#define SC_GETPEERNAME 7

#define SC_SEND 9

#define SC_RECV 10

#define SC_SENDTO 11

#define SC_RECVFROM 12

#define SC_SHUTDOWN 13

#define SC_SETSOCKOPT 14

#define SC_GETSOCKOPT 15

#define SC_SOCKETPAIR 8

#define SC_SENDMSG 16

#define SC_RECVMSG 17

#define SC_ACCEPT4 18

#define SC_RECVMMSG 19

#define SC_SENDMMSG 20

/* A struct mmsghdr: a struct msghdr, then the count the call stores for
 * it. */
struct kernel_mmsghdr {
    struct kernel_msghdr msg_hdr;
    unsigned int msg_len;
};

#define MMSG_LEN_OFF offsetof(struct kernel_mmsghdr, msg_len)

/* Policy: the messages one sendmmsg or recvmmsg takes, Linux's UIO_MAXIOV. */
#define MMSG_MAX 1024u

/* recvmmsg: wait for the first message only. */
#define MSG_WAITFORONE 0x10000u

/* The creation flags socket(2) takes above the type's low four bits and
 * accept4(2) takes in its flags: the values of O_CLOEXEC and O_NONBLOCK,
 * as Linux defines them. */
#define SOCK_TYPE_MASK 0xf

#define SOCK_CLOEXEC 02000000

#define SOCK_NONBLOCK 04000

/* Rach2's ICMP echo sub-call of socketcall, numbered clear of Linux's
 * sub-calls; utils/pingcall.h has the same values for utils/ping.c, and the
 * probes use them too. SC_PING takes the destination. */
#define SC_PING      0x1000
/* The same with the data size and hop limit, for ping's -s and -t. */
#define SC_PING_DATA 0x1001
/* An IPv6 echo: the address of the 16-byte destination, its scope, the data
 * size and the hop limit, for ping's -6. */
#define SC_PING6     0x1002

static int32_t dgram_send_opt(struct socket *sock, net_addr_t dst_ip, uint16_t dst_port,
                              const uint8_t *buf, uint16_t len, const struct net_txopt *opt);

/**
 * Send the `len` bytes at `buf` as one UDP datagram from `sock` to `dst_ip`
 * and `dst_port`, checking the destination first, and binding the socket to
 * an ephemeral port when it has none. Returns `len`, or the socket's pending
 * error, -EACCES for a broadcast address without `SO_BROADCAST`, -EMSGSIZE
 * past 65,507 IPv4 or 65,527 IPv6 bytes (`UDP_PAYLOAD_MAX`), -EAGAIN when
 * no ephemeral port is free, or the error of `udp_tx`, which is reported
 * rather than counted as bytes sent.
 *
 * A datagram to a broadcast address reaches every machine on the wire, so a
 * program must set `SO_BROADCAST` on the socket before it can send one: a
 * program that broadcasts by accident wakes the whole network. A datagram
 * past one frame leaves in fragments. The hop limit is the socket's, which
 * IP_TTL sets and traceroute counts up, or over IPv6 IPV6_UNICAST_HOPS.
 */
int32_t dgram_send(struct socket *sock, net_addr_t dst_ip, uint16_t dst_port, const uint8_t *buf,
                   uint16_t len)
{
    return dgram_send_opt(sock, dst_ip, dst_port, buf, len, 0);
}

/** `dgram_send` with sendmsg's control messages `opt`, NULL for none,
 * which `udp_tx` applies to an IPv6 datagram. */
static int32_t dgram_send_opt(struct socket *sock, net_addr_t dst_ip, uint16_t dst_port,
                              const uint8_t *buf, uint16_t len, const struct net_txopt *opt)
{
    int r = socket_dgram_error(sock);

    if (r < 0) {
        return r;
    }
    if (dst_ip.family != AF_INET6 && !sock->broadcast && netconf_is_broadcast(dst_ip.v4)) {
        return -EACCES;
    }
    if (len > (dst_ip.family == AF_INET6 ? UDP6_PAYLOAD_MAX : UDP_PAYLOAD_MAX)) {
        return -EMSGSIZE;
    }
    if (sock->local_port == 0 && udp_bind_ephemeral(sock) == 0) {
        return -EAGAIN;
    }
    r = udp_tx(sock, dst_ip, dst_port, buf, len, opt);
    if (r < 0) {
        return r;
    }
    return (int32_t)len;
}

/* The raw send buffer: one frame, filled from the caller's memory in a
 * system call; a larger message is copied to a block of the heap. Only a raw
 * send touches it, under the big lock that admits one processor to the
 * kernel at a time, and nothing sleeps between the copy into it and the
 * send, so it needs no lock of its own; what needs the network lock is the
 * send, which goes through the transmit buffers an interrupt also builds
 * in. */
static uint8_t raw_send_buf[1500];

/** Count the ICMP message of `len` bytes at `msg` that a raw socket sent,
 * when IP took it (`r` is 0), as Linux counts one. */
static void raw_count_sent(const struct socket *sock, int r, const uint8_t *msg, int len)
{
    if (r == 0 && sock->protocol == IP_PROTO_ICMP && len > 0) {
        icmp_count_out(msg[0]);
    }
}

/* recv flags honored: MSG_PEEK leaves what it copies for the next receive,
 * MSG_TRUNC has a receive on a datagram socket answer the datagram's whole
 * length, and MSG_DONTWAIT does not wait. */
#define MSG_PEEK     0x2
#define MSG_TRUNC    0x20
#define MSG_DONTWAIT 0x40

#define IPPROTO_RAW 255

/** The answer of a socket call on a descriptor that is no socket: EBADF for
 * one that is not open, as Linux gives it, and ENOTSOCK for another kind of
 * file. gnulib's tests of accept, bind, connect, listen and setsockopt
 * expect the first of each. */
static int32_t not_a_socket(const fd_entry_t *entry)
{
    return entry ? -ENOTSOCK : -EBADF;
}

#define IPPROTO_ICMP 1

#define IPPROTO_ICMPV6 58

/* setsockopt(2)/getsockopt(2) at the IP level: the hop limit a socket's
 * datagrams leave with, the option that would hand the caller the header,
 * which this stack does not implement, and the IP options of a socket's
 * packets, of which it keeps none. */
#define SOL_IPV6    41
#define IPV6_V6ONLY 26

/* The IPv6 options of RFC 3493 and RFC 3542, by Linux's numbers, which an
 * AF_INET6 socket keeps (socket.h), and the control message types a
 * receive answers with and a send takes: IPV6_PKTINFO, IPV6_HOPLIMIT and
 * IPV6_TCLASS. */
#define IPV6_UNICAST_HOPS   16
#define IPV6_MULTICAST_IF   17
#define IPV6_MULTICAST_HOPS 18
#define IPV6_MULTICAST_LOOP 19
#define IPV6_JOIN_GROUP     20
#define IPV6_LEAVE_GROUP    21
#define IPV6_RECVPKTINFO    49
#define IPV6_PKTINFO        50
#define IPV6_RECVHOPLIMIT   51
#define IPV6_HOPLIMIT       52
#define IPV6_RECVTCLASS     66
#define IPV6_TCLASS         67

/* struct ipv6_mreq: the group (16 bytes) and an interface index (4); and
 * struct in6_pktinfo: an address (16) and an interface index (4). */
#define IPV6_MREQ_LEN   20
#define IN6_PKTINFO_LEN 20

/* The ICMPv6 level and its one option, the filter of a raw ICMPv6 socket
 * (RFC 3542, 3.2). */
#define SOL_ICMPV6   58
#define ICMP6_FILTER 1

/* Netlink's level, whose options `netlink_setsockopt` takes. */
#define SOL_NETLINK 270

/* msg_flags: the control messages did not all fit the caller's buffer. */
#define MSG_CTRUNC 0x8

#define SOL_IP 0

#define IP_TTL 2

#define IP_HDRINCL 3

#define IP_OPTIONS 4

/* IP_MTU_DISCOVER: the IP_PMTUDISC_* of the socket's IPv4 datagrams
 * (ip.h). */
#define IP_MTU_DISCOVER 10

/* SO_RCVBUF: the bytes a datagram socket's receive queue may hold. */
#define SO_RCVBUF 8

/* setsockopt levels and the one packet option that is answered. */
#define SOL_PACKET 263

#define PACKET_AUXDATA 8

/* The first 8 bytes of a sockaddr: the sin_family, sin_port and sin_addr
 * that are read. */
#define SOCKADDR_MIN 8

/* The whole of a struct sockaddr_in: family (2), port (2), address (4), and
 * eight bytes of padding. `SOCKADDR_MIN` above is only the part that is read.
 */
#define SOCKADDR_IN_LEN 16

/* struct sockaddr_ll, the address a packet socket binds and sends to, with the
 * offset of each field beside it: family 0 (2 bytes), protocol 2 (2), ifindex
 * 4 (4), hatype 8 (2), pkttype 10 (1), halen 11 (1), addr 12 (8), for 20 in
 * all. A caller declaring fewer bytes gets -EINVAL, and nothing past them is
 * read. */
#define SOCKADDR_LL_LEN 20

#define SOCKADDR_LL_PROTOCOL 2

#define SOCKADDR_LL_IFINDEX 4

#define SOCKADDR_LL_HATYPE 8

#define ARPHRD_ETHER 1 /* sll_hatype for an Ethernet interface */

#define SOCKADDR_LL_HALEN 11

#define SOCKADDR_LL_ADDR 12

/* struct sockaddr_in6: family 0 (2 bytes), port 2 (2), flow information 4
 * (4), address 8 (16) and scope id 24 (4), for 28 in all. A caller may
 * declare RFC 2133's 24 bytes, without the scope id, as on Linux, and no
 * fewer. The scope id is an interface index in host order. */
#define SOCKADDR_IN6_LEN   28
#define SOCKADDR_IN6_MIN   24
#define SOCKADDR_IN6_ADDR  8
#define SOCKADDR_IN6_SCOPE 24

/**
 * Read the sockaddr_in6 the caller declared `addrlen` bytes of at `sa_ptr`
 * into `addr` and its scope id into `*scope`, 0 when the caller gave none.
 * Returns 0, -EINVAL for fewer than 24 bytes, -EAFNOSUPPORT for another
 * family, or -EFAULT.
 */
static int sockaddr_in6_in(uintptr_t sa_ptr, int addrlen, uint8_t *addr, int *scope)
{
    uint8_t sa[SOCKADDR_IN6_LEN];

    if (addrlen < SOCKADDR_IN6_MIN) {
        return -EINVAL;
    }
    k_memset(sa, 0, sizeof(sa));
    if (copy_from_user(sa, (const void __user *)sa_ptr,
                       addrlen < SOCKADDR_IN6_LEN ? (uint32_t)addrlen : SOCKADDR_IN6_LEN) != 0) {
        return -EFAULT;
    }
    if ((uint16_t)((uint16_t)sa[0] | ((uint16_t)sa[1] << 8)) != AF_INET6) {
        return -EAFNOSUPPORT;
    }
    k_memcpy(addr, sa + SOCKADDR_IN6_ADDR, 16);
    *scope = (int)((uint32_t)sa[SOCKADDR_IN6_SCOPE] | ((uint32_t)sa[SOCKADDR_IN6_SCOPE + 1] << 8) |
                   ((uint32_t)sa[SOCKADDR_IN6_SCOPE + 2] << 16) |
                   ((uint32_t)sa[SOCKADDR_IN6_SCOPE + 3] << 24));
    return 0;
}

/** Fill the 28 bytes at `sa` as the sockaddr_in6 of `addr`, with the scope
 * `ifindex` for a link-local address and 0 for another, as a receive
 * reports its sender. */
static void sockaddr_in6_out(uint8_t *sa, const uint8_t *addr, int ifindex)
{
    uint32_t scope = ip6_link_scoped(addr) && ifindex > 0 ? (uint32_t)ifindex : 0u;

    k_memset(sa, 0, SOCKADDR_IN6_LEN);
    sa[0] = (uint8_t)AF_INET6;
    k_memcpy(sa + SOCKADDR_IN6_ADDR, addr, 16);
    sa[SOCKADDR_IN6_SCOPE] = (uint8_t)scope;
    sa[SOCKADDR_IN6_SCOPE + 1] = (uint8_t)(scope >> 8);
    sa[SOCKADDR_IN6_SCOPE + 2] = (uint8_t)(scope >> 16);
    sa[SOCKADDR_IN6_SCOPE + 3] = (uint8_t)(scope >> 24);
}

/** Read an Internet endpoint and normalize mapped addresses. A link-scoped
 * IPv6 address with no scope id takes the socket's bound interface, then
 * `pktif`, a sendmsg's IPV6_PKTINFO interface or 0, and a group with none
 * of them IPV6_MULTICAST_IF's, as on Linux. Returns zero or a negative
 * errno: -EINVAL for a link-scoped address left with no interface, -ENODEV
 * for one the socket may not name (`socket_iface6`); checks the socket's
 * family and V6ONLY. */
static int inet_endpoint_in(const struct socket *sock, uintptr_t ptr, int len, int pktif,
                            net_addr_t *addr, uint16_t *port)
{
    uint8_t sa[SOCKADDR_MIN], ip[16];
    int scope, rc;
    uint16_t family;
    if (len < SOCKADDR_MIN)
        return -EINVAL;
    if (copy_from_user(sa, (const void __user *)ptr, sizeof(sa)))
        return -EFAULT;
    family = (uint16_t)(sa[0] | (sa[1] << 8));
    if (sock->family == AF_INET6 && family == AF_INET6) {
        rc = sockaddr_in6_in(ptr, len, ip, &scope);
        if (rc)
            return rc;
        *addr = net_addr6(ip, (uint32_t)scope);
    } else if (family == AF_INET &&
               (sock->family == AF_INET || (sock->family == AF_INET6 && !sock->v6only))) {
        *addr = net_addr4(SC_BE32(sa + 4));
    } else
        return -EAFNOSUPPORT;
    if (sock->v6only && addr->family != AF_INET6)
        return -ENETUNREACH;
    if (addr->family == AF_INET6 && ip6_link_scoped(addr->v6)) {
        if (!addr->scope)
            addr->scope = (uint32_t)sock->bound_device;
        if (!addr->scope)
            addr->scope = (uint32_t)pktif; /* IPV6_PKTINFO */
        if (!addr->scope && ip6_is_multicast(addr->v6))
            addr->scope = (uint32_t)sock->mif6; /* IPV6_MULTICAST_IF */
        if (!addr->scope)
            return -EINVAL;
        if (!socket_iface6(sock, (int)addr->scope))
            return -ENODEV;
    }
    *port = SC_BE16(sa + 2);
    return 0;
}

/** Build an Internet sockaddr in the family the socket exposes. */
static void inet_endpoint_out(uint8_t *sa, uint16_t family, net_addr_t addr, uint16_t port)
{
    if (family == AF_INET6) {
        uint8_t ip[16];
        net_addr_to6(addr, ip);
        sockaddr_in6_out(sa, ip, (int)addr.scope);
    } else {
        k_memset(sa, 0, SOCKADDR_IN_LEN);
        sa[0] = AF_INET;
        SC_WBE32(sa + 4, addr.v4);
    }
    SC_WBE16(sa + 2, port);
}

/**
 * Write an Internet source/peer sockaddr back to a caller's (addr, addrlen)
 * pair.
 *
 * Respects the caller's declared buffer capacity: read the in/out socklen,
 * copy at most that many of the 16 or 28 sockaddr bytes, and report the true
 * length, the recvfrom(2)/accept(2) truncation semantics. A null
 * address or length pointer writes nothing. Returns 0 or -EFAULT.
 *
 * sin_family is in host byte order, little-endian here, and only sin_port
 * and sin_addr in network order: a big-endian AF_INET would read back as
 * 0x0200, which a reader of the peer's address, such as dropbear's logging
 * of a connection, takes for an unknown family.
 */
static int copy_sockaddr_to_user(uintptr_t sa_ptr, uintptr_t salen_ptr, uint16_t port,
                                 net_addr_t ip, uint16_t family)
{
    uint8_t sa[SOCKADDR_IN6_LEN];
    uint32_t width = family == AF_INET6 ? SOCKADDR_IN6_LEN : SOCKADDR_IN_LEN;
    uint32_t caller_len;
    uint32_t copy_len;

    if (!sa_ptr || !salen_ptr) {
        return 0; /* no address to report */
    }

    if (copy_from_user(&caller_len, (const void __user *)salen_ptr, sizeof(caller_len)) != 0) {
        return -EFAULT;
    }

    inet_endpoint_out(sa, family, ip, port);

    copy_len = (caller_len < width) ? caller_len : (uint32_t)width;
    if (copy_len > 0 && copy_to_user((void __user *)sa_ptr, sa, copy_len) != 0) {
        return -EFAULT;
    }

    caller_len = (uint32_t)width;
    if (copy_to_user((void __user *)salen_ptr, &caller_len, sizeof(caller_len)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * Give the new descriptor `entry` the creation flags in `flags`: close-on-exec
 * for SOCK_CLOEXEC, and for SOCK_NONBLOCK an open file whose calls do not
 * block, as O_CLOEXEC and O_NONBLOCK give an opened file.
 */
static void sock_fd_flags(fd_entry_t *entry, uint32_t flags)
{
    if (flags & SOCK_CLOEXEC) {
        entry->flags |= FD_FLAG_CLOEXEC;
    }
    if (flags & SOCK_NONBLOCK) {
        entry->ofd->status_flags |= OFD_NONBLOCK;
    }
}

/** Whether a packet socket may take the EtherType type, in host order: 0,
 * which a socket opened before its bind has, or one of IPv4, ARP, IPv6 and
 * EAPOL, the types this stack has frames for. */
static int packet_type_ok(uint16_t type)
{
    return type == 0 || type == ETH_TYPE_IP || type == ETH_TYPE_ARP || type == ETH_TYPE_IPV6 ||
           type == ETH_TYPE_PAE;
}

/** Whether `protocol` is 0 or the IP protocol of stream or datagram `type`,
 * TCP's or UDP's, the two an IPv4 or IPv6 socket of those types may name. */
static int ip_protocol_ok(int type, int protocol)
{
    return protocol == 0 || protocol == (type == SOCK_STREAM ? 6 : 17);
}

/**
 * Check the kind of socket socket(2) is asked to make, `domain`, `*type` and
 * `protocol`: AF_INET with TCP, UDP or SOCK_RAW (ICMP, or the ioctl-only
 * descriptor of IPPROTO_RAW), AF_INET6 with TCP, UDP, raw ICMPv6 or raw
 * IPv6, AF_PACKET with SOCK_DGRAM, and
 * AF_NETLINK with SOCK_RAW or SOCK_DGRAM, which are one thing, since the
 * kernel's answers are datagrams: `*type` is set to SOCK_DGRAM for it. The
 * raw and packet forms need a privileged caller (-EPERM); a netlink protocol
 * is checked when the socket is made. Returns 0, or -EAFNOSUPPORT for another
 * family, -EPROTONOSUPPORT, -ESOCKTNOSUPPORT or -EINVAL for a type or a
 * protocol the family does not have.
 *
 * The families are IPv4, IPv6, packet and netlink. IPv4 has
 * two raw sockets, and the protocol tells them apart. IPPROTO_RAW is the
 * query descriptor: a DHCP client opens it and does nothing with it but use
 * the interface ioctls (BusyBox's `udhcp_read_interface`), so here it is a
 * descriptor that takes ioctls and closes, and fails to send or receive,
 * since this stack has no raw IP path. IPPROTO_ICMP is the socket `ping` and
 * `traceroute` send and receive on. AF_INET6's IPPROTO_RAW is Linux's: it
 * sends datagrams the caller built whole, header included, and receives
 * nothing. A packet socket reads every frame of its
 * protocol the machine receives and sends frames the IP layer never saw,
 * which is why it and the raw sockets need privilege. Its protocol is in
 * network byte order, as Linux takes it: one `packet_type_ok` allows, or 0
 * for none yet, as `arping` opens one before its bind gives ARP.
 */
static int32_t socket_kind(int domain, int *type, int protocol)
{
    if (domain != AF_INET && domain != AF_INET6 && domain != AF_PACKET && domain != AF_NETLINK) {
        return -EAFNOSUPPORT;
    }
    if (domain == AF_INET6) {
        if (*type == SOCK_RAW) {
            if (protocol != IPPROTO_ICMPV6 && protocol != IPPROTO_RAW)
                return -EPROTONOSUPPORT;
            if (!current_proc || !proc_privileged(current_proc))
                return -EPERM;
        } else if (*type == SOCK_STREAM || *type == SOCK_DGRAM) {
            if (!ip_protocol_ok(*type, protocol))
                return -EPROTONOSUPPORT;
        } else
            return -ESOCKTNOSUPPORT;
        return 0;
    }
    if (domain == AF_NETLINK) {
        if (*type != SOCK_RAW && *type != SOCK_DGRAM) {
            return -ESOCKTNOSUPPORT;
        }
        *type = SOCK_DGRAM;
        return 0;
    }
    if (domain == AF_INET && *type == SOCK_RAW) {
        if (protocol != IPPROTO_RAW && protocol != IPPROTO_ICMP) {
            return -EPROTONOSUPPORT;
        }
        if (!current_proc || !proc_privileged(current_proc)) {
            return -EPERM;
        }
    } else if (*type != SOCK_DGRAM && *type != SOCK_STREAM) {
        return -EINVAL;
    } else if (domain == AF_INET && !ip_protocol_ok(*type, protocol)) {
        return -EPROTONOSUPPORT;
    }
    if (domain == AF_PACKET) {
        if (!current_proc || !proc_privileged(current_proc)) {
            return -EPERM;
        }
        if (*type != SOCK_DGRAM) {
            return -ESOCKTNOSUPPORT;
        }
        /* x86 holds the network-order protocol byte-swapped. */
        if (!packet_type_ok((uint16_t)((uint16_t)protocol >> 8 | (uint16_t)protocol << 8))) {
            return -EINVAL;
        }
    }
    return 0;
}

/** socket(2): a socket of a kind `socket_kind` allows. Returns a new socket
 * fd or -errno: `socket_kind`'s, -ENOMEM, or the error of `netlink_open`,
 * `raw_register` or `fd_alloc`. */
static int32_t do_socket(int domain, int type, int protocol)
{
    struct socket *sock;
    fd_entry_t *entry;
    int32_t rc = socket_kind(domain, &type, protocol);
    int fd;

    if (rc != 0) {
        return rc;
    }
    sock = socket_alloc(type);
    if (!sock) {
        return -ENOMEM;
    }
    sock->family = (uint16_t)domain;
    if (domain == AF_INET6) {
        sock->local_ip = sock->remote_ip = net_addr6(ip6_any, 0);
    }
    if (domain != AF_NETLINK && proc_netns(current_proc) != &init_netns) {
        sock->ns = proc_netns(current_proc);
    }
    if (domain == AF_NETLINK) {
        int opened = netlink_open(sock, protocol);

        if (opened != 0) {
            socket_unref(sock);
            return opened;
        }
    }
    if (type == SOCK_RAW && (protocol == IPPROTO_ICMP || domain == AF_INET6)) {
        int reg;

        sock->protocol = (uint16_t)protocol;
        reg = raw_register(sock, (uint8_t)protocol);
        if (reg != 0) {
            socket_unref(sock);
            return reg;
        }
    }
    if (domain == AF_PACKET) {
        /* The argument is in network order and the socket keeps host order. */
        sock->protocol =
            (uint16_t)(((uint32_t)protocol & 0xffu) << 8 | (((uint32_t)protocol >> 8) & 0xffu));
    }
    sock->uid = current_proc->euid;

    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        socket_unref(sock);
        return fd;
    }

    entry = &current_proc->fds->entries[fd];
    entry->type = FD_TYPE_SOCKET;
    entry->sock = sock;
    entry->node = NULL;
    entry->ofd->offset = 0;
    entry->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    return fd;
}

/**
 * The socket operation of socketcall: `do_socket` with the creation flags
 * Linux takes in the type. SOCK_CLOEXEC and SOCK_NONBLOCK above the type's
 * low four bits are given to the new descriptor, and any other bit there is
 * EINVAL before the family is looked at, as in Linux.
 */
static int32_t socket_with_flags(int domain, int type, int protocol)
{
    uint32_t flags = (uint32_t)type & ~(uint32_t)SOCK_TYPE_MASK;
    int32_t fd;

    if (flags & ~(uint32_t)(SOCK_CLOEXEC | SOCK_NONBLOCK)) {
        return -EINVAL;
    }
    fd = do_socket(domain, type & SOCK_TYPE_MASK, protocol);
    if (fd >= 0) {
        sock_fd_flags(&current_proc->fds->entries[fd], flags);
    }
    return fd;
}

/** Bind the netlink socket `sock` to the port id and the groups of the
 * sockaddr_nl of `addrlen` bytes at user address `sa_ptr`. Returns
 * `netlink_bind`'s result, -EINVAL for a short address or another family, or
 * -EFAULT. */
static int32_t netlink_bind_user(struct socket *sock, uintptr_t sa_ptr, int addrlen)
{
    uint8_t nl[SOCKADDR_NL_LEN];

    if (addrlen < SOCKADDR_NL_LEN) {
        return -EINVAL;
    }
    if (copy_from_user(nl, (const void __user *)sa_ptr, SOCKADDR_NL_LEN) != 0) {
        return -EFAULT;
    }
    if ((nl[0] | (nl[1] << 8)) != AF_NETLINK) {
        return -EINVAL;
    }
    return netlink_bind(sock, SC_LE32(nl + 4), SC_LE32(nl + 8));
}

/**
 * Bind the TCP or UDP socket `sock` to the address `ip` and port `port`, as
 * `do_bind` describes: the address checked first, then the port's owner, a
 * datagram socket's registration and a stream's listener. Returns 0,
 * -EADDRNOTAVAIL, -EACCES, -EADDRINUSE, or -ENOMEM when there is no memory
 * to register a datagram socket's port.
 *
 * A port below 1024 is a service's, and a service is root's; otherwise any
 * user could take the port a DHCP client or an SSH server answers on and be
 * the one machines on the wire talk to. A datagram send before bind binds an
 * ephemeral source port, whose registration is dropped before the port asked
 * for is taken, or its slot in the port table would stay taken for the
 * socket's life. Port 0 means any port: a datagram socket takes a free
 * ephemeral one, as a send before bind does, and a stream is given one at the
 * bind, as on Linux, so that `getsockname` reports it before `listen`. The
 * check for a stream's listener is advisory, and is where a server learns of
 * the conflict: the rule belongs to `tcp_listen`, since `bind` and `listen`
 * are separate calls and a check made only here is bypassed by binding both
 * sockets first.
 */
static int32_t bind_inet(struct socket *sock, net_addr_t ip, uint16_t port)
{
    int rc;

    if (!net_addr_any(ip) && !net_addr_local(ip) &&
        !(ip.family == AF_INET && sock->type == SOCK_DGRAM && netdev_is_broadcast(ip.v4))) {
        return -EADDRNOTAVAIL;
    }

    if (port != 0 && port < 1024 && (!current_proc || !proc_privileged(current_proc))) {
        return -EACCES;
    }

    if (sock->type == SOCK_DGRAM) {
        if (sock->local_port != 0) {
            udp_unbind(sock);
            sock->local_port = 0;
        }
        sock->local_ip = ip;
        if (port == 0) {
            if (udp_bind_ephemeral(sock) == 0) {
                sock->local_ip = sock->family == AF_INET6 ? net_addr6(ip6_any, 0) : net_addr4(0);
                return -EADDRINUSE;
            }
            return 0;
        }
        rc = udp_bind(port, sock);
        if (rc < 0) {
            sock->local_ip = sock->family == AF_INET6 ? net_addr6(ip6_any, 0) : net_addr4(0);
            return rc == -ENOMEM ? -ENOMEM : -EADDRINUSE;
        }
    }
    if (sock->type == SOCK_STREAM) {
        net_addr_t saved = sock->local_ip;
        uint32_t held = net_lock();
        sock->local_ip = ip;
        if (!port)
            port = tcp_bind_ephemeral();
        if (!port || socket_stream_conflict(sock, port) || tcp_port_listening(sock, port)) {
            sock->local_ip = saved;
            net_unlock(held);
            return -EADDRINUSE;
        }
        net_unlock(held);
    }
    sock->local_ip = ip;
    sock->local_port = port;
    return 0;
}

/** Whether `s` is a raw or packet socket of a network namespace other than
 * the first, which reaches nothing there: its namespace's `lo` carries only
 * TCP and UDP. */
static int cut_off(const struct socket *s)
{
    return socket_outside(s) &&
           (s->type == SOCK_RAW || (s->family != AF_INET && s->family != AF_INET6));
}

/**
 * bind(2): record the local address and port from the sockaddr; a datagram
 * socket also registers them with UDP (-EADDRINUSE if taken), and a netlink
 * socket takes a port id (`netlink_bind_user`). The address must be any (0),
 * one of 127/8, an interface's, or for a datagram socket a broadcast address;
 * any other is -EADDRNOTAVAIL, as on Linux. A sockaddr shorter than the 8
 * bytes read is -EINVAL, and nothing past what the caller declared is read.
 * A TCP or UDP socket of a network namespace other than the first binds to
 * any or to 127/8 while its `lo` has an address (`socket_ns_reaches`), and
 * any other is -EADDRNOTAVAIL; a raw or packet socket there binds, and
 * nothing is delivered to it.
 *
 * A packet socket binds to an interface and has no port: the address is a
 * sockaddr_ll, and the bind is what puts the socket in the delivery table. A
 * protocol given there replaces the one `socket` was given, which is Linux's
 * rule and is how a client that opened with 0 gives the frames it reads; 0
 * leaves the socket's own. The change is made only after the bind succeeds,
 * so a bind that fails leaves the socket receiving what it received before,
 * and it discards what is queued, which arrived under the old protocol and
 * would reach a reader labelled with the new one.
 *
 * A raw socket binds to an address and has no port: the address is the one
 * of the machine's addresses it receives datagrams for, a sockaddr_in6 of at
 * least 24 bytes for an AF_INET6 one. `traceroute` binds its
 * receive socket to the address its send socket ended up with, and exits if
 * it cannot. The bind is checked against the addresses the machine has and
 * filters nothing afterwards; an address the machine does not have is
 * -EADDRNOTAVAIL rather than accepted and ignored. The query descriptor
 * binds to nothing (-EOPNOTSUPP).
 */
static int32_t do_bind(int fd, uintptr_t sa_ptr, int addrlen)
{
    fd_entry_t *entry;
    uint8_t sa[SOCKADDR_MIN];

    if (addrlen < SOCKADDR_MIN) {
        return -EINVAL;
    }

    if (copy_from_user(sa, (const void __user *)sa_ptr, SOCKADDR_MIN) != 0) {
        return -EFAULT;
    }

    entry = fd_get(current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }

    if (entry->sock->family == AF_NETLINK) {
        return netlink_bind_user(entry->sock, sa_ptr, addrlen);
    }
    if (cut_off(entry->sock)) {
        return 0; /* nothing is delivered to it */
    }

    if (entry->sock->family == AF_PACKET) {
        uint8_t sll[SOCKADDR_LL_LEN];
        int32_t ifindex;
        uint16_t proto;
        int32_t r;

        if (addrlen < SOCKADDR_LL_LEN) {
            return -EINVAL;
        }
        if (copy_from_user(sll, (const void __user *)sa_ptr, SOCKADDR_LL_LEN) != 0) {
            return -EFAULT;
        }
        ifindex = (int32_t)((uint32_t)sll[SOCKADDR_LL_IFINDEX] |
                            ((uint32_t)sll[SOCKADDR_LL_IFINDEX + 1] << 8) |
                            ((uint32_t)sll[SOCKADDR_LL_IFINDEX + 2] << 16) |
                            ((uint32_t)sll[SOCKADDR_LL_IFINDEX + 3] << 24));
        proto = SC_BE16(sll + SOCKADDR_LL_PROTOCOL);
        if (!packet_type_ok(proto)) {
            return -EINVAL;
        }
        r = packet_bind(entry->sock, (int)ifindex);
        if (r != 0) {
            return r;
        }
        if (proto != 0 && proto != entry->sock->protocol) {
            entry->sock->protocol = proto;
            packet_flush(entry->sock);
        }
        return 0;
    }

    {
        net_addr_t addr;
        uint16_t port;
        int rc = inet_endpoint_in(entry->sock, sa_ptr, addrlen, 0, &addr, &port);
        if (rc)
            return rc;
        if (sock_is_query(entry->sock))
            return -EOPNOTSUPP;
        if (!net_addr_any(addr) && !socket_ns_reaches(entry->sock, addr, 0))
            return -EADDRNOTAVAIL;
        if (entry->sock->type == SOCK_RAW) {
            if (!net_addr_any(addr) && !net_addr_local(addr))
                return -EADDRNOTAVAIL;
            entry->sock->local_ip = addr;
            return 0;
        }
        return bind_inet(entry->sock, addr, port);
    }
}

/**
 * Send the ICMPv6 message of `len` bytes at `buf_ptr` from the raw socket
 * `sock` to the sockaddr_in6 at `sa_ptr`, in the scope of its scope id.
 * The kernel sums the message, as Linux does for ICMPv6 whatever the
 * caller wrote, since the pseudo-header holds the source it chooses. The
 * socket's IPv6 options apply as they do to UDP (`udp_tx`): a group is sent
 * to with IPV6_MULTICAST_HOPS, 1 unless set, by IPV6_MULTICAST_IF when its
 * address has no scope, and any other address with IPV6_UNICAST_HOPS; the
 * class is IPV6_TCLASS; and sendmsg's control messages `opt`, NULL for
 * none, go before them. A message past the path MTU leaves in fragments.
 * A socket of IPPROTO_RAW sends instead the datagram at `buf_ptr` whole,
 * toward the address, as `ip6_tx_whole` says, whatever the options.
 *
 * Returns `len`, the error of `sockaddr_in6_in`, `icmp6_send` or
 * `ip6_tx_whole`, -EINVAL for a message too short to hold its checksum, a
 * source in `opt` that is not this machine's or an interface that
 * disagrees with the address's scope,
 * -EMSGSIZE past the largest payload, 65535 bytes, or past one 1500-byte
 * datagram for IPPROTO_RAW, -ENOMEM when no block holds a message past
 * `raw_send_buf`, or -EFAULT. The copy runs with interrupts on, and the
 * send under the network lock, as an IPv4 raw send does.
 */
static int32_t raw6_sendto(struct socket *sock, uintptr_t buf_ptr, int len, uintptr_t sa_ptr,
                           int addrlen, const struct net_txopt *opt)
{
    uint8_t dst[16], src[16];
    uint8_t *buf = raw_send_buf;
    uint32_t netflags;
    int whole = sock->protocol == IPPROTO_RAW;
    int scope, r, tclass;

    r = sockaddr_in6_in(sa_ptr, addrlen, dst, &scope);
    if (r != 0) {
        return r;
    }
    if (!scope && ip6_link_scoped(dst)) {
        scope = sock->bound_device;
    }
    if (opt && opt->ifindex) {
        if (scope && scope != opt->ifindex) {
            return -EINVAL;
        }
        scope = opt->ifindex;
    } else if (!scope && ip6_is_multicast(dst)) {
        scope = sock->mif6;
    }
    net_addr_to6(sock->local_ip, src);
    if (sock->local_ip.family != AF_INET6) {
        k_memset(src, 0, sizeof(src));
    }
    if (opt && opt->src.family == AF_INET6 && !net_addr_any(opt->src)) {
        net_addr_t from = opt->src;

        from.scope = 0;
        if (!net_addr_local(from)) {
            return -EINVAL;
        }
        k_memcpy(src, opt->src.v6, sizeof(src));
    }
    tclass = opt && opt->tclass >= 0 ? opt->tclass : sock->tclass6;
    if (len < 4) {
        return -EINVAL;
    }
    if (len > (whole ? (int)IP6_MTU : (int)IP6_PAYLOAD_MAX)) {
        return -EMSGSIZE;
    }
    if (len > (int)sizeof(raw_send_buf)) {
        buf = kmalloc((size_t)len);
        if (!buf) {
            return -ENOMEM;
        }
    }
    if (copy_from_user(buf, (const void __user *)buf_ptr, (uint32_t)len) != 0) {
        r = -EFAULT;
    } else {
        netflags = net_lock();
        if (whole) {
            r = ip6_tx_whole(buf, (uint32_t)len, dst, scope);
        } else {
            uint8_t hlim =
                opt && opt->hlim >= 0 ? (uint8_t)opt->hlim : socket_hops6(sock, dst, scope);

            r = icmp6_send_opts(src, dst, scope, buf, (uint16_t)len, hlim,
                                IP6_TXF_TCLASS(tclass) | (sock->mloop6 ? IP6_TXF_LOOP : 0));
        }
        net_unlock(netflags);
    }
    if (buf != raw_send_buf) {
        kfree(buf);
    }
    return r < 0 ? r : (int32_t)len;
}

/**
 * Send `len` bytes at `buf`, of at most one UDP packet, from the datagram
 * socket of `entry` to the sockaddr at `sa_ptr`, or with none to the address
 * it is connected to. One send path, so the broadcast permission and the
 * ephemeral bind are the same for sendto, sendmsg and a write to a connected
 * socket. A connected datagram socket needs no destination: `send` is
 * `sendto` with none, which is what musl's send() issues. `opt` is
 * sendmsg's control messages, or NULL. Returns what
 * `dgram_send` returns, -EDESTADDRREQ with no destination on a socket that is
 * not connected, -EINVAL for a sockaddr shorter than 8 bytes, or -EFAULT.
 */
static int32_t udp_sendto(fd_entry_t *entry, const uint8_t *buf, int len, uintptr_t sa_ptr,
                          int addrlen, const struct net_txopt *opt)
{
    net_addr_t dst;
    uint16_t port;
    int rc;
    if (!sa_ptr || !addrlen) {
        if (!entry->sock->connected)
            return -EDESTADDRREQ;
        dst = entry->sock->remote_ip;
        port = entry->sock->remote_port;
    } else {
        rc = inet_endpoint_in(entry->sock, sa_ptr, addrlen, opt ? opt->ifindex : 0, &dst, &port);
        if (rc)
            return rc;
    }
    return dgram_send_opt(entry->sock, dst, port, buf, (uint16_t)len, opt);
}

/** The length of the sockaddr a receive on `sock` reports its sender in. */
static uint32_t sockaddr_len_of(const struct socket *sock)
{
    if (sock->family == AF_PACKET) {
        return (uint32_t)SOCKADDR_LL_LEN;
    }
    if (sock->family == AF_NETLINK) {
        return (uint32_t)SOCKADDR_NL_LEN;
    }
    if (sock->family == AF_INET6) {
        return (uint32_t)SOCKADDR_IN6_LEN;
    }
    return (uint32_t)SOCKADDR_IN_LEN;
}

/* Policy: a netlink request is at most 64 KiB, far past the few dozen bytes
 * of the requests the socket answers. */
#define NETLINK_SEND_MAX 65536

/** Send the netlink message of `len` bytes at buf from the socket `sock`,
 * holding a reference to it, since a generic netlink request may sleep and
 * another thread may close the descriptor meanwhile. Returns
 * `netlink_send`'s result. */
static int32_t netlink_send_held(struct socket *sock, const uint8_t *buf, uint32_t len)
{
    int32_t rc;

    socket_ref(sock);
    rc = netlink_send(sock, buf, len);
    socket_unref(sock);
    return rc;
}

/** Send the netlink message of `len` bytes at user address `buf_ptr`.
 * Returns `netlink_send`'s result, -EMSGSIZE past `NETLINK_SEND_MAX`,
 * -ENOMEM or -EFAULT. */
static int32_t netlink_send_user(struct socket *sock, uintptr_t buf_ptr, int len)
{
    uint8_t *copy;
    int32_t rc;

    if (len > NETLINK_SEND_MAX) {
        return -EMSGSIZE;
    }
    copy = kmalloc(len ? (uint32_t)len : 1u);
    if (!copy) {
        return -ENOMEM;
    }
    if (copy_from_user(copy, (const void __user *)buf_ptr, (uint32_t)len) != 0) {
        kfree(copy);
        return -EFAULT;
    }
    rc = netlink_send_held(sock, copy, (uint32_t)len);
    kfree(copy);
    return rc;
}

/** Copy the sockaddr_nl `nl` to the caller's (sa_ptr, salen_ptr), truncated
 * to its capacity, with the true length written back. Returns 0 or -EFAULT. */
static int32_t netlink_addr_to_user(const uint8_t *nl, uintptr_t sa_ptr, uintptr_t salen_ptr)
{
    uint32_t cap, give;

    if (copy_from_user(&cap, (const void __user *)salen_ptr, sizeof(cap)) != 0) {
        return -EFAULT;
    }
    give = cap < (uint32_t)SOCKADDR_NL_LEN ? cap : (uint32_t)SOCKADDR_NL_LEN;
    if (give > 0 && copy_to_user((void __user *)sa_ptr, nl, give) != 0) {
        return -EFAULT;
    }
    cap = (uint32_t)SOCKADDR_NL_LEN;
    return copy_to_user((void __user *)salen_ptr, &cap, sizeof(cap)) == 0 ? 0 : -EFAULT;
}

/**
 * sendto(2): a stream socket ignores the destination and behaves like send; a
 * datagram socket sends to the sockaddr, binding an ephemeral source port
 * first. Returns the bytes sent or -errno.
 *
 * The query descriptor takes ioctls and nothing else: it has no protocol to
 * send under, and a caller given success here would wait for a reply that
 * cannot arrive (-EOPNOTSUPP). A netlink socket's message goes to the kernel,
 * the only destination it has. A connected stream socket sends to its peer
 * whatever the destination, or with none, which is the path musl's send()
 * takes: sendto with a null address. A datagram socket's payload must fit
 * one UDP datagram, sent in fragments past one frame: a larger one is
 * -EMSGSIZE before its length is cut to the 16 bits `udp_sendto` passes on.
 *
 * A raw socket sends the ICMP message the caller built, checksum included:
 * this layer supplies the IPv4 header and rewrites nothing, since a program
 * sending a wrong checksum on purpose is testing something. A message past
 * one frame leaves in fragments under the socket's IP_MTU_DISCOVER, up to
 * the largest IPv4 payload. An AF_INET6 one sends as `raw6_sendto` says.
 * The copy into `raw_send_buf`, or a block of the heap, runs with interrupts
 * on, since bringing in the caller's pages may sleep; the send then holds the
 * network lock against a receive
 * interrupt that would build a datagram in the same transmit buffers, the
 * rule of net.h, which `udp_tx` keeps for the same reason. A packet socket
 * sends an IP packet the caller built to the hardware address in its
 * sockaddr_ll, and this layer adds the Ethernet header and nothing else; an
 * interface index there is the one the frame leaves by, and 0 leaves the
 * choice to the bind. The stream, packet and UDP paths read the payload where
 * it lies, in the range `user_buffer_ok` checked, as `__force` casts mark.
 * `opt` is the control messages of a sendmsg(2) with one buffer, NULL for
 * none, which a UDP or raw IPv6 send applies.
 */
static int32_t do_sendto_opt(int fd, uintptr_t buf_ptr, int len, int flags, uintptr_t sa_ptr,
                             int addrlen, const struct net_txopt *opt)
{
    fd_entry_t *entry;

    if (len < 0 || !user_buffer_ok(buf_ptr, (uint32_t)len, 0)) {
        return -EFAULT;
    }

    entry = fd_get(current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }

    if (sock_is_query(entry->sock)) {
        return -EOPNOTSUPP;
    }

    if (entry->sock->family == AF_NETLINK) {
        return netlink_send_user(entry->sock, buf_ptr, len);
    }
    if (cut_off(entry->sock)) {
        return -ENETUNREACH;
    }

    if (entry->sock->type == SOCK_RAW) {
        uint8_t raw_sa[SOCKADDR_IN_LEN];
        uint8_t *msg;
        uint32_t raw_dst;
        int r;

        if (!sa_ptr || addrlen < SOCKADDR_MIN) {
            return -EDESTADDRREQ;
        }
        if (entry->sock->family == AF_INET6) {
            return raw6_sendto(entry->sock, buf_ptr, len, sa_ptr, addrlen, opt);
        }
        k_memset(raw_sa, 0, sizeof(raw_sa));
        if (copy_from_user(raw_sa, (const void __user *)sa_ptr, SOCKADDR_MIN) != 0) {
            return -EFAULT;
        }
        if ((uint16_t)((uint16_t)raw_sa[0] | ((uint16_t)raw_sa[1] << 8)) != AF_INET) {
            return -EAFNOSUPPORT;
        }
        if (len > IP_PAYLOAD_MAX) {
            return -EMSGSIZE;
        }
        raw_dst = SC_BE32(raw_sa + 4);
        msg = raw_send_buf;
        if (len > (int)sizeof(raw_send_buf)) {
            msg = kmalloc((size_t)len);
            if (!msg) {
                return -ENOMEM;
            }
        }
        if (copy_from_user(msg, (const void __user *)buf_ptr, (uint32_t)len) != 0) {
            r = -EFAULT;
        } else {
            uint32_t netflags = net_lock();
            r = ip_tx_opts(0, raw_dst, (uint8_t)entry->sock->protocol, msg, (uint16_t)len,
                           entry->sock->ip_ttl, entry->sock->pmtudisc);
            raw_count_sent(entry->sock, r, msg, len);
            net_unlock(netflags);
        }
        if (msg != raw_send_buf) {
            kfree(msg);
        }
        if (r < 0) {
            return r;
        }
        return (int32_t)len;
    }

    if (entry->sock->type == SOCK_STREAM) {
        int nonblock =
            (flags & MSG_DONTWAIT) || (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));
        return (int32_t)socket_tcp_send(
            entry->sock, (const uint8_t __force *)(const void __user *)buf_ptr, len, nonblock);
    }

    if (entry->sock->family == AF_PACKET) {
        uint8_t sll[SOCKADDR_LL_LEN];
        uint16_t proto;
        int32_t ifindex;

        if (len > ETH_MTU) {
            return -EMSGSIZE;
        }
        if (addrlen < SOCKADDR_LL_LEN) {
            return -EINVAL;
        }
        if (copy_from_user(sll, (const void __user *)sa_ptr, SOCKADDR_LL_LEN) != 0) {
            return -EFAULT;
        }
        if (sll[SOCKADDR_LL_HALEN] != 6) {
            return -EINVAL;
        }
        proto = SC_BE16(sll + SOCKADDR_LL_PROTOCOL);
        if (proto == 0) {
            proto = entry->sock->protocol;
        }
        ifindex = (int32_t)((uint32_t)sll[SOCKADDR_LL_IFINDEX] |
                            ((uint32_t)sll[SOCKADDR_LL_IFINDEX + 1] << 8) |
                            ((uint32_t)sll[SOCKADDR_LL_IFINDEX + 2] << 16) |
                            ((uint32_t)sll[SOCKADDR_LL_IFINDEX + 3] << 24));
        return (int32_t)packet_send(entry->sock, (int)ifindex, sll + SOCKADDR_LL_ADDR, proto,
                                    (const uint8_t __force *)(const void __user *)buf_ptr,
                                    (uint16_t)len);
    }

    if (len > 0xFFFF) {
        return -EMSGSIZE;
    }

    return udp_sendto(entry, (const uint8_t __force *)(const void __user *)buf_ptr, len, sa_ptr,
                      addrlen, opt);
}

/** sendto(2): `do_sendto_opt` with no control messages. */
static int32_t do_sendto(int fd, uintptr_t buf_ptr, int len, int flags, uintptr_t sa_ptr,
                         int addrlen)
{
    return do_sendto_opt(fd, buf_ptr, len, flags, sa_ptr, addrlen, 0);
}

/**
 * recvfrom(2): a stream socket behaves like recv (no source writeback); a
 * datagram socket reports the source address, truncated to the caller's
 * capacity. An address with no length pointer is EFAULT before anything is
 * taken, so the message stays queued; Linux fails the call only after it
 * has taken the message. Honors MSG_PEEK, MSG_DONTWAIT and O_NONBLOCK, the
 * last a status flag of the open file description, which dup and fork share.
 * Returns the bytes received or -errno.
 *
 * Where the source address will land is settled before a datagram is taken:
 * taking one destroys it, and a fault found afterwards would report an error
 * for a message that no longer exists. The caller's capacity is read once and
 * kept, so that a caller whose length word overlaps its own receive buffer
 * cannot change it under the writeback. What is checked is what will be
 * written: twenty bytes for a packet socket's sockaddr_ll, sixteen for an
 * AF_INET sender, twenty-eight for an AF_INET6 one, twelve for a netlink
 * socket's sockaddr_nl. A receive that
 * fails leaves the address arguments alone, since there is no sender to
 * report. The address is then written truncated to the capacity read before
 * the datagram was taken: for a packet socket the link address the frame came
 * from, for a netlink socket the kernel, and for an AF_INET one the sender.
 *
 * The query descriptor takes ioctls and nothing else: nothing is delivered to
 * a descriptor with no protocol, and a receive that waited here would wait
 * for a datagram that cannot arrive (-EOPNOTSUPP). A connected stream socket
 * receives as recv does, from the connection's peer, so the address is not
 * written and a null sockaddr is fine; this is the path musl's recv() takes,
 * recvfrom with a null address. The receive writes into the buffer
 * `user_buffer_ok` checked, as the `__force` casts mark.
 */
static int32_t do_recvfrom(int fd, uintptr_t buf_ptr, int len, int flags, uintptr_t sa_ptr,
                           uintptr_t salen_ptr)
{
    fd_entry_t *entry;
    net_addr_t src_ip = net_addr4(0);
    uint16_t src_port = 0;
    uint32_t addr_len = 0;
    uint32_t addr_give = 0;
    int want_addr;
    int bytes;
    int nonblock;
    int peek = (flags & MSG_PEEK) != 0;
    int how = (peek ? SOCK_RECV_PEEK : 0) | ((flags & MSG_TRUNC) ? SOCK_RECV_TRUNC : 0);

    if (len < 0 || !user_buffer_ok(buf_ptr, (uint32_t)len, 1)) {
        return -EFAULT;
    }
    entry = fd_get(current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    want_addr = sa_ptr != 0;
    if (want_addr) {
        uint32_t cap = 0;

        if (!salen_ptr || !user_buffer_ok(salen_ptr, sizeof(cap), 1) ||
            copy_from_user(&cap, (const void __user *)salen_ptr, sizeof(cap)) != 0) {
            return -EFAULT;
        }
        addr_len = sockaddr_len_of(entry->sock);
        addr_give = cap < addr_len ? cap : addr_len;
        if (addr_give > 0 && !user_buffer_ok(sa_ptr, addr_give, 1)) {
            return -EFAULT;
        }
    }

    if (sock_is_query(entry->sock)) {
        return -EOPNOTSUPP;
    }

    nonblock = (flags & MSG_DONTWAIT) || (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));

    if (entry->sock->type == SOCK_STREAM) {
        if (nonblock) {
            return (int32_t)socket_tcp_try_recv(
                entry->sock, (uint8_t __force *)(void __user *)buf_ptr, len, peek);
        }
        return (int32_t)socket_tcp_recv(entry->sock, (uint8_t __force *)(void __user *)buf_ptr, len,
                                        peek);
    }

    if (nonblock) {
        bytes = socket_try_recv(entry->sock, (uint8_t __force *)(void __user *)buf_ptr, len,
                                &src_ip, &src_port, how);
    } else {
        bytes = socket_recv(entry->sock, (uint8_t __force *)(void __user *)buf_ptr, len, &src_ip,
                            &src_port, how);
    }

    if (bytes < 0) {
        return bytes;
    }

    if (want_addr) {
        uint8_t sa[SOCKADDR_IN6_LEN];

        k_memset(sa, 0, sizeof(sa));
        if (entry->sock->family == AF_NETLINK) {
            sa[0] = (uint8_t)AF_NETLINK; /* the kernel, port id 0 */
        } else if (entry->sock->family == AF_PACKET) {
            if (packet_last_addr(entry->sock, sa) == 0) {
                addr_len = 0;
                addr_give = 0;
            }
        } else {
            inet_endpoint_out(sa, entry->sock->family, src_ip, src_port);
        }
        if (addr_give > 0 && copy_to_user((void __user *)sa_ptr, sa, addr_give) != 0) {
            return -EFAULT;
        }
        if (copy_to_user((void __user *)salen_ptr, &addr_len, sizeof(addr_len)) != 0) {
            return -EFAULT;
        }
    }
    return bytes;
}

/** SC_PING: an ICMP echo to `dst_ip`; the round-trip time in ticks, or the
 * error of `icmp_ping`. */
static int32_t do_ping(uint32_t dst_ip)
{
    return (int32_t)icmp_ping(dst_ip);
}

/**
 * SC_PING6: an ICMPv6 echo to the 16 bytes at `args[0]`, in the scope of
 * the interface of index `args[1]`, 0 for none, with `args[2]` bytes of data
 * and the hop limit `args[3]`. Returns the reply's ICMPv6 length in the low
 * 16 bits and its hop limit in the 8 above them, or -EINVAL for a hop limit
 * outside 1 to 255, -ENODEV for an index no interface has, -EMSGSIZE,
 * -EFAULT, or the error of `icmp_ping6_data`.
 */
static int32_t do_ping6(const unsigned long *args)
{
    uint8_t dst[16];
    uint16_t got = 0;
    uint8_t got_hlim = 0;
    int rc;

    if (args[3] < 1 || args[3] > 255) {
        return -EINVAL;
    }
    if (args[1] && (args[1] > 0x7ffffffful || !net_ns_seen() || !netdev_by_index((int)args[1]))) {
        return -ENODEV;
    }
    if (args[2] != (uint32_t)args[2]) {
        return -EMSGSIZE;
    }
    if (copy_from_user(dst, (const void __user *)args[0], sizeof(dst)) != 0) {
        return -EFAULT;
    }
    rc = icmp_ping6_data(dst, (int)args[1], (uint32_t)args[2], (uint8_t)args[3], &got, &got_hlim);
    return rc < 0 ? rc : (int32_t)((uint32_t)got | (uint32_t)got_hlim << 16);
}

/** Whether the handshake of `sock` has completed: ESTABLISHED, or
 * CLOSE_WAIT, since a peer's FIN does not undo the completed handshake. */
static int connect_established(struct socket *sock)
{
    return sock->tcb && (sock->tcb->state == TCP_ESTABLISHED || sock->tcb->state == TCP_CLOSE_WAIT);
}

/** The wait predicate of connect: true once the handshake has completed or
 * the connection has failed. */
static int connect_done(void *arg)
{
    struct socket *sock = (struct socket *)arg;
    return sock->tcb == NULL || connect_established(sock) || sock->tcb->state == TCP_CLOSED;
}

/** Wait predicate: a listening socket has a connection queued for accept. */
static int accept_ready(void *arg)
{
    struct socket *sock = (struct socket *)arg;
    return sock->accept_head != sock->accept_tail;
}

/**
 * connect(2): start the TCP handshake to the sockaddr. A blocking socket waits
 * it out (-EINTR on a signal, -ECONNREFUSED when the handshake fails or times
 * out); a non-blocking one returns 0 or -EINPROGRESS, and the caller polls
 * for the end. A datagram socket records its peer instead, and a netlink
 * socket has its one peer, the kernel, already. Returns 0 or -errno.
 *
 * The descriptor is resolved before its argument is read, so that a call on
 * something that is not a socket reports that instead of whatever was wrong
 * with an address it was never going to use. A raw or packet socket
 * has no peer to connect to (-EOPNOTSUPP): a packet socket is bound to an
 * interface and sends to a hardware address each time, and the datagram path
 * would put it in the UDP port table, which its teardown never unbinds from.
 *
 * The family is read first and on its own, because a disconnect is a family
 * and nothing else: `connect(fd, &unspec, sizeof(sa_family_t))` is how a
 * program lets go of a peer, and requiring a whole address for it would leave
 * that program connected. Two bytes are enough for a disconnect and only for
 * a disconnect, and only a datagram socket has one to do, since a stream's
 * connection ends by closing it; any other family must give a whole address,
 * or the bytes past the family would be this function's stack.
 *
 * A datagram socket's connect records where it will send and whom it will
 * hear from; there is no handshake and nothing to wait for, and AF_UNSPEC
 * undoes it. Something must answer, so the socket needs a source port: the
 * same ephemeral bind a send before bind would take. The bind and the peer
 * are published together, under one hold of the network lock. A bind that
 * landed first would put the socket in the port table with no peer recorded,
 * and a datagram arriving in that interval would be admitted from anyone; a
 * reconnect that published a new port beside an old address would admit a
 * sender that is neither the old peer nor the new one.
 *
 * A stream socket that is already listening, connecting or connected keeps
 * its TCB (-EALREADY while the handshake is in progress, -EISCONN
 * otherwise); a second `tcp_connect` would allocate another and leak the
 * first, whose stale delivery could reach a socket that reused it. The error
 * of `tcp_connect` is returned as it is: the refusal of a closed port comes
 * from the handshake, not from here, so turning a full pool or a duplicate
 * pairing into -ECONNREFUSED would report a local failure as the peer's
 * answer.
 *
 * In a network namespace other than the first, a socket reaches only 127/8,
 * while the namespace's `lo` is up with its address, and any other peer is
 * -ENETUNREACH, as is any peer of a raw socket there.
 */
static int32_t do_connect(int fd, uintptr_t sa_ptr, int addrlen)
{
    fd_entry_t *entry;
    net_addr_t dst_ip;
    uint16_t dst_port;
    uint16_t family;
    uint8_t sa[SOCKADDR_MIN];
    int rc;

    entry = fd_get(current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    if (entry->sock->type == SOCK_RAW) {
        return -EOPNOTSUPP;
    }
    if (entry->sock->family == AF_NETLINK) {
        return 0;
    }
    if (entry->sock->family == AF_PACKET) {
        return -EOPNOTSUPP;
    }
    if (cut_off(entry->sock)) {
        return -ENETUNREACH;
    }

    k_memset(sa, 0, sizeof(sa));
    if (addrlen < 2) {
        return -EINVAL;
    }
    if (copy_from_user(sa, (const void __user *)sa_ptr, 2) != 0) {
        return -EFAULT;
    }
    family = (uint16_t)((uint16_t)sa[0] | ((uint16_t)sa[1] << 8));
    if (family == 0) {
        if (entry->sock->type != SOCK_DGRAM)
            return -EAFNOSUPPORT;
        entry->sock->connected = 0;
        entry->sock->remote_ip =
            entry->sock->family == AF_INET6 ? net_addr6(ip6_any, 0) : net_addr4(0);
        entry->sock->remote_port = 0;
        return 0;
    }
    rc = inet_endpoint_in(entry->sock, sa_ptr, addrlen, 0, &dst_ip, &dst_port);
    if (rc)
        return rc;
    if (!socket_ns_reaches(entry->sock, dst_ip, 1))
        return -ENETUNREACH;
    if (entry->sock->type == SOCK_DGRAM) {
        net_addr_t from;
        uint32_t held = net_lock();
        rc = net_addr_source(entry->sock->local_ip, dst_ip, &from);
        if (rc < 0) {
            net_unlock(held);
            return rc;
        }
        if (!entry->sock->local_port && !udp_bind_ephemeral(entry->sock)) {
            net_unlock(held);
            return -EAGAIN;
        }
        entry->sock->remote_ip = dst_ip;
        entry->sock->remote_port = dst_port;
        entry->sock->connected = 1;
        net_unlock(held);
        return 0;
    }
    if (entry->sock->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }
    if (entry->sock->listening || entry->sock->tcb) {
        return (entry->sock->tcb && entry->sock->tcb->state == TCP_SYN_SENT) ? -EALREADY : -EISCONN;
    }

    {
        int opened = tcp_connect(entry->sock, dst_ip, dst_port);

        if (opened < 0) {
            return opened;
        }
    }

    if (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK)) {
        if (connect_established(entry->sock)) {
            return 0;
        }
        return -EINPROGRESS;
    }

    rc = wq_wait_event(&entry->sock->waiters, connect_done, entry->sock);
    if (rc < 0) {
        return rc;
    }

    if (!connect_established(entry->sock)) {
        return -ECONNREFUSED;
    }
    return 0;
}

/** listen(2): mark a stream socket passive and open its LISTEN TCB. Returns
 * 0, -EOPNOTSUPP for a socket that is not a stream, -EINVAL for one already
 * listening or connected, or the error of `tcp_listen`. */
static int32_t do_listen(int fd, int backlog)
{
    fd_entry_t *entry;

    (void)backlog; /* a fixed-size accept queue is used */

    entry = fd_get(current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    if (entry->sock->type == SOCK_RAW) {
        return -EOPNOTSUPP;
    }
    if (entry->sock->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }
    if (entry->sock->tcb || entry->sock->listening) {
        return -EINVAL; /* already listening or connected: a fresh TCB would leak the old one */
    }
    {
        int rc = tcp_listen(entry->sock, entry->sock->local_port);
        if (rc == 0) {
            entry->sock->listening = 1;
        }
        return rc;
    }
}

/**
 * accept(2) and accept4(2): dequeue a completed connection (blocking, or
 * -EAGAIN on a non-blocking listener), give a new descriptor for it the
 * creation flags `flags`, and report the peer address. A flag other than
 * SOCK_CLOEXEC and SOCK_NONBLOCK is EINVAL. Raw sockets return EOPNOTSUPP;
 * other sockets must be listening streams.
 *
 * The descriptor is reserved before the connection is dequeued, so a caller
 * with none free leaves it queued, and gets its socket only after the
 * address is copied out: the copy can sleep, and a thread sharing the table
 * must find nothing it could use or close. A copy that fails frees the slot
 * and the connection, and so does an address with no length pointer, which
 * is EFAULT, as in Linux. The address is truncated to the caller's declared
 * length (`copy_sockaddr_to_user`). The queued socket has the reference it
 * was queued with (`socket_tcp_accept`), which the new descriptor takes over.
 */
static int32_t do_accept(int fd, uintptr_t sa_ptr, uintptr_t salen_ptr, uint32_t flags)
{
    fd_entry_t *entry;
    fd_entry_t *new_entry;
    struct socket *listen_sock;
    struct socket *conn_sock;
    int new_fd;

    if (flags & ~(uint32_t)(SOCK_CLOEXEC | SOCK_NONBLOCK)) {
        return -EINVAL;
    }
    entry = fd_get(current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    if (entry->sock->type == SOCK_RAW) {
        return -EOPNOTSUPP;
    }
    listen_sock = entry->sock;
    if (listen_sock->type != SOCK_STREAM || !listen_sock->listening) {
        return -EINVAL;
    }

    if (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK) &&
        listen_sock->accept_head == listen_sock->accept_tail) {
        return -EAGAIN;
    }

    {
        int rc = wq_wait_event(&listen_sock->waiters, accept_ready, listen_sock);
        if (rc < 0) {
            return rc;
        }
    }

    new_fd = fd_alloc(current_proc->fds);
    if (new_fd < 0) {
        return new_fd;
    }
    conn_sock = listen_sock->accept_queue[listen_sock->accept_head];
    listen_sock->accept_head = (listen_sock->accept_head + 1) & (SOCK_ACCEPT_BACKLOG - 1);

    if ((sa_ptr && !salen_ptr) ||
        copy_sockaddr_to_user(sa_ptr, salen_ptr, conn_sock->remote_port, conn_sock->remote_ip,
                              conn_sock->family) != 0) {
        fd_free(current_proc->fds, new_fd);
        socket_unref(conn_sock);
        return -EFAULT;
    }
    new_entry = &current_proc->fds->entries[new_fd];
    new_entry->sock = conn_sock;
    new_entry->node = NULL;
    new_entry->ofd->offset = 0;
    new_entry->flags = FD_FLAG_READ | FD_FLAG_WRITE;
    sock_fd_flags(new_entry, flags);
    new_entry->type = FD_TYPE_SOCKET;
    return new_fd;
}

/** send(2): write to the socket's connected peer. On a stream that is the
 * connection's; any other socket sends as sendto(2) with no address does,
 * which is what POSIX makes send, and what glibc's DNS resolver uses on a
 * connected UDP socket through subcall 9. A stream blocks for room in its
 * send ring unless MSG_DONTWAIT or O_NONBLOCK is set, and `socket_tcp_send`
 * reads the payload from the buffer `user_buffer_ok` checked. Returns the
 * bytes sent or -errno. */
static int32_t do_send(int fd, uintptr_t buf_ptr, int len, int flags)
{
    fd_entry_t *entry;
    int nonblock;

    if (len < 0 || !user_buffer_ok(buf_ptr, (uint32_t)len, 0)) {
        return -EFAULT;
    }

    entry = fd_get(current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    if (entry->sock->type != SOCK_STREAM) {
        return do_sendto(fd, buf_ptr, len, flags, 0, 0);
    }
    nonblock = (flags & MSG_DONTWAIT) || (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));
    return (int32_t)socket_tcp_send(
        entry->sock, (const uint8_t __force *)(const void __user *)buf_ptr, len, nonblock);
}

/**
 * recv(2): read from a connected stream socket, or one message from a
 * datagram socket. Honors MSG_PEEK, MSG_DONTWAIT and O_NONBLOCK, and on a
 * datagram socket MSG_TRUNC, which answers the datagram's whole length.
 * Returns the bytes received or -errno, -EOPNOTSUPP for a raw socket.
 *
 * A datagram socket reads one message with no sender reported, which is what
 * `recv` is: `recvfrom` without the address. Failing it here would leave one
 * of the four ways of reading a socket disagreeing with the other three about
 * whether the same queued datagram exists. The receive writes into the buffer
 * `user_buffer_ok` checked.
 */
static int32_t do_recv(int fd, uintptr_t buf_ptr, int len, int flags)
{
    fd_entry_t *entry;
    int nonblock;
    int peek = (flags & MSG_PEEK) != 0;

    if (len < 0 || !user_buffer_ok(buf_ptr, (uint32_t)len, 1)) {
        return -EFAULT;
    }

    entry = fd_get(current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    if (entry->sock->type == SOCK_RAW) {
        return -EOPNOTSUPP;
    }
    nonblock = (flags & MSG_DONTWAIT) || (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));

    if (entry->sock->type == SOCK_DGRAM) {
        int how = (peek ? SOCK_RECV_PEEK : 0) | ((flags & MSG_TRUNC) ? SOCK_RECV_TRUNC : 0);

        if (nonblock) {
            return (int32_t)socket_try_recv(entry->sock, (uint8_t __force *)(void __user *)buf_ptr,
                                            len, 0, 0, how);
        }
        return (int32_t)socket_recv(entry->sock, (uint8_t __force *)(void __user *)buf_ptr, len, 0,
                                    0, how);
    }
    if (entry->sock->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }
    if (nonblock) {
        return (int32_t)socket_tcp_try_recv(entry->sock, (uint8_t __force *)(void __user *)buf_ptr,
                                            len, peek);
    }
    return (int32_t)socket_tcp_recv(entry->sock, (uint8_t __force *)(void __user *)buf_ptr, len,
                                    peek);
}

/**
 * Report the local address: a TCP control block's, else the address the
 * socket bound, except that a connected datagram socket bound to any address
 * reports the one its datagrams leave from, as Linux does. Packet sockets
 * report their binding's interface; an unbound socket or an all-interface
 * binding has index zero and no MAC. A netlink socket reports its own
 * sockaddr_nl. Unlike accept and recvfrom, which take a null address as no
 * address to report (`copy_sockaddr_to_user`), getsockname needs both
 * pointers, and is -EFAULT without them. Returns 0 or -errno, -EOPNOTSUPP for
 * a raw socket.
 */
static int32_t do_getsockname(int fd, uintptr_t sa_ptr, uintptr_t salen_ptr)
{
    fd_entry_t *entry = fd_get(current_proc->fds, fd);
    uint32_t flags;
    net_addr_t local_ip;

    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    if (entry->sock->type == SOCK_RAW) {
        return -EOPNOTSUPP;
    }
    if (!sa_ptr || !salen_ptr) {
        return -EFAULT;
    }
    if (entry->sock->family == AF_NETLINK) {
        uint8_t nl[SOCKADDR_NL_LEN];

        netlink_name(entry->sock, nl);
        return netlink_addr_to_user(nl, sa_ptr, salen_ptr);
    }
    if (entry->sock->family == AF_PACKET) {
        uint8_t sll[SOCKADDR_LL_LEN];
        uint32_t caller_len, copy_len;
        int ifindex = packet_ifindex(entry->sock);
        struct netdev *nd = netdev_by_index(ifindex);

        if (copy_from_user(&caller_len, (const void __user *)salen_ptr, sizeof(caller_len)) != 0) {
            return -EFAULT;
        }
        k_memset(sll, 0, sizeof(sll));
        sll[0] = (uint8_t)(AF_PACKET & 0xff);
        sll[1] = (uint8_t)((AF_PACKET >> 8) & 0xff);
        sll[SOCKADDR_LL_PROTOCOL] = (uint8_t)((entry->sock->protocol >> 8) & 0xff);
        sll[SOCKADDR_LL_PROTOCOL + 1] = (uint8_t)(entry->sock->protocol & 0xff);
        SC_WLE32(sll + SOCKADDR_LL_IFINDEX, (uint32_t)ifindex);
        if (nd) {
            sll[SOCKADDR_LL_HATYPE] =
                (uint8_t)(nd->loopback ? NETDEV_LOOPBACK_TYPE & 0xff : ARPHRD_ETHER);
            sll[SOCKADDR_LL_HATYPE + 1] = (uint8_t)(nd->loopback ? NETDEV_LOOPBACK_TYPE >> 8 : 0);
            sll[SOCKADDR_LL_HALEN] = 6;
            k_memcpy(sll + SOCKADDR_LL_ADDR, nd->mac, 6);
        }
        copy_len = caller_len < (uint32_t)SOCKADDR_LL_LEN ? caller_len : (uint32_t)SOCKADDR_LL_LEN;
        if (copy_len > 0 && copy_to_user((void __user *)sa_ptr, sll, copy_len) != 0) {
            return -EFAULT;
        }
        caller_len = (uint32_t)SOCKADDR_LL_LEN; /* the true length, as Linux reports */
        if (copy_to_user((void __user *)salen_ptr, &caller_len, sizeof(caller_len)) != 0) {
            return -EFAULT;
        }
        return 0;
    }
    flags = net_lock();
    if (entry->sock->tcb) {
        local_ip = entry->sock->tcb->local_ip;
    } else if (net_addr_any(entry->sock->local_ip) && entry->sock->connected) {
        int rc = net_addr_source(entry->sock->local_ip, entry->sock->remote_ip, &local_ip);
        if (rc < 0) {
            net_unlock(flags);
            return rc;
        }
    } else {
        local_ip = entry->sock->local_ip;
    }
    net_unlock(flags);
    return copy_sockaddr_to_user(sa_ptr, salen_ptr, entry->sock->local_port, local_ip,
                                 entry->sock->family);
}

/** getpeername: the connected peer's address (an established stream socket
 * or a datagram socket that connect(2) gave a peer). A stream has a peer
 * once its handshake completes, and not as a listener, in SYN_SENT or once
 * closed; a datagram socket has one, with no handshake, once connected, and a
 * netlink socket's is the kernel. Returns 0, -ENOTCONN with no peer, -EFAULT
 * without both pointers, or -EOPNOTSUPP for a raw socket. */
static int32_t do_getpeername(int fd, uintptr_t sa_ptr, uintptr_t salen_ptr)
{
    fd_entry_t *entry = fd_get(current_proc->fds, fd);

    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    if (entry->sock->type == SOCK_RAW) {
        return -EOPNOTSUPP;
    }
    if (entry->sock->family == AF_NETLINK) {
        uint8_t nl[SOCKADDR_NL_LEN];

        if (!sa_ptr || !salen_ptr) {
            return -EFAULT;
        }
        k_memset(nl, 0, sizeof(nl));
        nl[0] = (uint8_t)AF_NETLINK; /* the kernel, port id 0 */
        return netlink_addr_to_user(nl, sa_ptr, salen_ptr);
    }
    if (entry->sock->type == SOCK_DGRAM) {
        if (!entry->sock->connected) {
            return -ENOTCONN;
        }
    } else if (!tcp_is_connected(entry->sock)) {
        return -ENOTCONN;
    }
    if (!sa_ptr || !salen_ptr) {
        return -EFAULT;
    }
    return copy_sockaddr_to_user(sa_ptr, salen_ptr, entry->sock->remote_port,
                                 entry->sock->remote_ip, entry->sock->family);
}

/* The `how` of shutdown(2), Linux's values. */
#define SHUT_RD 0

#define SHUT_WR 1

#define SHUT_RDWR 2

/**
 * shutdown(how): SHUT_RD(0)/SHUT_WR(1)/SHUT_RDWR(2). SHUT_WR closes the
 * sending half: the FIN goes after the queued data, a later send is EPIPE,
 * and the peer's data and FIN still reach the reader, as `nc` needs when it
 * has sent its input and reads the answer. SHUT_RDWR closes the connection as
 * close() does, and the socket reads end of input. SHUT_RD alone is accepted
 * as a no-op. Returns 0, -EINVAL for another `how`, -ENOTCONN or -EOPNOTSUPP.
 *
 * Only a connected stream socket can be shut down: not a listener, which
 * would free its LISTEN TCB and stop accepting without a word, nor an
 * unconnected or a UDP socket. `tcp_close` detaches the socket from its TCB
 * both ways, so no later answer from the peer can reach its queue, and a
 * sharer already asleep in `recv` is woken here or never.
 */
static int32_t do_shutdown(int fd, int how)
{
    fd_entry_t *entry = fd_get(current_proc->fds, fd);

    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    if (entry->sock->type == SOCK_RAW) {
        return -EOPNOTSUPP;
    }
    if (how < SHUT_RD || how > SHUT_RDWR) {
        return -EINVAL;
    }
    if (!tcp_is_connected(entry->sock)) {
        return -ENOTCONN;
    }
    if (how == SHUT_WR) {
        tcp_shutdown_send(entry->sock);
        wq_wake_all(&entry->sock->waiters);
    } else if (how == SHUT_RDWR) {
        entry->sock->shut_down = 1;
        tcp_close(entry->sock); /* send FIN; idempotent vs the later close() */
        wq_wake_all(&entry->sock->waiters);
    }
    return 0;
}

/* Socket-option levels/names recognized by get/setsockopt. */
#define SOL_SOCKET 1

#define SO_REUSEADDR 2

#define SO_TYPE 3

#define SO_ERROR 4

#define SO_BROADCAST 6

#define SO_BINDTODEVICE 25

#define IFNAMSIZ 16

#define SO_KEEPALIVE 9

#define IPPROTO_TCP 6

#define TCP_NODELAY 1

#define TCP_KEEPIDLE 4

#define TCP_KEEPINTVL 5

#define TCP_KEEPCNT 6

#define TCP_USER_TIMEOUT 18

/** Whether `optname` is one of the TCP options a stream socket keeps:
 * TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT and TCP_USER_TIMEOUT. */
static int tcp_option_kept(int optname)
{
    return optname == TCP_KEEPIDLE || optname == TCP_KEEPINTVL || optname == TCP_KEEPCNT ||
           optname == TCP_USER_TIMEOUT;
}

/**
 * Keep the TCP option `optname` (`tcp_option_kept`) of the integer `val`
 * on the stream socket `sock`, with Linux's bounds: TCP_KEEPIDLE and
 * TCP_KEEPINTVL 1 to 32767 seconds, TCP_KEEPCNT 1 to 127, and
 * TCP_USER_TIMEOUT any count of milliseconds not negative, 0 for none.
 * Returns 0, or -EINVAL for a value shorter than an int or out of range.
 * The connection reads them at each tick (tcp.h).
 */
static int32_t tcp_option_set(struct socket *sock, int optname, int optlen, int val)
{
    int most = TCP_KEEPINTVL_MAX;

    if (optname == TCP_KEEPCNT) {
        most = TCP_KEEPCNT_MAX;
    } else if (optname == TCP_KEEPIDLE) {
        most = TCP_KEEPIDLE_MAX;
    }
    if (optlen < (int)sizeof(int)) {
        return -EINVAL;
    }
    if (optname == TCP_USER_TIMEOUT) {
        if (val < 0) {
            return -EINVAL;
        }
        sock->user_timeout = (uint32_t)val;
        return 0;
    }
    if (val < 1 || val > most) {
        return -EINVAL;
    }
    if (optname == TCP_KEEPIDLE) {
        sock->keepidle = val;
    } else if (optname == TCP_KEEPINTVL) {
        sock->keepintvl = val;
    } else {
        sock->keepcnt = val;
    }
    return 0;
}

/** The TCP option `optname` (`tcp_option_kept`) of the stream socket
 * `sock` as getsockopt reads it: a keepalive value as the socket set it,
 * else the /proc/sys default it takes, and TCP_USER_TIMEOUT as set. */
static int tcp_option_get(const struct socket *sock, int optname)
{
    if (optname == TCP_KEEPIDLE) {
        return sock->keepidle ? sock->keepidle : tcp_keepalive_time;
    }
    if (optname == TCP_KEEPINTVL) {
        return sock->keepintvl ? sock->keepintvl : tcp_keepalive_intvl;
    }
    if (optname == TCP_KEEPCNT) {
        return sock->keepcnt ? sock->keepcnt : tcp_keepalive_probes;
    }
    return (int)sock->user_timeout;
}

/**
 * Join or leave, as `join` says, the group of the struct ipv6_mreq of
 * `optlen` bytes at `optval_ptr` for the AF_INET6 socket `sock`
 * (IPV6_JOIN_GROUP, IPV6_LEAVE_GROUP). A join names its interface, or with
 * index 0 takes the one the group's route takes; a leave with index 0 takes
 * the first membership of the group on any interface. Returns 0, -EINVAL
 * for a value shorter than the structure or an address that is no group,
 * -EPROTO on a stream socket, -EFAULT, -ENODEV for an interface the socket
 * may not name (`socket_iface6`) or no route, which a socket of a network
 * namespace other than the first never has, -EADDRINUSE for a group the
 * socket has joined there, -EADDRNOTAVAIL for a leave of one it has not,
 * or -ENOBUFS past SOCK_GROUPS6 memberships or the interface's groups.
 */
static int32_t group6_change(struct socket *sock, int join, uintptr_t optval_ptr, int optlen)
{
    uint8_t mreq[IPV6_MREQ_LEN], via[16];
    struct netdev *nd;
    uint32_t flags;
    int ifindex, i, r = 0;

    if (optlen < IPV6_MREQ_LEN) {
        return -EINVAL;
    }
    if (sock->type == SOCK_STREAM) {
        return -EPROTO;
    }
    if (!optval_ptr || copy_from_user(mreq, (const void __user *)optval_ptr, sizeof(mreq)) != 0) {
        return -EFAULT;
    }
    if (!ip6_is_multicast(mreq)) {
        return -EINVAL;
    }
    ifindex = (int)SC_LE32(mreq + 16);
    flags = net_lock();
    for (i = 0; i < sock->ngroups6; i++) {
        if (ip6_equal(sock->groups6[i].group, mreq) &&
            (ifindex == 0 || sock->groups6[i].ifindex == ifindex)) {
            break;
        }
    }
    if (!join) {
        if (i == sock->ngroups6) {
            r = -EADDRNOTAVAIL;
        } else {
            nd = socket_iface6(sock, sock->groups6[i].ifindex);
            if (nd) {
                netdev_leave6(nd, mreq);
            }
            sock->groups6[i] = sock->groups6[--sock->ngroups6];
        }
    } else if (i < sock->ngroups6) {
        r = -EADDRINUSE;
    } else if (sock->ngroups6 == SOCK_GROUPS6) {
        r = -ENOBUFS;
    } else {
        if (ifindex) {
            nd = socket_iface6(sock, ifindex);
        } else {
            nd = socket_outside(sock) ? 0 : ip6_route_find(mreq, 0, via);
        }
        r = nd ? netdev_join6(nd, mreq) : -ENODEV;
        if (r == 0) {
            k_memcpy(sock->groups6[sock->ngroups6].group, mreq, 16);
            sock->groups6[sock->ngroups6].ifindex = nd->index;
            sock->ngroups6++;
        } else if (r == -ENOSPC) {
            r = -ENOBUFS;
        }
    }
    net_unlock(flags);
    return r;
}

/** Whether `optname` is an option of SOL_IPV6 that `ipv6_set` keeps. */
static int ipv6_option_known(int optname)
{
    switch (optname) {
    case IPV6_UNICAST_HOPS:
    case IPV6_MULTICAST_IF:
    case IPV6_MULTICAST_HOPS:
    case IPV6_MULTICAST_LOOP:
    case IPV6_JOIN_GROUP:
    case IPV6_LEAVE_GROUP:
    case IPV6_RECVPKTINFO:
    case IPV6_RECVHOPLIMIT:
    case IPV6_RECVTCLASS:
    case IPV6_TCLASS:
        return 1;
    default:
        return 0;
    }
}

/** The SOCK_RX_* bit of the IPV6_RECV* option `optname`, or 0. */
static uint8_t ipv6_rx_bit(int optname)
{
    switch (optname) {
    case IPV6_RECVPKTINFO:
        return SOCK_RX_PKTINFO;
    case IPV6_RECVHOPLIMIT:
        return SOCK_RX_HOPLIMIT;
    case IPV6_RECVTCLASS:
        return SOCK_RX_TCLASS;
    default:
        return 0;
    }
}

/**
 * setsockopt at SOL_IPV6 of an option `ipv6_option_known` names, on the
 * AF_INET6 socket `sock`, with `val` the int at `optval_ptr` when `optlen`
 * holds one. The hop limits and the class take -1 to 255, -1 for the
 * default (64 or the routers', 1, and 0); the loop 0 or 1; the multicast
 * interface an index the socket may name (`socket_iface6`), or 0, and when
 * the socket is bound to an interface, that one. Returns 0, -EINVAL for a
 * value shorter than an int or out of range, -ENODEV for an interface the
 * socket may not name, or the errors of `group6_change`.
 */
static int32_t ipv6_set(struct socket *sock, int optname, uintptr_t optval_ptr, int optlen, int val)
{
    int whole = optlen >= (int)sizeof(int);

    if (optname == IPV6_JOIN_GROUP || optname == IPV6_LEAVE_GROUP) {
        return group6_change(sock, optname == IPV6_JOIN_GROUP, optval_ptr, optlen);
    }
    if (!whole) {
        return -EINVAL;
    }
    switch (optname) {
    case IPV6_UNICAST_HOPS:
    case IPV6_MULTICAST_HOPS:
    case IPV6_TCLASS:
        if (val < -1 || val > 255) {
            return -EINVAL;
        }
        if (optname == IPV6_UNICAST_HOPS) {
            sock->hops6 = (int16_t)val;
        } else if (optname == IPV6_MULTICAST_HOPS) {
            sock->mhops6 = (int16_t)(val < 0 ? 1 : val);
        } else {
            sock->tclass6 = (int16_t)(val < 0 ? 0 : val);
        }
        return 0;
    case IPV6_MULTICAST_LOOP:
        if (val != 0 && val != 1) {
            return -EINVAL;
        }
        sock->mloop6 = (uint8_t)val;
        return 0;
    case IPV6_MULTICAST_IF:
        if (val != 0 && !socket_iface6(sock, val)) {
            return -ENODEV;
        }
        if (val != 0 && sock->bound_device && val != sock->bound_device) {
            return -EINVAL;
        }
        sock->mif6 = val;
        return 0;
    default:
        if (val) {
            sock->rxopt6 |= ipv6_rx_bit(optname);
        } else {
            sock->rxopt6 &= (uint8_t)~ipv6_rx_bit(optname);
        }
        return 0;
    }
}

/** The value getsockopt reports for the option `optname` of SOL_IPV6,
 * which `ipv6_option_known` names, on the AF_INET6 socket `sock`, into
 * `*val`. Returns 0, or -ENOPROTOOPT for a join or a leave, which are set
 * and not read. */
static int32_t ipv6_get(const struct socket *sock, int optname, int *val)
{
    switch (optname) {
    case IPV6_UNICAST_HOPS:
        *val = sock->hops6 >= 0 ? sock->hops6 : IP6_HOP_LIMIT_DEFAULT;
        return 0;
    case IPV6_MULTICAST_HOPS:
        *val = sock->mhops6;
        return 0;
    case IPV6_TCLASS:
        *val = sock->tclass6;
        return 0;
    case IPV6_MULTICAST_LOOP:
        *val = sock->mloop6;
        return 0;
    case IPV6_MULTICAST_IF:
        *val = sock->mif6;
        return 0;
    case IPV6_RECVPKTINFO:
    case IPV6_RECVHOPLIMIT:
    case IPV6_RECVTCLASS:
        *val = (sock->rxopt6 & ipv6_rx_bit(optname)) != 0;
        return 0;
    default:
        return -ENOPROTOOPT;
    }
}

/**
 * setsockopt(SOL_ICMPV6, ICMP6_FILTER) on `sock`: the first `optlen` bytes
 * of the 32 of a struct icmp6_filter at `optval_ptr` replace the filter's,
 * as `raw_set_filter6` says. Returns 0, -ENOPROTOOPT on a socket that is
 * not an AF_INET6 raw one, -EOPNOTSUPP on one of IPPROTO_RAW, -EINVAL for
 * a negative length, or -EFAULT.
 */
static int32_t icmp6_filter_set(struct socket *sock, uintptr_t optval_ptr, int optlen)
{
    uint8_t filter[RAW_FILTER6_LEN];
    uint32_t take;

    if (sock->type != SOCK_RAW || sock->family != AF_INET6) {
        return -ENOPROTOOPT;
    }
    if (sock->protocol != IPPROTO_ICMPV6) {
        return -EOPNOTSUPP;
    }
    if (optlen < 0) {
        return -EINVAL;
    }
    take = (uint32_t)optlen < sizeof(filter) ? (uint32_t)optlen : (uint32_t)sizeof(filter);
    if (take > 0 &&
        (!optval_ptr || copy_from_user(filter, (const void __user *)optval_ptr, take) != 0)) {
        return -EFAULT;
    }
    return raw_set_filter6(sock, filter, take) == 0 ? 0 : -ENOPROTOOPT;
}

/** getsockopt(SOL_ICMPV6, ICMP6_FILTER): the filter, cut to the caller's
 * length, which is written back. Returns 0, the errors of
 * `icmp6_filter_set` but -EFAULT for a null pointer, or -EINVAL for a
 * negative length. */
static int32_t icmp6_filter_get(struct socket *sock, uintptr_t optval_ptr, uintptr_t optlen_ptr)
{
    uint8_t filter[RAW_FILTER6_LEN];
    int32_t olen;

    if (sock->type != SOCK_RAW || sock->family != AF_INET6) {
        return -ENOPROTOOPT;
    }
    if (sock->protocol != IPPROTO_ICMPV6) {
        return -EOPNOTSUPP;
    }
    if (!optval_ptr || !optlen_ptr ||
        copy_from_user(&olen, (const void __user *)optlen_ptr, sizeof(olen)) != 0) {
        return -EFAULT;
    }
    if (olen < 0) {
        return -EINVAL;
    }
    if (olen > RAW_FILTER6_LEN) {
        olen = RAW_FILTER6_LEN;
    }
    if (raw_get_filter6(sock, filter) != 0) {
        return -ENOPROTOOPT;
    }
    if ((olen > 0 && copy_to_user((void __user *)optval_ptr, filter, (uint32_t)olen) != 0) ||
        copy_to_user((void __user *)optlen_ptr, &olen, sizeof(olen)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/**
 * setsockopt: keep SO_BROADCAST, SO_REUSEADDR and SO_KEEPALIVE, the hop
 * limit IP_TTL (1 to 255), SO_BINDTODEVICE, an interface by name or none
 * for an empty name, SO_RCVBUF, which Linux doubles and bounds by
 * SOCK_RCVBUF_MAX and SOCK_RCVBUF_MIN, IP_MTU_DISCOVER, an IP_PMTUDISC_*
 * value, and on a stream socket the keepalive's TCP_KEEPIDLE, TCP_KEEPINTVL
 * and TCP_KEEPCNT and TCP_USER_TIMEOUT (`tcp_option_set`), and accept every
 * other option with no effect, so that the setup of a forking server that
 * sets TCP_NODELAY succeeds; failing them would stop programs that set
 * options a small stack has no use for. Returns 0, or -EINVAL for a flag or
 * hop limit shorter than an int, a hop limit, IP_MTU_DISCOVER or TCP option
 * out of range or a SO_BINDTODEVICE value of no bytes, -ENODEV for an
 * unknown interface, -EFAULT, or -ENOPROTOOPT for the two options below.
 *
 * Two options fail with -ENOPROTOOPT. A DHCP client sets PACKET_AUXDATA on a
 * packet socket so that it can be told the kernel has not checked a
 * datagram's UDP checksum; there is no such offload here and no ancillary
 * data at all, and a success would promise data that never arrives. With the
 * failure the client verifies the checksum itself, which is the outcome that
 * is true. IP_HDRINCL would hand the caller the IP header to fill in, and
 * accepting and ignoring it would send a datagram whose header the caller
 * did not write.
 *
 * The hop limit is kept, because traceroute is this option counted up: a
 * stack that accepted it and sent every probe with the same limit would be
 * answered at the first hop every time and look like a machine one hop from
 * everywhere. SO_BINDTODEVICE reads only the name's 16 bytes from the start
 * of whatever the caller passed, since BusyBox passes a whole `struct ifreq`.
 * A flag whose value is too short to read is -EINVAL rather than a flag set
 * to zero, and an int-sized value at a null pointer is -EFAULT rather than
 * accepted.
 *
 * A netlink socket's SOL_NETLINK options are `netlink_setsockopt`'s.
 *
 * An AF_INET6 socket keeps the IPv6 options `ipv6_set` takes, which its
 * UDP and raw datagrams honour, and a raw ICMPv6 one its filter
 * (`icmp6_filter_set`); either level's option on a socket of another
 * family is -ENOPROTOOPT.
 */
static int32_t do_setsockopt(int fd, int level, int optname, uintptr_t optval_ptr, int optlen)
{
    fd_entry_t *entry = fd_get(current_proc->fds, fd);
    int val = 0;

    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    if (level == SOL_PACKET && optname == PACKET_AUXDATA) {
        return -ENOPROTOOPT;
    }
    (void)optname;
    if (level == SOL_SOCKET && optname == SO_BINDTODEVICE) {
        char name[IFNAMSIZ];
        struct netdev *nd;
        uint32_t take = (optlen < 0) ? 0u : (uint32_t)optlen;

        if (take == 0) {
            return -EINVAL;
        }
        if (take > IFNAMSIZ) {
            take = IFNAMSIZ;
        }
        k_memset(name, 0, sizeof(name));
        if (!optval_ptr || copy_from_user(name, (const void __user *)optval_ptr, take) != 0) {
            return -EFAULT;
        }
        name[IFNAMSIZ - 1] = '\0';
        if (name[0] == '\0') {
            entry->sock->bound_device = 0; /* an empty name unbinds */
            return 0;
        }
        nd = netdev_by_name(name);
        if (!nd) {
            return -ENODEV;
        }
        entry->sock->bound_device = nd->index;
        return 0;
    }

    if (optlen >= (int)sizeof(int)) {
        if (!optval_ptr ||
            copy_from_user(&val, (const void __user *)optval_ptr, sizeof(val)) != 0) {
            return -EFAULT;
        }
    }

    if (level == SOL_NETLINK && entry->sock->family == AF_NETLINK) {
        return netlink_setsockopt(entry->sock, optname, val, optlen);
    }
    if (level == SOL_IPV6 && ipv6_option_known(optname)) {
        if (entry->sock->family != AF_INET6) {
            return -ENOPROTOOPT;
        }
        return ipv6_set(entry->sock, optname, optval_ptr, optlen, val);
    }
    if (level == SOL_ICMPV6 && optname == ICMP6_FILTER) {
        return icmp6_filter_set(entry->sock, optval_ptr, optlen);
    }
    if (level == SOL_IPV6 && optname == IPV6_V6ONLY) {
        if (entry->sock->family != AF_INET6)
            return -ENOPROTOOPT;
        if (optlen < (int)sizeof(int) || entry->sock->local_port || entry->sock->tcb ||
            !net_addr_any(entry->sock->local_ip))
            return -EINVAL;
        entry->sock->v6only = val != 0;
        return 0;
    }
    if (level == SOL_IP && optname == IP_TTL) {
        if (optlen < (int)sizeof(int)) {
            return -EINVAL;
        }
        if (val < 1 || val > 255) {
            return -EINVAL;
        }
        entry->sock->ip_ttl = (uint8_t)val;
        return 0;
    }
    if (level == SOL_IP && optname == IP_HDRINCL) {
        return -ENOPROTOOPT;
    }
    if (level == SOL_IP && optname == IP_MTU_DISCOVER) {
        if (optlen < (int)sizeof(int)) {
            return -EINVAL;
        }
        if (val < IP_PMTUDISC_DONT || val > IP_PMTUDISC_OMIT) {
            return -EINVAL;
        }
        entry->sock->pmtudisc = (uint8_t)val;
        return 0;
    }
    if (level == SOL_SOCKET && optname == SO_RCVBUF) {
        uint32_t flags;

        if (optlen < (int)sizeof(int)) {
            return -EINVAL;
        }
        if (val < 0 || val > SOCK_RCVBUF_MAX) {
            val = SOCK_RCVBUF_MAX;
        }
        flags = net_lock();
        entry->sock->rcvbuf = val * 2 < SOCK_RCVBUF_MIN ? SOCK_RCVBUF_MIN : val * 2;
        net_unlock(flags);
        return 0;
    }

    if (level == IPPROTO_TCP && tcp_option_kept(optname) && entry->sock->type == SOCK_STREAM) {
        return tcp_option_set(entry->sock, optname, optlen, val);
    }

    if (level == SOL_SOCKET &&
        (optname == SO_BROADCAST || optname == SO_REUSEADDR || optname == SO_KEEPALIVE)) {
        if (optlen < (int)sizeof(int)) {
            return -EINVAL;
        }
        if (optname == SO_BROADCAST) {
            entry->sock->broadcast = val ? 1 : 0;
        } else if (optname == SO_REUSEADDR) {
            entry->sock->reuseaddr = val ? 1 : 0;
        } else {
            entry->sock->keepalive = val ? 1 : 0;
        }
    }
    return 0;
}

/** getsockopt: report the options programs read at setup: SO_ERROR (the
 * pending connection error, cleared by the read), SO_TYPE (the real socket
 * type), SO_REUSEADDR, SO_BROADCAST, SO_KEEPALIVE, SO_RCVBUF and
 * IP_MTU_DISCOVER as stored, a stream's keepalive values and
 * TCP_USER_TIMEOUT (`tcp_option_get`), SO_BINDTODEVICE as the interface
 * name, and IP_OPTIONS as empty, a result of width 0, since no socket keeps
 * IP options: OpenSSH's server ends a connection whose socket reports any. A
 * result is truncated to the caller's capacity, and the length written is
 * what was copied, less than an int for a caller that offered less.
 * SO_ERROR is read and cleared under the network lock, so the interrupt that
 * sets it cannot come between. The IPv6 options read back as `ipv6_get`
 * reports them, and a raw ICMPv6 socket's filter as `icmp6_filter_get`
 * does. Returns 0, -ENOPROTOOPT for IP_HDRINCL, for an IPv6 option of a
 * socket of another family or a join or leave, or -EFAULT. */
static int32_t do_getsockopt(int fd, int level, int optname, uintptr_t optval_ptr,
                             uintptr_t optlen_ptr)
{
    fd_entry_t *entry = fd_get(current_proc->fds, fd);
    int val = 0;
    uint32_t olen, width = (uint32_t)sizeof(val);

    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    if (level == SOL_IPV6 && optname == IPV6_V6ONLY) {
        if (entry->sock->family != AF_INET6)
            return -ENOPROTOOPT;
        val = entry->sock->v6only;
    } else if (level == SOL_IPV6 && ipv6_option_known(optname)) {
        int32_t r;

        if (entry->sock->family != AF_INET6) {
            return -ENOPROTOOPT;
        }
        r = ipv6_get(entry->sock, optname, &val);
        if (r != 0) {
            return r;
        }
    } else if (level == SOL_ICMPV6 && optname == ICMP6_FILTER) {
        return icmp6_filter_get(entry->sock, optval_ptr, optlen_ptr);
    } else if (level == SOL_SOCKET && optname == SO_TYPE) {
        val = entry->sock->type;
    } else if (level == SOL_SOCKET && optname == SO_ERROR) {
        uint32_t flags = net_lock();
        if (entry->sock->tcb) {
            val = (int)entry->sock->tcb->so_error;
            entry->sock->tcb->so_error = 0;
        } else {
            val = entry->sock->so_error;
            entry->sock->so_error = 0;
        }
        net_unlock(flags);
    } else if (level == SOL_SOCKET && optname == SO_BINDTODEVICE) {
        char name[IFNAMSIZ];
        uint32_t give;
        struct netdev *nd = netdev_by_index(entry->sock->bound_device);

        k_memset(name, 0, sizeof(name));
        if (nd) {
            k_strncpy(name, nd->name, sizeof(name) - 1);
        }
        if (!optval_ptr || !optlen_ptr) {
            return -EFAULT;
        }
        if (copy_from_user(&olen, (const void __user *)optlen_ptr, sizeof(olen)) != 0) {
            return -EFAULT;
        }
        give = (uint32_t)k_strlen(name) + 1u;
        if (give > olen) {
            give = olen;
        }
        if (give > 0 && copy_to_user((void __user *)optval_ptr, name, give) != 0) {
            return -EFAULT;
        }
        if (copy_to_user((void __user *)optlen_ptr, &give, sizeof(give)) != 0) {
            return -EFAULT;
        }
        return 0;
    } else if (level == SOL_IP && optname == IP_TTL) {
        val = (int)entry->sock->ip_ttl;
    } else if (level == SOL_IP && optname == IP_HDRINCL) {
        return -ENOPROTOOPT;
    } else if (level == SOL_IP && optname == IP_OPTIONS) {
        width = 0;
    } else if (level == SOL_IP && optname == IP_MTU_DISCOVER) {
        val = entry->sock->pmtudisc;
    } else if (level == SOL_SOCKET && optname == SO_RCVBUF) {
        val = entry->sock->rcvbuf;
    } else if (level == SOL_SOCKET && optname == SO_BROADCAST) {
        val = entry->sock->broadcast;
    } else if (level == SOL_SOCKET && optname == SO_REUSEADDR) {
        val = entry->sock->reuseaddr;
    } else if (level == SOL_SOCKET && optname == SO_KEEPALIVE) {
        val = entry->sock->keepalive;
    } else if (level == IPPROTO_TCP && tcp_option_kept(optname) &&
               entry->sock->type == SOCK_STREAM) {
        val = tcp_option_get(entry->sock, optname);
    } else {
        val = 0; /* TCP_NODELAY and the rest: off or none */
    }
    if (!optval_ptr || !optlen_ptr) {
        return -EFAULT;
    }
    if (copy_from_user(&olen, (const void __user *)optlen_ptr, sizeof(olen)) != 0) {
        return -EFAULT;
    }
    if (olen > width) {
        olen = width;
    }
    if (olen > 0 && copy_to_user((void __user *)optval_ptr, &val, olen) != 0) {
        return -EFAULT;
    }
    if (copy_to_user((void __user *)optlen_ptr, &olen, sizeof(olen)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/** Whether a receive on `sock` answers with control messages: an AF_INET6
 * UDP or raw socket that asked for one (IPV6_RECV*). */
static int inet6_rx_control(const struct socket *sock)
{
    return sock->family == AF_INET6 && sock->type != SOCK_STREAM && sock->rxopt6 != 0;
}

/**
 * Write one control message of `level` and `type` with the `len` bytes at
 * `data` at offset `*used` of the caller's control buffer of `controllen`
 * bytes at `control`, and move `*used` past it and its padding, as far as
 * the buffer goes, as Linux's `put_cmsg` counts it. A message that does not
 * fit whole is not written and sets MSG_CTRUNC in `*mflags`, after which no
 * other is, so that what was written is a prefix of what was asked for.
 * Returns 0, or -EFAULT.
 */
static int32_t put_cmsg(uintptr_t control, uint32_t controllen, uint32_t *used, int level, int type,
                        const void *data, uint32_t len, unsigned int *mflags)
{
    struct kernel_cmsghdr hdr;
    uint32_t need = (uint32_t)CMSG_HDR + len;

    if ((*mflags & MSG_CTRUNC) || !control || controllen < *used || controllen - *used < need) {
        *mflags |= MSG_CTRUNC;
        return 0;
    }
    k_memset(&hdr, 0, sizeof(hdr));
    hdr.cmsg_len = need;
    hdr.cmsg_level = level;
    hdr.cmsg_type = type;
    if (copy_to_user((void __user *)(control + *used), &hdr, sizeof(hdr)) != 0 ||
        copy_to_user((void __user *)(control + *used + CMSG_HDR), data, len) != 0) {
        return -EFAULT;
    }
    *used += (uint32_t)CMSG_ALIGN(need);
    if (*used > controllen) {
        *used = controllen;
    }
    return 0;
}

/**
 * Write the control messages the AF_INET6 socket `sock` asked for
 * (`rxopt6`) about the datagram a receive took, which `rx_last` describes,
 * in the order Linux writes them: IPV6_PKTINFO, its destination and the
 * interface it came in on, an IPv4 one's destination mapped; IPV6_HOPLIMIT
 * and IPV6_TCLASS, each an int, when the datagram's are known. Sets
 * `*used` to the bytes of the buffer used. Returns 0, or -EFAULT.
 */
static int32_t put_rx_control(const struct socket *sock, uintptr_t control, uint32_t controllen,
                              uint32_t *used, unsigned int *mflags)
{
    const struct net_rxinfo *info = &sock->rx_last;
    int32_t r = 0;
    int v;

    *used = 0;
    if (sock->rxopt6 & SOCK_RX_PKTINFO) {
        uint8_t pi[IN6_PKTINFO_LEN];

        net_addr_to6(info->dst, pi);
        SC_WLE32(pi + 16, info->ifindex);
        r = put_cmsg(control, controllen, used, SOL_IPV6, IPV6_PKTINFO, pi, sizeof(pi), mflags);
    }
    if (r == 0 && (sock->rxopt6 & SOCK_RX_HOPLIMIT) && info->hlim >= 0) {
        v = info->hlim;
        r = put_cmsg(control, controllen, used, SOL_IPV6, IPV6_HOPLIMIT, &v, sizeof(v), mflags);
    }
    if (r == 0 && (sock->rxopt6 & SOCK_RX_TCLASS) && info->tclass >= 0) {
        v = info->tclass;
        r = put_cmsg(control, controllen, used, SOL_IPV6, IPV6_TCLASS, &v, sizeof(v), mflags);
    }
    return r;
}

/**
 * Read the control messages of a sendmsg on the AF_INET6 socket `sock`, the
 * `controllen` bytes at `control`, into `opt`: IPV6_PKTINFO, a source
 * address, :: for none, and an interface, 0 for none; IPV6_HOPLIMIT and
 * IPV6_TCLASS, each an int from -1 to 255, -1 for the socket's. Messages
 * of another level are passed over, as they were before any was read.
 * Returns 0, -EINVAL for a header whose length is short or runs past the
 * buffer, a message of SOL_IPV6 of another type or the wrong size or a
 * value out of range, -ENODEV for an interface the socket may not name
 * (`socket_iface6`), or -EFAULT.
 */
static int32_t get_tx_control(const struct socket *sock, uintptr_t control, uint32_t controllen,
                              struct net_txopt *opt)
{
    uint32_t off = 0;

    k_memset(opt, 0, sizeof(*opt));
    opt->src = net_addr4(0);
    opt->hlim = -1;
    opt->tclass = -1;
    while (control && controllen - off >= (uint32_t)CMSG_HDR) {
        struct kernel_cmsghdr hdr;
        uint32_t dlen;
        uint8_t data[IN6_PKTINFO_LEN];
        int v;

        if (copy_from_user(&hdr, (const void __user *)(control + off), sizeof(hdr)) != 0) {
            return -EFAULT;
        }
        if (hdr.cmsg_len < CMSG_HDR || hdr.cmsg_len > controllen - off) {
            return -EINVAL;
        }
        dlen = (uint32_t)(hdr.cmsg_len - CMSG_HDR);
        if (hdr.cmsg_level == SOL_IPV6) {
            uint32_t want = hdr.cmsg_type == IPV6_PKTINFO ? IN6_PKTINFO_LEN : (uint32_t)sizeof(int);

            if ((hdr.cmsg_type != IPV6_PKTINFO && hdr.cmsg_type != IPV6_HOPLIMIT &&
                 hdr.cmsg_type != IPV6_TCLASS) ||
                dlen < want) {
                return -EINVAL;
            }
            if (copy_from_user(data, (const void __user *)(control + off + CMSG_HDR), want) != 0) {
                return -EFAULT;
            }
            if (hdr.cmsg_type == IPV6_PKTINFO) {
                opt->ifindex = (int)SC_LE32(data + 16);
                if (opt->ifindex != 0 && !socket_iface6(sock, opt->ifindex)) {
                    return -ENODEV;
                }
                opt->src = net_addr6(data, 0);
            } else {
                k_memcpy(&v, data, sizeof(v));
                if (v < -1 || v > 255) {
                    return -EINVAL;
                }
                if (hdr.cmsg_type == IPV6_HOPLIMIT) {
                    opt->hlim = (int16_t)v;
                } else {
                    opt->tclass = (int16_t)v;
                }
            }
        }
        if ((uint32_t)CMSG_ALIGN(hdr.cmsg_len) >= controllen - off) {
            break;
        }
        off += (uint32_t)CMSG_ALIGN(hdr.cmsg_len);
    }
    return 0;
}

/**
 * Receive one datagram into a scatter list (recvmsg, socketcall 17).
 *
 * The DHCP client is what needs this: it reads its packet socket through
 * `recvmsg`, not `read` so that it can look for `PACKET_AUXDATA` in the
 * control data, and finding none it verifies the UDP checksum itself. So the
 * scatter half is the whole of the work here. The control buffer is answered
 * with a length of zero, the true answer, but on an AF_INET6 UDP or raw
 * socket that asked for IPV6_PKTINFO, IPV6_HOPLIMIT or IPV6_TCLASS
 * (`put_rx_control`); a message that does not fit sets MSG_CTRUNC, and the
 * buffer is checked with the others before the datagram is taken.
 *
 * A datagram longer than the buffers is cut, and `msg_flags` says so with
 * MSG_TRUNC; with MSG_TRUNC in `flags` the call answers the datagram's whole
 * length, which netlink's programs, iproute2's ip among them, use with
 * MSG_PEEK and no buffer before they read a message.
 *
 * The sender is reported through `msg_name`: a `sockaddr_in` for an AF_INET
 * socket, and for a packet socket the `sockaddr_ll` the frame came from, with
 * the sender's hardware address and who the frame was addressed to. A struct
 * sockaddr_in is 16 bytes (family, port, address, and eight bytes of padding
 * a caller may compare against sizeof) and a sockaddr_ll is 20. Either is
 * written whole where it fits and truncated where the caller's buffer is
 * smaller, and `msg_namelen` is the true length either way, which is what
 * Linux does and how a caller learns its buffer was too small; it is zero
 * for a packet socket that has no sender to report.
 *
 * Every destination is checked before the datagram is taken, because taking
 * it is what destroys it: a fault discovered half way through the copy would
 * report an error for a message the machine no longer has. This is the pass
 * `sys_readv` makes for the same reason. Only what will be written of the
 * address is checked, twenty bytes for a packet socket and sixteen for an
 * AF_INET one, however much the caller declared, since checking the whole
 * declared capacity would fail a call whose address fits.
 *
 * Returns the bytes taken, or with MSG_TRUNC the datagram's whole length, or
 * -errno: -EOPNOTSUPP on a socket that is neither a datagram nor a raw
 * socket, or on the raw descriptor that takes only ioctls, -EINVAL for
 * more than IOV_MAX_LOCAL buffers, -EFAULT, or the error of the receive.
 */
static int32_t do_recvmsg(int fd, uintptr_t msg_ptr, int flags)
{
    struct kernel_msghdr hdr;
    fd_entry_t *entry;
    uintptr_t iov_user, name_ptr;
    uint32_t namelen;
    uint32_t addr_len, addr_give;
    uint32_t iovlen;
    net_addr_t src_ip = net_addr4(0);
    uint16_t src_port = 0;
    uint32_t i;
    int32_t taken;
    int32_t whole = 0;

    if (copy_from_user(&hdr, (const void __user *)msg_ptr, sizeof(hdr)) != 0) {
        return -EFAULT;
    }
    entry = current_proc ? fd_get(current_proc->fds, fd) : (fd_entry_t *)0;
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    if ((entry->sock->type != SOCK_DGRAM && entry->sock->type != SOCK_RAW) ||
        sock_is_query(entry->sock)) {
        return -EOPNOTSUPP;
    }

    name_ptr = hdr.msg_name;
    namelen = (uint32_t)hdr.msg_namelen;
    iov_user = hdr.msg_iov;
    if (hdr.msg_iovlen > IOV_MAX_LOCAL) {
        return -EINVAL;
    }
    iovlen = (uint32_t)hdr.msg_iovlen;

    if (!user_buffer_ok(msg_ptr, sizeof(hdr), 1)) {
        return -EFAULT;
    }
    if (inet6_rx_control(entry->sock) && hdr.msg_control && hdr.msg_controllen > 0 &&
        (hdr.msg_controllen > 0x7FFFFFFFu ||
         !user_buffer_ok(hdr.msg_control, (uint32_t)hdr.msg_controllen, 1))) {
        return -EFAULT;
    }
    addr_len = sockaddr_len_of(entry->sock);
    addr_give = namelen < addr_len ? namelen : addr_len;
    if (name_ptr && addr_give > 0 && !user_buffer_ok(name_ptr, addr_give, 1)) {
        return -EFAULT;
    }
    for (i = 0; i < iovlen; i++) {
        struct kernel_iovec iov;

        if (copy_from_user(&iov, (const void __user *)(iov_user + i * sizeof(struct kernel_iovec)),
                           sizeof(iov)) != 0) {
            return -EFAULT;
        }
        if (iov.iov_len == 0) {
            continue;
        }
        if (iov.iov_len > 0x7FFFFFFFu || !user_buffer_ok(iov.iov_base, iov.iov_len, 1)) {
            return -EFAULT;
        }
    }

    if ((flags & MSG_DONTWAIT) && (taken = socket_dgram_nowait(entry->sock)) < 0) {
        return taken;
    }

    taken = readv_dgram(entry, iov_user, (int)iovlen, &src_ip, &src_port, (flags & MSG_PEEK) != 0,
                        &whole);
    if (taken < 0) {
        return taken;
    }

    if (name_ptr && addr_len > 0) {
        uint8_t sa[SOCKADDR_IN6_LEN];

        k_memset(sa, 0, sizeof(sa));
        if (entry->sock->family == AF_NETLINK) {
            sa[0] = (uint8_t)AF_NETLINK; /* the kernel, port id 0 */
        } else if (entry->sock->family == AF_PACKET) {
            if (packet_last_addr(entry->sock, sa) == 0) {
                addr_len = 0;
                addr_give = 0;
            }
        } else {
            inet_endpoint_out(sa, entry->sock->family, src_ip, src_port);
        }
        if (addr_give > 0 && copy_to_user((void __user *)name_ptr, sa, addr_give) != 0) {
            return -EFAULT;
        }
    }
    hdr.msg_namelen = name_ptr ? (int)addr_len : 0;
    hdr.msg_flags = whole > taken ? (unsigned int)MSG_TRUNC : 0u;
    if (inet6_rx_control(entry->sock)) {
        uint32_t used;

        if (put_rx_control(entry->sock, hdr.msg_control, (uint32_t)hdr.msg_controllen, &used,
                           &hdr.msg_flags) != 0) {
            return -EFAULT;
        }
        hdr.msg_controllen = used;
    } else {
        hdr.msg_controllen = 0;
    }
    if (copy_to_user((void __user *)msg_ptr, &hdr, sizeof(hdr)) != 0) {
        return -EFAULT;
    }
    return (flags & MSG_TRUNC) ? whole : taken;
}

/**
 * sendmsg(2) on an AF_INET, packet or netlink socket: the caller's buffers in
 * order, as one send. A stream sends them one after another and stops at the
 * first that goes short; a UDP or netlink socket gathers at most eight into
 * one datagram, of at most 65535 bytes or `NETLINK_SEND_MAX`; a raw or packet
 * socket takes one buffer. On an AF_INET6 UDP or raw socket the control
 * messages IPV6_PKTINFO, IPV6_HOPLIMIT and IPV6_TCLASS apply to this send
 * alone (`get_tx_control`); elsewhere control messages carry nothing and
 * are ignored. Returns the bytes sent, a stream's count before a failure,
 * or -errno: -EMSGSIZE for too many buffers or bytes, -EOPNOTSUPP for more
 * than one buffer on a raw or packet socket, or an error of
 * `get_tx_control`.
 */
static int32_t do_sendmsg(int fd, uintptr_t msg_ptr, int flags)
{
    struct kernel_msghdr hdr;
    struct net_txopt txopt;
    const struct net_txopt *opt = 0;
    struct kernel_iovec iov[8];
    uint8_t *gathered;
    fd_entry_t *entry;
    uintptr_t iov_user, name_ptr;
    uint32_t iovlen, namelen, i, total = 0;
    int32_t rc;

    if (copy_from_user(&hdr, (const void __user *)msg_ptr, sizeof(hdr)) != 0) {
        return -EFAULT;
    }
    entry = current_proc ? fd_get(current_proc->fds, fd) : (fd_entry_t *)0;
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    name_ptr = hdr.msg_name;
    namelen = (uint32_t)hdr.msg_namelen;
    iov_user = hdr.msg_iov;
    if (hdr.msg_iovlen > IOV_MAX_LOCAL) {
        return -EMSGSIZE;
    }
    iovlen = (uint32_t)hdr.msg_iovlen;
    if (cut_off(entry->sock)) {
        return -ENETUNREACH;
    }
    if (entry->sock->family == AF_INET6 && entry->sock->type != SOCK_STREAM && hdr.msg_control &&
        hdr.msg_controllen > 0) {
        if (hdr.msg_controllen > 0x7FFFFFFFu) {
            return -EINVAL;
        }
        rc = get_tx_control(entry->sock, hdr.msg_control, (uint32_t)hdr.msg_controllen, &txopt);
        if (rc != 0) {
            return rc;
        }
        opt = &txopt;
    }
    if (entry->sock->type == SOCK_STREAM) {
        for (i = 0; i < iovlen; i++) {
            struct kernel_iovec one;

            if (copy_from_user(&one, (const void __user *)(iov_user + i * sizeof(one)),
                               sizeof(one)) != 0) {
                return total ? (int32_t)total : -EFAULT;
            }
            if (one.iov_len == 0) {
                continue;
            }
            rc = do_sendto(fd, one.iov_base, (int)USER_RW_LEN(one.iov_len), flags, 0, 0);
            if (rc < 0) {
                return total ? (int32_t)total : rc;
            }
            total += (uint32_t)rc;
            if ((uint32_t)rc < one.iov_len) {
                break;
            }
        }
        return (int32_t)total;
    }
    if (iovlen > sizeof(iov) / sizeof(iov[0])) {
        return -EMSGSIZE;
    }
    if (copy_from_user(iov, (const void __user *)iov_user, iovlen * sizeof(iov[0])) != 0) {
        return -EFAULT;
    }
    if (iovlen == 1 || entry->sock->type != SOCK_DGRAM ||
        (entry->sock->family != AF_INET && entry->sock->family != AF_INET6 &&
         entry->sock->family != AF_NETLINK)) {
        if (iovlen > 1) {
            return -EOPNOTSUPP;
        }
        return do_sendto_opt(fd, iovlen ? iov[0].iov_base : 0u,
                             iovlen ? (int)USER_RW_LEN(iov[0].iov_len) : 0, flags, name_ptr,
                             (int)namelen, opt);
    }
    for (i = 0; i < iovlen; i++) {
        uint32_t most = entry->sock->family == AF_NETLINK ? NETLINK_SEND_MAX : 0xFFFFu;

        if (iov[i].iov_len > most - total) {
            return -EMSGSIZE;
        }
        total += (uint32_t)iov[i].iov_len;
    }
    gathered = kmalloc(total ? total : 1u);
    if (!gathered) {
        return -ENOMEM;
    }
    total = 0;
    for (i = 0; i < iovlen; i++) {
        if (copy_from_user(gathered + total, (const void __user *)iov[i].iov_base,
                           iov[i].iov_len) != 0) {
            kfree(gathered);
            return -EFAULT;
        }
        total += (uint32_t)iov[i].iov_len;
    }
    if (entry->sock->family == AF_NETLINK) {
        rc = netlink_send_held(entry->sock, gathered, total);
    } else {
        rc = udp_sendto(entry, gathered, (int)total, name_ptr, (int)namelen, opt);
    }
    kfree(gathered);
    return rc;
}

/** The calls on an AF_UNIX socket, which ipc/unix.c answers; -EOPNOTSUPP
 * for a sub-call it does not have. */
static int32_t unix_call(int call, fd_entry_t *e, const unsigned long *args)
{
    switch (call) {
    case SC_BIND:
        return unix_bind(e, args[1], (uint32_t)args[2]);
    case SC_CONNECT:
        return unix_connect(e, args[1], (uint32_t)args[2]);
    case SC_LISTEN:
        return unix_listen(e, (int)args[1]);
    case SC_ACCEPT:
        return unix_accept(e, args[1], args[2], 0);
    case SC_ACCEPT4:
        return unix_accept(e, args[1], args[2], (int)args[3]);
    case SC_GETSOCKNAME:
        return unix_getname(e, args[1], args[2], 0);
    case SC_GETPEERNAME:
        return unix_getname(e, args[1], args[2], 1);
    case SC_SEND:
        return unix_sendto(e, args[1], USER_RW_LEN(args[2]), (int)args[3], 0, 0);
    case SC_RECV:
        return unix_recvfrom(e, args[1], USER_RW_LEN(args[2]), (int)args[3], 0, 0);
    case SC_SENDTO:
        return unix_sendto(e, args[1], USER_RW_LEN(args[2]), (int)args[3], args[4],
                           (uint32_t)args[5]);
    case SC_RECVFROM:
        return unix_recvfrom(e, args[1], USER_RW_LEN(args[2]), (int)args[3], args[4], args[5]);
    case SC_SHUTDOWN:
        return unix_shutdown(e, (int)args[1]);
    case SC_SETSOCKOPT:
        return unix_setsockopt(e, (int)args[1], (int)args[2], args[3], (uint32_t)args[4]);
    case SC_GETSOCKOPT:
        return unix_getsockopt(e, (int)args[1], (int)args[2], args[3], args[4]);
    case SC_SENDMSG:
        return unix_sendmsg(e, args[1], (int)args[2]);
    case SC_RECVMSG:
        return unix_recvmsg(e, args[1], (int)args[2]);
    default:
        return -EOPNOTSUPP;
    }
}

/** socket(2) and socketpair(2) of AF_UNIX: the type with its SOCK_CLOEXEC and
 * SOCK_NONBLOCK bits taken off, and any other bit -EINVAL. */
static int32_t unix_make(int call, const unsigned long *args)
{
    uint32_t flags = (uint32_t)args[1] & ~(uint32_t)SOCK_TYPE_MASK;
    int type = (int)(args[1] & SOCK_TYPE_MASK);

    if (flags & ~(uint32_t)(SOCK_CLOEXEC | SOCK_NONBLOCK)) {
        return -EINVAL;
    }
    if (call == SC_SOCKETPAIR) {
        return unix_socketpair(type, (int)args[2], (int)flags, args[3]);
    }
    return unix_socket(type, (int)args[2], (int)flags);
}

static int32_t socket_dispatch(int call, unsigned long *args);

/**
 * sendmmsg and recvmmsg: sendmsg or recvmsg on the socket `args[0]` for each
 * of the `args[2]` headers at `args[1]`, with the flags `args[3]`, storing each
 * message's count in its header, as Linux's do; glibc's resolver sends its
 * two queries with sendmmsg. Returns the messages done, or the first error
 * when none was. After the first message recvmmsg waits for no more, with or
 * without MSG_WAITFORONE, so a call returns what has come; its timeout is not
 * read.
 */
static int32_t mmsg(int call, const unsigned long *args)
{
    unsigned long sub[3];
    uint32_t vlen = (uint32_t)args[2] > MMSG_MAX ? MMSG_MAX : (uint32_t)args[2];
    uint32_t i;
    int32_t rc = 0;

    sub[0] = args[0];
    sub[2] = args[3] & ~MSG_WAITFORONE;
    for (i = 0; i < vlen; i++) {
        sub[1] = args[1] + i * sizeof(struct kernel_mmsghdr);
        rc = socket_dispatch(call == SC_SENDMMSG ? SC_SENDMSG : SC_RECVMSG, sub);
        if (rc < 0) {
            break;
        }
        if (copy_to_user((void __user *)(sub[1] + MMSG_LEN_OFF), &rc, sizeof(rc)) != 0) {
            rc = -EFAULT;
            break;
        }
        if (call == SC_RECVMMSG) {
            sub[2] |= MSG_DONTWAIT;
        }
    }
    return i > 0 ? (int32_t)i : rc;
}

/**
 * Multiplexed socket syscall (socketcall).
 *
 * Its arguments are the SC_* sub-call number and a user pointer to its argument
 * array, of longs. Copies as many as the sub-call takes in one fault-checked
 * read, then dispatches through `socket_dispatch`. Returns its result,
 * -EINVAL for an unknown sub-call, or -EFAULT.
 */
long sys_socketcall(isr_params_t *params)
{
    int call = (int)SC_ARG(params, 0);
    uintptr_t argp = SC_ARG(params, 1);
    unsigned long args[6];
    int nargs;

    switch (call) {
    case SC_PING:
        nargs = 1;
        break;
    case SC_PING_DATA:
        nargs = 3;
        break;
    case SC_PING6:
        nargs = 4;
        break;
    case SC_LISTEN:
    case SC_SHUTDOWN:
        nargs = 2;
        break;
    case SC_SOCKET:
    case SC_BIND:
    case SC_CONNECT:
    case SC_ACCEPT:
    case SC_GETSOCKNAME:
    case SC_GETPEERNAME:
        nargs = 3;
        break;
    case SC_RECVMSG:
    case SC_SENDMSG:
        nargs = 3;
        break;
    case SC_SOCKETPAIR:
    case SC_SEND:
    case SC_RECV:
    case SC_ACCEPT4:
    case SC_SENDMMSG:
        nargs = 4;
        break;
    case SC_SETSOCKOPT:
    case SC_GETSOCKOPT:
    case SC_RECVMMSG:
        nargs = 5;
        break;
    case SC_SENDTO:
    case SC_RECVFROM:
        nargs = 6;
        break;
    default:
        return -EINVAL;
    }

    if (copy_from_user(args, (const void __user *)argp, (uint32_t)nargs * sizeof(args[0])) != 0) {
        return -EFAULT;
    }
    return socket_dispatch(call, args);
}

/**
 * Run socket sub-call `call` with its arguments `args`, for socketcall and
 * for the direct socket calls. A call on an AF_UNIX socket, or one that
 * makes one, goes to ipc/unix.c; the rest to the calls above. Returns the
 * sub-call's result, -EOPNOTSUPP for a socketpair of another family, or
 * -EINVAL for an unknown sub-call.
 */
static int32_t socket_dispatch(int call, unsigned long *args)
{
    fd_entry_t *e;

    if ((call == SC_SOCKET || call == SC_SOCKETPAIR) && (uint32_t)args[0] == AF_UNIX) {
        return unix_make(call, args);
    }
    if (call == SC_SOCKETPAIR) {
        return -EOPNOTSUPP;
    }
    if (call == SC_SENDMMSG || call == SC_RECVMMSG) {
        return mmsg(call, args);
    }
    e = (call != SC_SOCKET && call != SC_PING && call != SC_PING_DATA && call != SC_PING6 &&
         current_proc)
            ? fd_get(current_proc->fds, (int)args[0])
            : (fd_entry_t *)0;
    if (e && e->type == FD_TYPE_UNIX) {
        return unix_call(call, e, args);
    }

    switch (call) {
    case SC_SOCKET:
        return socket_with_flags((int)args[0], (int)args[1], (int)args[2]);
    case SC_BIND:
        return do_bind((int)args[0], args[1], (int)args[2]);
    case SC_SENDTO:
        return do_sendto((int)args[0], args[1], (int)USER_RW_LEN(args[2]), (int)args[3], args[4],
                         (int)args[5]);
    case SC_RECVFROM:
        return do_recvfrom((int)args[0], args[1], (int)USER_RW_LEN(args[2]), (int)args[3], args[4],
                           args[5]);
    case SC_PING:
        return do_ping((uint32_t)args[0]);
    case SC_PING_DATA: {
        uint16_t got = 0;
        uint8_t got_ttl = 0;
        int rc;

        if (args[2] < 1 || args[2] > 255) {
            return -EINVAL;
        }
        if (args[1] != (uint32_t)args[1]) {
            return -EMSGSIZE;
        }
        rc = icmp_ping_data((uint32_t)args[0], (uint32_t)args[1], (uint8_t)args[2], &got, &got_ttl);
        return rc < 0 ? rc : (int32_t)((uint32_t)got | (uint32_t)got_ttl << 16);
    }
    case SC_PING6:
        return do_ping6(args);
    case SC_CONNECT:
        return do_connect((int)args[0], args[1], (int)args[2]);
    case SC_LISTEN:
        return do_listen((int)args[0], (int)args[1]);
    case SC_ACCEPT:
        return do_accept((int)args[0], args[1], args[2], 0);
    case SC_ACCEPT4:
        return do_accept((int)args[0], args[1], args[2], (uint32_t)args[3]);
    case SC_GETSOCKNAME:
        return do_getsockname((int)args[0], args[1], args[2]);
    case SC_GETPEERNAME:
        return do_getpeername((int)args[0], args[1], args[2]);
    case SC_SHUTDOWN:
        return do_shutdown((int)args[0], (int)args[1]);
    case SC_SETSOCKOPT:
        return do_setsockopt((int)args[0], (int)args[1], (int)args[2], args[3], (int)args[4]);
    case SC_GETSOCKOPT:
        return do_getsockopt((int)args[0], (int)args[1], (int)args[2], args[3], args[4]);
    case SC_SEND:
        return do_send((int)args[0], args[1], (int)USER_RW_LEN(args[2]), (int)args[3]);
    case SC_RECV:
        return do_recv((int)args[0], args[1], (int)USER_RW_LEN(args[2]), (int)args[3]);
    case SC_RECVMSG:
        return do_recvmsg((int)args[0], args[1], (int)args[2]);
    case SC_SENDMSG:
        return do_sendmsg((int)args[0], args[1], (int)args[2]);
    default:
        return -EINVAL;
    }
}

/** The body of the direct socket calls: the same sub-calls with their
 * arguments in registers. They are x86-64's socket calls; on i386 they are
 * numbers 359 to 373, and recvmmsg and sendmmsg, which a C library built for
 * a Linux of 4.3 or later makes instead of socketcall. */
static int32_t direct(int call, isr_params_t *params)
{
    unsigned long args[6];

    args[0] = SC_ARG(params, 0);
    args[1] = SC_ARG(params, 1);
    args[2] = SC_ARG(params, 2);
    args[3] = SC_ARG(params, 3);
    args[4] = SC_ARG(params, 4);
    args[5] = SC_ARG(params, 5);
    return socket_dispatch(call, args);
}

/** socket(2), the sub-call SC_SOCKET. */
long sys_socket(isr_params_t *params)
{
    return direct(SC_SOCKET, params);
}

/** socketpair(2), the sub-call SC_SOCKETPAIR. */
long sys_socketpair(isr_params_t *params)
{
    return direct(SC_SOCKETPAIR, params);
}

/** bind(2), the sub-call SC_BIND. */
long sys_bind(isr_params_t *params)
{
    return direct(SC_BIND, params);
}

/** connect(2), the sub-call SC_CONNECT. */
long sys_connect(isr_params_t *params)
{
    return direct(SC_CONNECT, params);
}

/** listen(2), the sub-call SC_LISTEN. */
long sys_listen(isr_params_t *params)
{
    return direct(SC_LISTEN, params);
}

/** accept4(2), the sub-call SC_ACCEPT4. */
long sys_accept4(isr_params_t *params)
{
    return direct(SC_ACCEPT4, params);
}

/** getsockopt(2), the sub-call SC_GETSOCKOPT. */
long sys_getsockopt(isr_params_t *params)
{
    return direct(SC_GETSOCKOPT, params);
}

/** setsockopt(2), the sub-call SC_SETSOCKOPT. */
long sys_setsockopt(isr_params_t *params)
{
    return direct(SC_SETSOCKOPT, params);
}

/** getsockname(2), the sub-call SC_GETSOCKNAME. */
long sys_getsockname(isr_params_t *params)
{
    return direct(SC_GETSOCKNAME, params);
}

/** getpeername(2), the sub-call SC_GETPEERNAME. */
long sys_getpeername(isr_params_t *params)
{
    return direct(SC_GETPEERNAME, params);
}

/** sendto(2), the sub-call SC_SENDTO. */
long sys_sendto(isr_params_t *params)
{
    return direct(SC_SENDTO, params);
}

/** sendmsg(2), the sub-call SC_SENDMSG. */
long sys_sendmsg(isr_params_t *params)
{
    return direct(SC_SENDMSG, params);
}

/** recvfrom(2), the sub-call SC_RECVFROM. */
long sys_recvfrom(isr_params_t *params)
{
    return direct(SC_RECVFROM, params);
}

/** recvmsg(2), the sub-call SC_RECVMSG. */
long sys_recvmsg(isr_params_t *params)
{
    return direct(SC_RECVMSG, params);
}

/** shutdown(2), the sub-call SC_SHUTDOWN. */
long sys_shutdown(isr_params_t *params)
{
    return direct(SC_SHUTDOWN, params);
}

/** recvmmsg(2), the sub-call SC_RECVMMSG. */
long sys_recvmmsg(isr_params_t *params)
{
    return direct(SC_RECVMMSG, params);
}

/** sendmmsg(2), the sub-call SC_SENDMMSG. */
long sys_sendmmsg(isr_params_t *params)
{
    return direct(SC_SENDMMSG, params);
}
