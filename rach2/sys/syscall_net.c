/*
 * The network arm of the system call layer: socketcall and its operations
 * over the socket layer. Registered by syscall.c; what it shares with the
 * other arms is in syscall_internal.h.
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
#include "arch/i686/tls.h"
#include "arch/i686/gdt.h"
#include "lib/krandom.h"
#include "lib/kerrno.h"
#include "lib/ksignal.h"
#include "fs/fd.h"
#include "fs/file.h"
#include "ipc/pipe.h"
#include "io/poll.h"
#include "tty/tty.h"
#include "tty/pty.h"
#include "arch/x86/rtc.h"
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
#include "display/vga_gfx.h"
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
#include "network/net.h"
#include "network/netconf.h" /* netconf_is_broadcast */
#include "cmdline.h"         /* kcmdline_has, for the ENOSYS trace flag */
#include "ipc/unix.h"
#include "syscall_internal.h"

/* Read and write the little-endian words of a caller's own structures (a
 * msghdr's pointers and lengths), which are host order and not wire order. */
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

/* The i386 struct mmsghdr: a struct msghdr, then the count the call stores
 * for it. */
#define MMSGHDR_SIZE 32u
#define MMSG_LEN_OFF 28u

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

/* Rach2's private ICMP-echo socketcall. RENUMBERED out of the BSD range (it
 * used to squat on 15 == Linux getsockopt, which a real server needs).
 * Userspace callers (utils/ping.c, tests/pingtest.c) use the same value. */
#define SC_PING      0x1000
/* The same with the data size and hop limit, for ping's -s and -t. */
#define SC_PING_DATA 0x1001

/**
 * Send one datagram, judging the destination first.
 *
 * A datagram to a broadcast address reaches every machine on the wire, so a
 * socket must have asked for that with `SO_BROADCAST` before it can send one.
 * A program that broadcasts by accident wakes the whole network, and the
 * option exists so that it has to mean it.
 */
int32_t dgram_send(struct socket *sock, uint32_t dst_ip, uint16_t dst_port, const uint8_t *buf,
                   uint16_t len)
{
    int r = socket_dgram_error(sock);

    if (r < 0) {
        return r;
    }
    if (!sock->broadcast && netconf_is_broadcast(dst_ip)) {
        return -EACCES;
    }
    /* One IPv4 datagram over Ethernet: 1500 bytes of frame, less the twenty of
       the IP header and the eight of UDP's. A larger one is refused here rather
       than dropped further down, where this call would have reported it sent. */
    if (len > 1472) {
        return -EMSGSIZE;
    }
    if (sock->local_port == 0 && udp_bind_ephemeral(sock) == 0) {
        return -EAGAIN;
    }
    /* The hop limit is the socket's, which IP_TTL sets and traceroute counts
       up. What the send layer refuses is reported here rather than counted as
       bytes sent. */
    r = udp_tx(sock, dst_ip, dst_port, buf, len);
    if (r < 0) {
        return r;
    }
    return (int32_t)len;
}

/* The raw send buffer: one MTU, filled from user space in syscall context.
   Only syscall context touches it, and this kernel runs one process there at a
   time, so it needs no lock of its own; what needs the lock is the send, which
   goes through the transmit buffers an interrupt also builds in. */
static uint8_t raw_send_buf[1500];

/* Count the ICMP message of `len` bytes in `raw_send_buf` that a raw socket
   sent, when IP took it (`r` is 0), as Linux counts one. */
static void raw_count_sent(const struct socket *sock, int r, int len)
{
    if (r == 0 && sock->protocol == IP_PROTO_ICMP && len > 0) {
        icmp_count_out(raw_send_buf[0]);
    }
}

/* recv flags honored: MSG_PEEK leaves what it copies for the next receive,
 * MSG_TRUNC has recvmsg on a datagram socket answer the datagram's whole
 * length, and MSG_DONTWAIT does not wait. */
#define MSG_PEEK     0x2
#define MSG_TRUNC    0x20
#define MSG_DONTWAIT 0x40

#define IPPROTO_RAW 255

/* The answer of a socket call on a descriptor that is no socket: EBADF for
 * one that is not open, as Linux gives it, and ENOTSOCK for another kind of
 * file. gnulib's tests of accept, bind, connect, listen and setsockopt ask
 * the first of each. */
static int32_t not_a_socket(const fd_entry_t *entry)
{
    return entry ? -ENOTSOCK : -EBADF;
}

#define IPPROTO_ICMP 1

/* setsockopt(2)/getsockopt(2) at the IP level: the hop limit a socket's
 * datagrams leave with, the option that would hand the caller the header,
 * which this stack does not implement, and the IP options a socket's packets
 * carry, of which it keeps none. */
#define SOL_IP 0

#define IP_TTL 2

#define IP_HDRINCL 3

#define IP_OPTIONS 4

/* setsockopt levels and the one packet option that is answered. */
#define SOL_PACKET 263

#define PACKET_AUXDATA 8

/* We read sin_family/sin_port/sin_addr from the first 8 sockaddr bytes. */
#define SOCKADDR_MIN 8

/* The whole of a struct sockaddr_in: family (2), port (2), address (4), and
 * eight bytes of padding. `SOCKADDR_MIN` above is only the part that is read.
 */
#define SOCKADDR_IN_LEN 16

/* struct sockaddr_ll, the address a packet socket binds and sends to, with the
 * offset of each field beside it: family 0 (2 bytes), protocol 2 (2), ifindex
 * 4 (4), hatype 8 (2), pkttype 10 (1), halen 11 (1), addr 12 (8), for 20 in
 * all. A caller declaring fewer bytes is refused, not read past. */
#define SOCKADDR_LL_LEN 20

#define SOCKADDR_LL_PROTOCOL 2

#define SOCKADDR_LL_IFINDEX 4

#define SOCKADDR_LL_HATYPE 8

#define ARPHRD_ETHER 1 /* sll_hatype for an Ethernet interface */

#define SOCKADDR_LL_HALEN 11

#define SOCKADDR_LL_ADDR 12

/**
 * Write an AF_INET source/peer sockaddr back to a caller's (addr, addrlen)
 * pair.
 *
 * Respects the caller's declared buffer capacity: read the in/out socklen,
 * copy at most that many of the 16 sockaddr_in bytes, and report the true
 * length (16), the recvfrom(2)/accept(2) truncation semantics. Returns 0 or
 * -EFAULT.
 */
static int copy_sockaddr_to_user(uint32_t sa_ptr, uint32_t salen_ptr, uint16_t port, uint32_t ip)
{
    uint8_t sa[16];
    uint32_t caller_len;
    uint32_t copy_len;

    if (!sa_ptr || !salen_ptr) {
        return 0; /* caller does not want the address back */
    }

    if (copy_from_user(&caller_len, (const void __user *)salen_ptr, sizeof(caller_len)) != 0) {
        return -EFAULT;
    }

    k_memset(sa, 0, sizeof(sa));
    /* sin_family is in HOST byte order (little-endian on i386), NOT network
     * order: a big-endian AF_INET would read back as 0x0200 and a peer-address
     * consumer (dropbear's connection logging) rejects it as an unknown family.
     * Only sin_port and sin_addr are network (big-endian). */
    sa[0] = 2; /* AF_INET, low byte */
    sa[1] = 0;
    SC_WBE16(sa + 2, port);
    SC_WBE32(sa + 4, ip);

    copy_len = (caller_len < sizeof(sa)) ? caller_len : (uint32_t)sizeof(sa);
    if (copy_len > 0 && copy_to_user((void __user *)sa_ptr, sa, copy_len) != 0) {
        return -EFAULT;
    }

    /* Report the real sockaddr_in length even when the caller's buffer was
     * shorter (and thus the address was truncated). */
    caller_len = (uint32_t)sizeof(sa);
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

/* The kinds of socket socket(2) makes: AF_INET with SOCK_STREAM, SOCK_DGRAM
 * or SOCK_RAW (ICMP, or the ioctl-only descriptor of IPPROTO_RAW), AF_PACKET
 * with SOCK_DGRAM, and AF_NETLINK with SOCK_RAW or SOCK_DGRAM, which are one
 * thing, since the kernel's answers are datagrams: `*type` is set to
 * SOCK_DGRAM for it. The raw and packet forms take effective uid 0; a
 * netlink protocol is judged when the socket is made. 0, or -errno. */
static int32_t socket_kind(int domain, int *type, int protocol)
{
    /* IPv4, the packet family the DHCP client needs to see replies that
     * arrive before this machine owns an address, and netlink. Any other
     * family is refused, not made into a socket that cannot work. */
    if (domain != AF_INET && domain != AF_PACKET && domain != AF_NETLINK) {
        return -EAFNOSUPPORT;
    }
    if (domain == AF_NETLINK) {
        if (*type != SOCK_RAW && *type != SOCK_DGRAM) {
            return -ESOCKTNOSUPPORT;
        }
        *type = SOCK_DGRAM;
        return 0;
    }
    /* The query descriptor. A DHCP client opens AF_INET/SOCK_RAW/IPPROTO_RAW
     * and does nothing with it but ask the interface ioctls about the machine
     * (busybox's `udhcp_read_interface`), so that is what it is here: a
     * descriptor that accepts ioctls and closes, and refuses to send or
     * receive, because this stack has no raw IP path to offer. Root's, like
     * every raw socket. */
    if (domain == AF_INET && *type == SOCK_RAW) {
        /* Two raw sockets, and the protocol tells them apart. IPPROTO_RAW is
         * the query descriptor a DHCP client opens for the interface ioctls
         * and nothing else; IPPROTO_ICMP is the one `ping` and `traceroute`
         * send and receive on. Both are root's. */
        if (protocol != IPPROTO_RAW && protocol != IPPROTO_ICMP) {
            return -EPROTONOSUPPORT;
        }
        if (!current_proc || !proc_privileged(current_proc)) {
            return -EPERM;
        }
    } else if (*type != SOCK_DGRAM && *type != SOCK_STREAM) {
        return -EINVAL;
    }
    if (domain == AF_PACKET) {
        /* It reads every frame of its protocol the machine receives and sends
         * frames the IP layer never saw, so it is root's. The protocol is in
         * network byte order, as Linux takes it, and the two this stack has
         * frames for are IPv4 and ARP. */
        if (!current_proc || !proc_privileged(current_proc)) {
            return -EPERM;
        }
        if (*type != SOCK_DGRAM) {
            return -ESOCKTNOSUPPORT;
        }
        /* Zero is legal and means the socket has no protocol yet: `arping`
         * opens one that way and names ARP in its bind. */
        if ((uint16_t)protocol != 0 && (uint16_t)protocol != PACKET_PROTO_IP &&
            (uint16_t)protocol != PACKET_PROTO_ARP) {
            return -EINVAL;
        }
    }
    return 0;
}

/* socket(2): a socket of a kind `socket_kind` allows. Returns a new socket
 * fd or -errno. */
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
    if (domain == AF_INET && type == SOCK_RAW && protocol == IPPROTO_ICMP) {
        int reg;

        sock->protocol = (uint16_t)protocol;
        reg = raw_register(sock, (uint8_t)protocol);
        if (reg != 0) {
            socket_unref(sock);
            return reg;
        }
    }
    if (domain == AF_PACKET) {
        /* The argument is in network order, as Linux takes it; the socket
         * holds it in host order, which is what the wire comparison uses. */
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

/* Bind the netlink socket `sock` to the port id and the groups of the
 * sockaddr_nl of `addrlen` bytes at user address `sa_ptr`. */
static int32_t netlink_bind_user(struct socket *sock, uint32_t sa_ptr, int addrlen)
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
 * datagram socket's registration and a stream's listener.
 */
static int32_t bind_inet(struct socket *sock, uint32_t ip, uint16_t port)
{
    if (ip != 0 && !ip_is_local(ip) && !(sock->type == SOCK_DGRAM && netdev_is_broadcast(ip))) {
        return -EADDRNOTAVAIL;
    }

    /* A port below 1024 is a service's, and a service is root's. Without this
     * any user could take the port a DHCP client or an SSH server answers on
     * and be the one machines on the wire talk to. */
    if (port != 0 && port < 1024 && (!current_proc || !proc_privileged(current_proc))) {
        return -EACCES;
    }

    if (sock->type == SOCK_DGRAM) {
        /* A datagram send before bind auto-binds an ephemeral source port; drop
         * that registration before claiming the requested one, or its
         * port-table slot leaks for the socket's life. */
        if (sock->local_port != 0) {
            udp_unbind(sock);
            sock->local_port = 0;
        }
        sock->local_ip = ip;
        if (port == 0) {
            /* Port 0 means "any port": let the stack pick a free ephemeral and
             * record it, the same path a send before bind takes. */
            if (udp_bind_ephemeral(sock) == 0) {
                sock->local_ip = 0;
                return -EADDRINUSE;
            }
            return 0;
        }
        if (udp_bind(port, sock) < 0) {
            sock->local_ip = 0;
            return -EADDRINUSE;
        }
    }
    if (sock->type == SOCK_STREAM && port == 0) {
        /* Port 0 means any port, and a stream is given one at the bind, as on
         * Linux, so that `getsockname` reports it before `listen`. */
        port = tcp_bind_ephemeral();
        if (port == 0) {
            return -EADDRINUSE;
        }
    } else if (sock->type == SOCK_STREAM && tcp_port_listening(ip, port)) {
        /* Advisory, and where a server reports it: the invariant belongs to
         * `tcp_listen`, since `bind` and `listen` are separate syscalls and any
         * check made only here is bypassed by binding both sockets first. */
        return -EADDRINUSE;
    }
    sock->local_ip = ip;
    sock->local_port = port;
    return 0;
}

/* bind(2): record the local address and port from the sockaddr; a datagram
 * socket also registers them with UDP (-EADDRINUSE if taken), and a netlink
 * socket takes a port id (`netlink_bind_user`). The address must be any (0),
 * one of 127/8, an interface's, or for a datagram socket a broadcast address;
 * any other is refused with -EADDRNOTAVAIL, as Linux refuses it. */
static int32_t do_bind(int fd, uint32_t sa_ptr, int addrlen)
{
    fd_entry_t *entry;
    uint8_t sa[SOCKADDR_MIN];

    /* The caller's declared sockaddr must be at least the header we read; a
     * short or negative addrlen must not make us read past it. */
    if (addrlen < SOCKADDR_MIN) {
        return -EINVAL;
    }

    /* Copy the sockaddr header into a kernel buffer; the BE helpers below then
     * operate on the kernel copy. */
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
    if (socket_outside(entry->sock)) {
        return 0; /* a namespace with no interface: nothing is delivered to it */
    }

    /* A packet socket binds to an interface, not to a port: the address is a
     * sockaddr_ll and binding is what puts it in the delivery table. */
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
        /* A protocol named here replaces the one `socket` was given, which is
         * Linux's rule and is how a client that opened with 0 says what it
         * wants. Naming none leaves the socket's own. The change is made only
         * after the bind is accepted: a bind that fails must leave the socket
         * receiving exactly what it received before. */
        proto = SC_BE16(sll + SOCKADDR_LL_PROTOCOL);
        if (proto != 0 && proto != ETH_TYPE_IP && proto != ETH_TYPE_ARP) {
            return -EINVAL;
        }
        r = packet_bind(entry->sock, (int)ifindex);
        if (r != 0) {
            return r;
        }
        if (proto != 0 && proto != entry->sock->protocol) {
            entry->sock->protocol = proto;
            /* What is queued arrived under the old protocol. A reader would
               be given it labelled with the new one, so it goes. */
            packet_flush(entry->sock);
        }
        return 0;
    }

    /* A raw socket binds to an address, not to a port: it says which of the
     * machine's addresses it wants datagrams for. `traceroute` binds its
     * receive socket to the address its send socket ended up with, and dies if
     * it cannot. The bind is judged against the addresses the machine has and
     * filters nothing afterwards, which entry 116 of the shortcomings list
     * records; an address the machine does not have is
     * refused rather than accepted and quietly ignored. */
    if (entry->sock->type == SOCK_RAW) {
        uint32_t want;

        if (sock_is_query(entry->sock)) {
            return -EOPNOTSUPP;
        }
        want = SC_BE32(sa + 4);
        if (want != 0 && !ip_is_local(want)) {
            return -EADDRNOTAVAIL;
        }
        return 0;
    }

    return bind_inet(entry->sock, SC_BE32(sa + 4), SC_BE16(sa + 2));
}

/**
 * Send `len` bytes at `buf`, of at most one UDP packet, from the datagram
 * socket of `entry` to the sockaddr at `sa_ptr`, or with none to the address
 * it is connected to. One send path, so the broadcast permission and the
 * ephemeral bind are the same for sendto, sendmsg and a write to a connected
 * socket.
 */
static int32_t udp_sendto(fd_entry_t *entry, const uint8_t *buf, int len, uint32_t sa_ptr,
                          int addrlen)
{
    uint8_t sa[SOCKADDR_MIN];

    /* A connected datagram socket needs no destination: `send` is `sendto`
     * with none, which is what musl's send() issues. */
    if (sa_ptr == 0 || addrlen == 0) {
        if (!entry->sock->connected) {
            return -EDESTADDRREQ;
        }
        return dgram_send(entry->sock, entry->sock->remote_ip, entry->sock->remote_port, buf,
                          (uint16_t)len);
    }
    if (addrlen < SOCKADDR_MIN) {
        return -EINVAL; /* declared sockaddr shorter than the header we read */
    }
    if (copy_from_user(sa, (const void __user *)sa_ptr, SOCKADDR_MIN) != 0) {
        return -EFAULT;
    }
    return dgram_send(entry->sock, SC_BE32(sa + 4), SC_BE16(sa + 2), buf, (uint16_t)len);
}

/* The length of the sockaddr a receive on `sock` reports its sender in. */
static uint32_t sockaddr_len_of(const struct socket *sock)
{
    if (sock->family == AF_PACKET) {
        return (uint32_t)SOCKADDR_LL_LEN;
    }
    if (sock->family == AF_NETLINK) {
        return (uint32_t)SOCKADDR_NL_LEN;
    }
    return (uint32_t)SOCKADDR_IN_LEN;
}

/* Policy: a netlink request is at most 64 KiB, far past the few dozen bytes
 * of the requests the socket answers. */
#define NETLINK_SEND_MAX 65536

/* Send the netlink message of `len` bytes at user address `buf_ptr`. */
static int32_t netlink_send_user(struct socket *sock, uint32_t buf_ptr, int len)
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
    rc = netlink_send(sock, copy, (uint32_t)len);
    kfree(copy);
    return rc;
}

/* Copy the sockaddr_nl `nl` to the caller's (sa_ptr, salen_ptr), truncated
 * to its capacity, with the true length written back. */
static int32_t netlink_addr_to_user(const uint8_t *nl, uint32_t sa_ptr, uint32_t salen_ptr)
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

/* sendto(2): a stream socket ignores the destination and behaves like send; a
 * datagram socket sends to the sockaddr, binding an ephemeral source port
 * first. */
static int32_t do_sendto(int fd, uint32_t buf_ptr, int len, int flags, uint32_t sa_ptr, int addrlen)
{
    fd_entry_t *entry;

    if (len < 0 || !user_buffer_ok(buf_ptr, (uint32_t)len, 0)) {
        return -EFAULT;
    }

    entry = fd_get(current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }

    /* The query descriptor accepts ioctls and nothing else: it has no protocol
     * to send under, and a caller that got silence here would be waiting for a
     * datagram that cannot arrive. */
    if (sock_is_query(entry->sock)) {
        return -EOPNOTSUPP;
    }

    /* A netlink socket's message goes to the kernel, which is the only
     * destination it has. */
    if (entry->sock->family == AF_NETLINK) {
        return netlink_send_user(entry->sock, buf_ptr, len);
    }
    if (socket_outside(entry->sock)) {
        return -ENETUNREACH; /* a network namespace with no interface */
    }

    /* A raw socket sends the ICMP message the caller built, checksum included:
     * this layer supplies the IPv4 header and rewrites nothing, since a
     * program sending a wrong checksum on purpose is testing something. */
    if (entry->sock->type == SOCK_RAW) {
        uint8_t raw_sa[SOCKADDR_IN_LEN];
        uint32_t raw_dst;
        int r;

        if (!sa_ptr || addrlen < SOCKADDR_MIN) {
            return -EDESTADDRREQ;
        }
        k_memset(raw_sa, 0, sizeof(raw_sa));
        if (copy_from_user(raw_sa, (const void __user *)sa_ptr, SOCKADDR_MIN) != 0) {
            return -EFAULT;
        }
        if ((uint16_t)((uint16_t)raw_sa[0] | ((uint16_t)raw_sa[1] << 8)) != AF_INET) {
            return -EAFNOSUPPORT;
        }
        if (len > (int)sizeof(raw_send_buf)) {
            return -EMSGSIZE;
        }
        raw_dst = SC_BE32(raw_sa + 4);
        /* The copy runs with interrupts on, since it may fault; the send is
           then held against a receive interrupt that would build a datagram
           through the same transmit buffers, which is the rule in net.h and
           what udp_tx does for the same reason. */
        if (copy_from_user(raw_send_buf, (const void __user *)buf_ptr, (uint32_t)len) != 0) {
            return -EFAULT;
        }
        {
            uint32_t netflags = net_lock();
            r = ip_tx(raw_dst, (uint8_t)entry->sock->protocol, raw_send_buf, (uint16_t)len,
                      entry->sock->ip_ttl);
            raw_count_sent(entry->sock, r, len);
            net_unlock(netflags);
        }
        if (r < 0) {
            return r;
        }
        return (int32_t)len;
    }

    /* On a connected stream socket, sendto with any (or no) destination behaves
     * like send; the connection's peer is the destination, so the sockaddr is
     * ignored (this is the path musl's send() takes: sendto with a NULL addr).
     */
    if (entry->sock->type == SOCK_STREAM) {
        int nonblock =
            (flags & MSG_DONTWAIT) || (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));
        return (int32_t)socket_tcp_send(
            entry->sock, (const uint8_t __force *)(const void __user *)buf_ptr, len, nonblock);
    }

    /* A packet socket sends an IP packet the caller built, to a MAC address it
     * names: this layer adds the Ethernet header and nothing else. */
    if (entry->sock->family == AF_PACKET) {
        uint8_t sll[SOCKADDR_LL_LEN];
        uint16_t proto;
        int32_t ifindex;

        if (len > 1500) {
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
        /* An interface named here is the one the frame leaves by, which is
           what the field is for; zero leaves the choice to the bind. */
        ifindex = (int32_t)((uint32_t)sll[SOCKADDR_LL_IFINDEX] |
                            ((uint32_t)sll[SOCKADDR_LL_IFINDEX + 1] << 8) |
                            ((uint32_t)sll[SOCKADDR_LL_IFINDEX + 2] << 16) |
                            ((uint32_t)sll[SOCKADDR_LL_IFINDEX + 3] << 24));
        /* The payload is read where it lies, as the UDP path reads it: the
           range above was validated, and __force strips __user at the call. */
        return (int32_t)packet_send(entry->sock, (int)ifindex, sll + SOCKADDR_LL_ADDR, proto,
                                    (const uint8_t __force *)(const void __user *)buf_ptr,
                                    (uint16_t)len);
    }

    /* The payload must fit one unfragmented UDP packet (this stack does not
     * fragment; udp_tx caps it at 1500). Reject an oversize datagram with
     * -EMSGSIZE, not truncate it through the uint16_t wire length or let
     * udp_tx silently drop it while this call reports a full send. */
    if (len > 1500) {
        return -EMSGSIZE;
    }

    /* udp_tx reads the payload itself; __force strips __user at the
     * boundary. */
    return udp_sendto(entry, (const uint8_t __force *)(const void __user *)buf_ptr, len, sa_ptr,
                      addrlen);
}

/* recvfrom(2): a stream socket behaves like recv (no source writeback); a
 * datagram socket reports the source address, truncated to the caller's
 * capacity. An address with no length pointer is EFAULT before anything is
 * taken, so the message stays queued; Linux fails the call only after it
 * has taken the message. Honors MSG_PEEK, MSG_DONTWAIT and O_NONBLOCK. */
static int32_t do_recvfrom(int fd, uint32_t buf_ptr, int len, int flags, uint32_t sa_ptr,
                           uint32_t salen_ptr)
{
    fd_entry_t *entry;
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    uint32_t addr_len = 0;
    uint32_t addr_give = 0;
    int want_addr;
    int bytes;
    int nonblock;
    int peek = (flags & MSG_PEEK) != 0;

    if (len < 0 || !user_buffer_ok(buf_ptr, (uint32_t)len, 1)) {
        return -EFAULT;
    }
    entry = fd_get(current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    /* Where the source address will land is settled here, before a datagram is
     * taken: taking one destroys it, and a fault found afterwards would report
     * an error for a message that no longer exists. The caller's capacity is
     * read once and kept, so that a caller whose length word overlaps its own
     * receive buffer cannot change it under the writeback. What is checked is
     * what will actually be written: twenty bytes for a packet socket's
     * sockaddr_ll, sixteen for an AF_INET sender. */
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

    /* The query descriptor accepts ioctls and nothing else: nothing is
     * delivered to a descriptor with no protocol, and a caller that got
     * silence here would be waiting for a datagram that cannot arrive. */
    if (sock_is_query(entry->sock)) {
        return -EOPNOTSUPP;
    }

    /* Receive without blocking when the caller asked per-call (MSG_DONTWAIT) or
     * the descriptor is O_NONBLOCK (an OFD status flag, shared by dup/fork). */
    nonblock = (flags & MSG_DONTWAIT) || (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));

    /* On a connected stream socket, recvfrom behaves like recv; the peer is the
     * connection's, so the optional source-address writeback is skipped and a
     * NULL sockaddr is fine (this is the path musl's recv() takes: recvfrom
     * with a NULL addr). */
    if (entry->sock->type == SOCK_STREAM) {
        if (nonblock) {
            return (int32_t)socket_tcp_try_recv(
                entry->sock, (uint8_t __force *)(void __user *)buf_ptr, len, peek);
        }
        return (int32_t)socket_tcp_recv(entry->sock, (uint8_t __force *)(void __user *)buf_ptr, len,
                                        peek);
    }

    /* socket_recv/socket_try_recv write straight into the validated user
     * buffer; __force strips __user at the boundary. */
    if (nonblock) {
        bytes = socket_try_recv(entry->sock, (uint8_t __force *)(void __user *)buf_ptr, len,
                                &src_ip, &src_port, peek);
    } else {
        bytes = socket_recv(entry->sock, (uint8_t __force *)(void __user *)buf_ptr, len, &src_ip,
                            &src_port, peek);
    }

    /* A receive that failed leaves the caller's address arguments alone: there
     * is no sender to report, and the values that would be written are the ones
     * this frame never set. */
    if (bytes < 0) {
        return bytes;
    }

    /* Report the source address, truncated to the capacity read before the
     * datagram was taken. A packet socket reports the link address the frame
     * came from, a sockaddr_ll; an AF_INET one reports the sender. */
    if (want_addr) {
        uint8_t sa[SOCKADDR_LL_LEN];

        k_memset(sa, 0, sizeof(sa));
        if (entry->sock->family == AF_NETLINK) {
            sa[0] = (uint8_t)AF_NETLINK; /* the kernel, port id 0 */
        } else if (entry->sock->family == AF_PACKET) {
            if (packet_last_addr(entry->sock, sa) == 0) {
                addr_len = 0;
                addr_give = 0;
            }
        } else {
            sa[0] = (uint8_t)(AF_INET & 0xff);
            SC_WBE16(sa + 2, src_port);
            SC_WBE32(sa + 4, src_ip);
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

static int32_t do_ping(uint32_t dst_ip)
{
    return (int32_t)icmp_ping(dst_ip);
}

/** A peer's FIN does not undo the completed handshake. */
static int connect_established(struct socket *sock)
{
    return sock->tcb && (sock->tcb->state == TCP_ESTABLISHED || sock->tcb->state == TCP_CLOSE_WAIT);
}

/** Wake once the handshake has completed or the connection has failed. */
static int connect_done(void *arg)
{
    struct socket *sock = (struct socket *)arg;
    return sock->tcb == NULL || connect_established(sock) || sock->tcb->state == TCP_CLOSED;
}

/* Wait predicate: a listening socket has a connection queued for accept. */
static int accept_ready(void *arg)
{
    struct socket *sock = (struct socket *)arg;
    return sock->accept_head != sock->accept_tail;
}

/* connect(2): start the TCP handshake to the sockaddr. A blocking socket waits
 * it out (-EINTR on a signal); a non-blocking one returns 0 or -EINPROGRESS. */
static int32_t do_connect(int fd, uint32_t sa_ptr, int addrlen)
{
    fd_entry_t *entry;
    uint32_t dst_ip;
    uint16_t dst_port;
    uint16_t family;
    uint8_t sa[SOCKADDR_MIN];
    int rc;

    /* The family is read first and on its own, because a disconnect is a
     * family and nothing else: `connect(fd, &unspec, sizeof(sa_family_t))` is
     * how a program lets go of a peer, and requiring a whole address for it
     * would leave that program connected. */
    /* The descriptor is resolved before its argument is read, so that a call on
     * something that is not a socket says so instead of reporting whatever was
     * wrong with an address it was never going to use. */
    entry = fd_get(current_proc->fds, fd);
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    if (entry->sock->type == SOCK_RAW) {
        return -EOPNOTSUPP;
    }
    /* A netlink socket's one peer is the kernel, which a connect names and
     * which it has already. */
    if (entry->sock->family == AF_NETLINK) {
        return 0;
    }
    /* A packet socket has no peer to connect to: it is bound to an interface
     * and sends to a hardware address each time. Reaching the datagram arm
     * below would put it in the UDP port table, which its teardown never
     * unbinds from. */
    if (entry->sock->family == AF_PACKET) {
        return -EOPNOTSUPP;
    }
    if (socket_outside(entry->sock)) {
        return -ENETUNREACH; /* a network namespace with no interface */
    }

    k_memset(sa, 0, sizeof(sa));
    if (addrlen < 2) {
        return -EINVAL;
    }
    if (copy_from_user(sa, (const void __user *)sa_ptr, 2) != 0) {
        return -EFAULT;
    }
    family = (uint16_t)((uint16_t)sa[0] | ((uint16_t)sa[1] << 8));
    /* Two bytes are enough for a disconnect and only for a disconnect, and only
     * a datagram socket has one to do: a stream's connection ends by closing
     * it. Anything else must give a whole address, or the bytes past the family
     * would be this function's stack. */
    if (family == 0) {
        if (entry->sock->type != SOCK_DGRAM) {
            return -EAFNOSUPPORT;
        }
    } else {
        if (addrlen < SOCKADDR_MIN) {
            return -EINVAL; /* declared sockaddr shorter than the header we read */
        }
        if (copy_from_user(sa, (const void __user *)sa_ptr, SOCKADDR_MIN) != 0) {
            return -EFAULT;
        }
    }
    /* A datagram socket's connect records where it will send and who it will
     * hear from; there is no handshake and nothing to wait for. AF_UNSPEC
     * undoes it, which is how a program lets go of a peer. */
    if (entry->sock->type == SOCK_DGRAM) {
        uint32_t held;

        if (family == 0) {
            held = net_lock();
            entry->sock->connected = 0;
            entry->sock->remote_ip = 0;
            entry->sock->remote_port = 0;
            net_unlock(held);
            return 0;
        }
        if (family != AF_INET) {
            return -EAFNOSUPPORT;
        }
        /* Something must answer, so the socket needs a source port: the same
         * ephemeral bind a send before bind would have taken. The bind and the
         * peer are published together, under one hold. A bind that landed first
         * would put the socket in the port table with no peer recorded, and a
         * datagram arriving in that interval would be admitted from anyone; a
         * reconnect that published a new port beside an old address would admit
         * a sender that is neither the old peer nor the new one. */
        held = net_lock();
        if (entry->sock->local_port == 0 && udp_bind_ephemeral(entry->sock) == 0) {
            net_unlock(held);
            return -EAGAIN;
        }
        entry->sock->remote_port = SC_BE16(sa + 2);
        entry->sock->remote_ip = SC_BE32(sa + 4);
        entry->sock->connected = 1;
        net_unlock(held);
        return 0;
    }
    if (entry->sock->type != SOCK_STREAM) {
        return -EOPNOTSUPP;
    }
    if (entry->sock->listening || entry->sock->tcb) {
        /* A socket that is already listening, connecting, or connected keeps
         * its TCB; a second tcp_connect would allocate another and leak the
         * first, whose stale delivery could reach a since-reused socket. */
        return (entry->sock->tcb && entry->sock->tcb->state == TCP_SYN_SENT) ? -EALREADY : -EISCONN;
    }

    dst_port = SC_BE16(sa + 2);
    dst_ip = SC_BE32(sa + 4);

    {
        /* Verbatim: the refusal a closed port earns comes from the handshake,
         * not from here, so collapsing a full pool or a duplicate pairing into
         * ECONNREFUSED would report a local failure as the peer's answer. */
        int opened = tcp_connect(entry->sock, dst_ip, dst_port);

        if (opened < 0) {
            return opened;
        }
    }

    /* A non-blocking socket does not wait out the handshake: report it as
     * in-progress (or already done) and let the caller poll for completion. */
    if (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK)) {
        if (connect_established(entry->sock)) {
            return 0;
        }
        return -EINPROGRESS;
    }

    /* Block until the handshake completes, fails, or times out, yielding to
     * other processes; a pending signal unwinds with -EINTR. */
    rc = wq_wait_event(&entry->sock->waiters, connect_done, entry->sock);
    if (rc < 0) {
        return rc;
    }

    if (!connect_established(entry->sock)) {
        return -ECONNREFUSED;
    }
    return 0;
}

/* listen(2): mark a stream socket passive and open its LISTEN TCB. */
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
    if (socket_outside(entry->sock)) {
        entry->sock->listening = 1; /* nothing reaches it to connect */
        return 0;
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
 * is EFAULT, as in Linux.
 */
static int32_t do_accept(int fd, uint32_t sa_ptr, uint32_t salen_ptr, uint32_t flags)
{
    fd_entry_t *entry;
    fd_entry_t *new_entry;
    struct socket *listen_sock;
    struct socket *conn_sock;
    int new_fd;

    /* The peer-address writeback is validated and truncated to the caller's
     * declared length by copy_sockaddr_to_user below. */

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

    /* A non-blocking listener returns immediately when nothing is queued. */
    if (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK) &&
        listen_sock->accept_head == listen_sock->accept_tail) {
        return -EAGAIN;
    }

    /* Block until a connection is queued, yielding to other processes; a
     * pending signal unwinds with -EINTR. */
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

    /* The queued socket already holds the reference created when tcp_rx
     * accepted it; the new descriptor takes ownership of that reference. */
    if ((sa_ptr && !salen_ptr) || copy_sockaddr_to_user(sa_ptr, salen_ptr, conn_sock->remote_port,
                                                        conn_sock->remote_ip) != 0) {
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

/* send(2): write to the socket's connected peer. On a stream that is the
 * connection's; any other socket sends as sendto(2) with no address does,
 * which is what POSIX makes send, and what glibc's DNS resolver asks of a
 * connected UDP socket through subcall 9. */
static int32_t do_send(int fd, uint32_t buf_ptr, int len, int flags)
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
    if (socket_outside(entry->sock)) {
        return -ENETUNREACH;
    }

    /* socket_tcp_send copies the payload itself, blocking on send-ring space
     * unless asked not to; __force strips __user at the boundary. */
    nonblock = (flags & MSG_DONTWAIT) || (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));
    return (int32_t)socket_tcp_send(
        entry->sock, (const uint8_t __force *)(const void __user *)buf_ptr, len, nonblock);
}

/* recv(2): read from a connected stream socket. Honors MSG_PEEK,
 * MSG_DONTWAIT and O_NONBLOCK. */
static int32_t do_recv(int fd, uint32_t buf_ptr, int len, int flags)
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
    /* Don't block when the caller asked per-call (MSG_DONTWAIT) or the
     * descriptor is O_NONBLOCK. socket_tcp_*recv write into the validated user
     * buffer; __force strips __user at the boundary. */
    nonblock = (flags & MSG_DONTWAIT) || (entry->ofd && (entry->ofd->status_flags & OFD_NONBLOCK));

    /* A datagram socket reads one message with no sender reported, which is
     * what `recv` is: `recvfrom` without the address. Refusing it here would
     * leave one of the four ways of reading a socket disagreeing with the
     * other three about whether the same queued datagram exists. */
    if (entry->sock->type == SOCK_DGRAM) {
        if (nonblock) {
            return (int32_t)socket_try_recv(entry->sock, (uint8_t __force *)(void __user *)buf_ptr,
                                            len, 0, 0, peek);
        }
        return (int32_t)socket_recv(entry->sock, (uint8_t __force *)(void __user *)buf_ptr, len, 0,
                                    0, peek);
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
 * binding has index zero and no MAC.
 */
static int32_t do_getsockname(int fd, uint32_t sa_ptr, uint32_t salen_ptr)
{
    fd_entry_t *entry = fd_get(current_proc->fds, fd);
    uint32_t flags, local_ip;

    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    if (entry->sock->type == SOCK_RAW) {
        return -EOPNOTSUPP;
    }
    /* Unlike accept/recvfrom, getsockname requires the buffers
     * (copy_sockaddr_to_user treats NULL as "no address wanted"). */
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
        struct netdev *nd = netdev_at(ifindex - 1);

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
    } else if (entry->sock->local_ip == 0 && entry->sock->connected) {
        local_ip = ip_source(entry->sock->remote_ip);
    } else {
        local_ip = entry->sock->local_ip;
    }
    net_unlock(flags);
    return copy_sockaddr_to_user(sa_ptr, salen_ptr, entry->sock->local_port, local_ip);
}

/* getpeername: the connected peer's address (an established stream socket
 * or a datagram socket that connect(2) gave a peer). */
static int32_t do_getpeername(int fd, uint32_t sa_ptr, uint32_t salen_ptr)
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
    /* A peer exists once the TCP handshake completes (not a listener, not a
     * still-connecting SYN_SENT, not a closed socket), or once a datagram
     * socket has been connected to one, which is a peer with no handshake. */
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
                                 entry->sock->remote_ip);
}

/* shutdown(how): SHUT_RD(0)/SHUT_WR(1)/SHUT_RDWR(2). SHUT_WR closes the
 * sending half: the FIN goes after the queued data, a later send is EPIPE,
 * and the peer's data and FIN still reach the reader, as `nc` needs when it
 * has sent its input and reads the answer. SHUT_RDWR closes the connection as
 * close() does, and the socket reads end of input. SHUT_RD alone is accepted
 * as a no-op. */
#define SHUT_RD 0

#define SHUT_WR 1

#define SHUT_RDWR 2

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
    /* Only a connected stream socket can be shut down, NOT a listener (which
     * would free its LISTEN TCB and silently stop accepting), an unconnected,
     * or a UDP socket. */
    if (!tcp_is_connected(entry->sock)) {
        return -ENOTCONN;
    }
    if (how == SHUT_WR) {
        tcp_shutdown_send(entry->sock);
        wq_wake_all(&entry->sock->waiters);
    } else if (how == SHUT_RDWR) {
        entry->sock->shut_down = 1;
        tcp_close(entry->sock); /* send FIN; idempotent vs the later close() */
        /* `tcp_close` detaches the socket from its block both ways, so no
           later answer from the peer can reach this queue. A sharer already
           asleep in `recv` is woken here or never. */
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

/** setsockopt: accept the boolean options a forking server sets (SO_REUSEADDR,
 * TCP_NODELAY, SO_KEEPALIVE) so its setup succeeds. The stack already reuses
 * listen ports and sends promptly, so these are honored-by-behavior or
 * documented no-ops; an unknown option is accepted instead of failing a
 * server's startup. */
static int32_t do_setsockopt(int fd, int level, int optname, uint32_t optval_ptr, int optlen)
{
    fd_entry_t *entry = fd_get(current_proc->fds, fd);
    int val = 0;

    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    /* The one option this stack answers with a refusal, not a blanket
     * success. A DHCP client asks a packet socket for PACKET_AUXDATA so that it
     * can be told the kernel has not checked a datagram's UDP checksum; there
     * is no such offload here and no ancillary data at all, and a success would
     * promise data that never arrives. Refused, the client verifies the
     * checksum itself, which is the outcome that is true. */
    if (level == SOL_PACKET && optname == PACKET_AUXDATA) {
        return -ENOPROTOOPT;
    }
    (void)optname;
    if (level == SOL_SOCKET && optname == SO_BINDTODEVICE) {
        char name[IFNAMSIZ];
        struct netdev *nd;
        uint32_t take = (optlen < 0) ? 0u : (uint32_t)optlen;

        /* The name is at the start of whatever the caller passed, and busybox
         * passes a whole `struct ifreq` for it, 32 bytes on this machine. So
         * only the name's worth is read and a larger buffer is not a refusal.
         */
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

    /* A declared int-sized option with a NULL value is a bad pointer, not a
     * silently-accepted no-op. */
    if (optlen >= (int)sizeof(int)) {
        if (!optval_ptr ||
            copy_from_user(&val, (const void __user *)optval_ptr, sizeof(val)) != 0) {
            return -EFAULT;
        }
    }

    /* The hop limit is real, because traceroute is nothing but this option
     * counted up: a stack that accepted it and sent every probe with the same
     * limit would answer at the first hop every time and look like a machine
     * one hop from everywhere. */
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
    /* And the option that would hand the caller the IP header to fill in is
     * refused rather than accepted and ignored, which would send a datagram
     * whose header the caller believes it wrote. */
    if (level == SOL_IP && optname == IP_HDRINCL) {
        return -ENOPROTOOPT;
    }

    /* The two flags this stack really keeps. Everything else is accepted and
     * has no effect, which is what it has always done: refusing them would
     * stop programs that set options a small stack has no use for. */
    if (level == SOL_SOCKET && (optname == SO_BROADCAST || optname == SO_REUSEADDR)) {
        /* A flag whose value was too short to read is not a flag set to zero:
           the caller passed something this could not understand. */
        if (optlen < (int)sizeof(int)) {
            return -EINVAL;
        }
        if (optname == SO_BROADCAST) {
            entry->sock->broadcast = val ? 1 : 0;
        } else {
            entry->sock->reuseaddr = val ? 1 : 0;
        }
    }
    return 0;
}

/** getsockopt: report the options programs read at setup: SO_ERROR (the
 * pending connection error, cleared by the read), SO_TYPE (the real socket
 * type), SO_REUSEADDR and SO_BROADCAST as stored, SO_BINDTODEVICE as the
 * interface name, and IP_OPTIONS as empty, a result of width 0, since no
 * socket keeps IP options: OpenSSH's server ends a connection whose socket
 * reports any. A result is truncated to the caller's capacity, and the length
 * written is what was copied. */
static int32_t do_getsockopt(int fd, int level, int optname, uint32_t optval_ptr,
                             uint32_t optlen_ptr)
{
    fd_entry_t *entry = fd_get(current_proc->fds, fd);
    int val = 0;
    uint32_t olen, width = (uint32_t)sizeof(val);

    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    if (level == SOL_SOCKET && optname == SO_TYPE) {
        val = entry->sock->type;
    } else if (level == SOL_SOCKET && optname == SO_ERROR) {
        /* Report and clear the connection's pending async error (the standard
         * non-blocking-connect completion check). Read-and-clear under net_lock
         * so the IRQ that sets it cannot race the clear. */
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
        struct netdev *nd = netdev_at(entry->sock->bound_device - 1);

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
        give = k_strlen(name) + 1u;
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
    } else if (level == SOL_SOCKET && optname == SO_BROADCAST) {
        val = entry->sock->broadcast;
    } else if (level == SOL_SOCKET && optname == SO_REUSEADDR) {
        val = entry->sock->reuseaddr;
    } else {
        val = 0; /* SO_KEEPALIVE / TCP_NODELAY and the rest: off/none */
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
    /* The count actually written, which for a caller that offered less than an
       int is less than an int: telling it four would be describing bytes that
       were never put there. */
    if (copy_to_user((void __user *)optlen_ptr, &olen, sizeof(olen)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/*
 * struct msghdr on i386: name (4), namelen (4), iov (4), iovlen (4), control
 * (4), controllen (4), flags (4). The offsets are the ABI and are written out,
 * not modelled, because a struct here would have to match a layout
 * this kernel does not otherwise depend on.
 */
#define MSGHDR_LEN 28

#define MSGHDR_NAME 0

#define MSGHDR_NAMELEN 4

#define MSGHDR_IOV 8

#define MSGHDR_IOVLEN 12

#define MSGHDR_CONTROLLEN 20

#define MSGHDR_FLAGS 24

/**
 * Receive one datagram into a scatter list (recvmsg, socketcall 17).
 *
 * The DHCP client is what needs this: it reads its packet socket through
 * `recvmsg`, not `read` so that it can look for `PACKET_AUXDATA` in the
 * control data, and finding none it verifies the UDP checksum itself. So the
 * scatter half is the whole of the work here. The control buffer is answered
 * with a length of zero, which is the true answer: this stack attaches no
 * ancillary data to anything.
 *
 * A datagram longer than the buffers is cut, and `msg_flags` says so with
 * MSG_TRUNC; with MSG_TRUNC in `flags` the call answers the datagram's whole
 * length, which netlink's programs, iproute2's ip among them, ask with
 * MSG_PEEK and no buffer before they read a message.
 *
 * The sender is reported through `msg_name`: a `sockaddr_in` for an AF_INET
 * socket, and for a packet socket the `sockaddr_ll` the frame came from, with
 * the sender's hardware address and who the frame was addressed to.
 */
static int32_t do_recvmsg(int fd, uint32_t msg_ptr, int flags)
{
    uint8_t hdr[MSGHDR_LEN];
    fd_entry_t *entry;
    uint32_t iov_user, name_ptr, namelen;
    uint32_t addr_len, addr_give;
    uint32_t iovlen;
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    uint32_t i;
    int32_t taken;
    int32_t whole = 0;

    if (copy_from_user(hdr, (const void __user *)msg_ptr, MSGHDR_LEN) != 0) {
        return -EFAULT;
    }
    entry = current_proc ? fd_get(current_proc->fds, fd) : (fd_entry_t *)0;
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    if (entry->sock->type != SOCK_DGRAM) {
        return -EOPNOTSUPP;
    }

    name_ptr = SC_LE32(hdr + MSGHDR_NAME);
    namelen = SC_LE32(hdr + MSGHDR_NAMELEN);
    iov_user = SC_LE32(hdr + MSGHDR_IOV);
    iovlen = SC_LE32(hdr + MSGHDR_IOVLEN);
    if (iovlen > (uint32_t)IOV_MAX_LOCAL) {
        return -EINVAL;
    }

    /* Every destination is checked before the datagram is taken, because
       taking it is what destroys it: a fault discovered half way through the
       copy would report an error for a message the machine no longer has.
       This is the pass `sys_readv` makes for the same reason. */
    if (!user_buffer_ok(msg_ptr, MSGHDR_LEN, 1)) {
        return -EFAULT;
    }
    /* Only what will be written is checked, and it is settled here rather than
       after the datagram is taken: a packet socket reports twenty bytes and an
       AF_INET one sixteen, however much the caller declared. Validating the
       whole declared capacity would refuse a call whose address fits
       perfectly. */
    addr_len = sockaddr_len_of(entry->sock);
    addr_give = namelen < addr_len ? namelen : addr_len;
    if (name_ptr && addr_give > 0 && !user_buffer_ok(name_ptr, addr_give, 1)) {
        return -EFAULT;
    }
    for (i = 0; i < iovlen; i++) {
        struct kernel_iovec iov;

        if (copy_from_user(
                &iov, (const void __user *)(iov_user + i * (uint32_t)sizeof(struct kernel_iovec)),
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

    /* A struct sockaddr_in is 16 bytes (family, port, address, and eight bytes
       of padding a caller may compare against sizeof) and a sockaddr_ll is 20.
       Either is written whole where it fits, truncated where the caller's
       buffer is smaller, and the true length is reported either way, which is
       what Linux does. */
    if (name_ptr && addr_len > 0) {
        uint8_t sa[SOCKADDR_LL_LEN];

        k_memset(sa, 0, sizeof(sa));
        if (entry->sock->family == AF_NETLINK) {
            sa[0] = (uint8_t)AF_NETLINK; /* the kernel, port id 0 */
        } else if (entry->sock->family == AF_PACKET) {
            if (packet_last_addr(entry->sock, sa) == 0) {
                addr_len = 0;
                addr_give = 0;
            }
        } else {
            sa[0] = (uint8_t)(AF_INET & 0xff);
            SC_WBE16(sa + 2, src_port);
            SC_WBE32(sa + 4, src_ip);
        }
        if (addr_give > 0 && copy_to_user((void __user *)name_ptr, sa, addr_give) != 0) {
            return -EFAULT;
        }
    }
    /* The true length, whatever the caller's capacity was: that is how a caller
       learns its buffer was too small. Zero for a packet socket, which has no
       sender to report. */
    SC_WLE32(hdr + MSGHDR_NAMELEN, name_ptr ? addr_len : 0u);
    SC_WLE32(hdr + MSGHDR_CONTROLLEN, 0);
    SC_WLE32(hdr + MSGHDR_FLAGS, whole > taken ? (uint32_t)MSG_TRUNC : 0u);
    if (copy_to_user((void __user *)msg_ptr, hdr, MSGHDR_LEN) != 0) {
        return -EFAULT;
    }
    return (flags & MSG_TRUNC) ? whole : taken;
}

/**
 * sendmsg(2) on an AF_INET or packet socket: the caller's buffers in order,
 * as one send. A stream sends them one after another and stops at the first
 * that goes short; a UDP socket gathers them into one datagram; a raw or
 * packet socket takes one buffer. Control messages carry nothing here and
 * are ignored.
 */
static int32_t do_sendmsg(int fd, uint32_t msg_ptr, int flags)
{
    uint8_t hdr[MSGHDR_LEN];
    struct kernel_iovec iov[8];
    uint8_t *gathered;
    fd_entry_t *entry;
    uint32_t iov_user, iovlen, name_ptr, namelen, i, total = 0;
    int32_t rc;

    if (copy_from_user(hdr, (const void __user *)msg_ptr, MSGHDR_LEN) != 0) {
        return -EFAULT;
    }
    entry = current_proc ? fd_get(current_proc->fds, fd) : (fd_entry_t *)0;
    if (!entry || entry->type != FD_TYPE_SOCKET || !entry->sock) {
        return not_a_socket(entry);
    }
    name_ptr = SC_LE32(hdr + MSGHDR_NAME);
    namelen = SC_LE32(hdr + MSGHDR_NAMELEN);
    iov_user = SC_LE32(hdr + MSGHDR_IOV);
    iovlen = SC_LE32(hdr + MSGHDR_IOVLEN);
    if (iovlen > (uint32_t)IOV_MAX_LOCAL) {
        return -EMSGSIZE;
    }
    if (socket_outside(entry->sock)) {
        return -ENETUNREACH;
    }
    if (entry->sock->type == SOCK_STREAM) {
        for (i = 0; i < iovlen; i++) {
            struct kernel_iovec one;

            if (copy_from_user(&one, (const void __user *)(iov_user + i * 8u), sizeof(one)) != 0) {
                return total ? (int32_t)total : -EFAULT;
            }
            if (one.iov_len == 0) {
                continue;
            }
            rc = do_sendto(fd, one.iov_base, (int)one.iov_len, flags, 0, 0);
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
        (entry->sock->family != AF_INET && entry->sock->family != AF_NETLINK)) {
        if (iovlen > 1) {
            return -EOPNOTSUPP;
        }
        return do_sendto(fd, iovlen ? iov[0].iov_base : 0u, iovlen ? (int)iov[0].iov_len : 0, flags,
                         name_ptr, (int)namelen);
    }
    for (i = 0; i < iovlen; i++) {
        uint32_t most = entry->sock->family == AF_NETLINK ? NETLINK_SEND_MAX : 1500u;

        if (iov[i].iov_len > most - total) {
            return -EMSGSIZE;
        }
        total += iov[i].iov_len;
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
        total += iov[i].iov_len;
    }
    if (entry->sock->family == AF_NETLINK) {
        rc = netlink_send(entry->sock, gathered, total);
    } else {
        rc = udp_sendto(entry, gathered, (int)total, name_ptr, (int)namelen);
    }
    kfree(gathered);
    return rc;
}

/* The calls on an AF_UNIX socket, which ipc/unix.c answers. */
static int32_t unix_call(int call, fd_entry_t *e, const uint32_t *args)
{
    switch (call) {
    case SC_BIND:
        return unix_bind(e, args[1], args[2]);
    case SC_CONNECT:
        return unix_connect(e, args[1], args[2]);
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
        return unix_sendto(e, args[1], args[2], (int)args[3], 0, 0);
    case SC_RECV:
        return unix_recvfrom(e, args[1], args[2], (int)args[3], 0, 0);
    case SC_SENDTO:
        return unix_sendto(e, args[1], args[2], (int)args[3], args[4], args[5]);
    case SC_RECVFROM:
        return unix_recvfrom(e, args[1], args[2], (int)args[3], args[4], args[5]);
    case SC_SHUTDOWN:
        return unix_shutdown(e, (int)args[1]);
    case SC_SETSOCKOPT:
        return unix_setsockopt(e, (int)args[1], (int)args[2], args[3], args[4]);
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

/* socket(2) and socketpair(2) of AF_UNIX: the type with its SOCK_CLOEXEC and
 * SOCK_NONBLOCK bits taken off, and any other bit refused. */
static int32_t unix_make(int call, const uint32_t *args)
{
    uint32_t flags = args[1] & ~(uint32_t)SOCK_TYPE_MASK;
    int type = (int)(args[1] & SOCK_TYPE_MASK);

    if (flags & ~(uint32_t)(SOCK_CLOEXEC | SOCK_NONBLOCK)) {
        return -EINVAL;
    }
    if (call == SC_SOCKETPAIR) {
        return unix_socketpair(type, (int)args[2], (int)flags, args[3]);
    }
    return unix_socket(type, (int)args[2], (int)flags);
}

static int32_t socket_dispatch(int call, uint32_t *args);

/**
 * sendmmsg and recvmmsg: sendmsg or recvmsg on the socket `args[0]` for each
 * of the `args[2]` headers at `args[1]`, with the flags `args[3]`, storing each
 * message's count in its header, as Linux's do; glibc's resolver sends its
 * two queries with sendmmsg. Returns the messages done, or the first error
 * when none was. After the first message recvmmsg waits for no more, with or
 * without MSG_WAITFORONE, so a call returns what has come; its timeout is not
 * read.
 */
static int32_t mmsg(int call, const uint32_t *args)
{
    uint32_t sub[3];
    uint32_t vlen = args[2] > MMSG_MAX ? MMSG_MAX : args[2];
    uint32_t i;
    int32_t rc = 0;

    sub[0] = args[0];
    sub[2] = args[3] & ~MSG_WAITFORONE;
    for (i = 0; i < vlen; i++) {
        sub[1] = args[1] + i * MMSGHDR_SIZE;
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
 * Multiplexed socket syscall (socketcall, syscall 102).
 *
 * EBX is the SC_* sub-call number, ECX a user pointer to its argument array.
 * Copies the right number of 32-bit args in one fault-checked read, then
 * dispatches to the matching do_* helper. Returns its result, or -EINVAL for an
 * unknown sub-call.
 */
long sys_socketcall(isr_params_t *params)
{
    int call = (int)SC_ARG(params, 0);
    uint32_t argp = SC_ARG(params, 1);
    uint32_t args[6];
    int nargs;

    switch (call) {
    case SC_PING:
        nargs = 1;
        break;
    case SC_PING_DATA:
        nargs = 3;
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

    /* Pull the argument array out of user memory in one fault-checked copy. */
    if (copy_from_user(args, (const void __user *)argp, (uint32_t)nargs * 4) != 0) {
        return -EFAULT;
    }
    return socket_dispatch(call, args);
}

/**
 * Run socket sub-call `call` with its arguments `args`, for socketcall and
 * for the direct socket calls. A call on an AF_UNIX socket, or one that
 * makes one, goes to ipc/unix.c; the rest to the calls above.
 */
static int32_t socket_dispatch(int call, uint32_t *args)
{
    fd_entry_t *e;

    if ((call == SC_SOCKET || call == SC_SOCKETPAIR) && args[0] == AF_UNIX) {
        return unix_make(call, args);
    }
    if (call == SC_SOCKETPAIR) {
        return -EOPNOTSUPP;
    }
    if (call == SC_SENDMMSG || call == SC_RECVMMSG) {
        return mmsg(call, args);
    }
    e = (call != SC_SOCKET && call != SC_PING && call != SC_PING_DATA && current_proc)
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
        return do_sendto((int)args[0], args[1], (int)args[2], (int)args[3], args[4], (int)args[5]);
    case SC_RECVFROM:
        return do_recvfrom((int)args[0], args[1], (int)args[2], (int)args[3], args[4], args[5]);
    case SC_PING:
        return do_ping(args[0]);
    case SC_PING_DATA: {
        uint16_t got = 0;
        uint8_t got_ttl = 0;
        int rc;

        if (args[2] < 1 || args[2] > 255) {
            return -EINVAL;
        }
        rc = icmp_ping_data(args[0], args[1], (uint8_t)args[2], &got, &got_ttl);
        return rc < 0 ? rc : (int32_t)((uint32_t)got | (uint32_t)got_ttl << 16);
    }
    case SC_CONNECT:
        return do_connect((int)args[0], args[1], (int)args[2]);
    case SC_LISTEN:
        return do_listen((int)args[0], (int)args[1]);
    case SC_ACCEPT:
        return do_accept((int)args[0], args[1], args[2], 0);
    case SC_ACCEPT4:
        return do_accept((int)args[0], args[1], args[2], args[3]);
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
        return do_send((int)args[0], args[1], (int)args[2], (int)args[3]);
    case SC_RECV:
        return do_recv((int)args[0], args[1], (int)args[2], (int)args[3]);
    case SC_RECVMSG:
        return do_recvmsg((int)args[0], args[1], (int)args[2]);
    case SC_SENDMSG:
        return do_sendmsg((int)args[0], args[1], (int)args[2]);
    default:
        return -EINVAL;
    }
}

/* The direct socket calls, numbers 359 to 373, which a C library built for a
 * Linux newer than 4.3 makes instead of socketcall: the same sub-calls with
 * their arguments in registers. */
static int32_t direct(int call, isr_params_t *params)
{
    uint32_t args[6];

    args[0] = SC_ARG(params, 0);
    args[1] = SC_ARG(params, 1);
    args[2] = SC_ARG(params, 2);
    args[3] = SC_ARG(params, 3);
    args[4] = SC_ARG(params, 4);
    args[5] = SC_ARG(params, 5);
    return socket_dispatch(call, args);
}

long sys_socket(isr_params_t *params)
{
    return direct(SC_SOCKET, params);
}

long sys_socketpair(isr_params_t *params)
{
    return direct(SC_SOCKETPAIR, params);
}

long sys_bind(isr_params_t *params)
{
    return direct(SC_BIND, params);
}

long sys_connect(isr_params_t *params)
{
    return direct(SC_CONNECT, params);
}

long sys_listen(isr_params_t *params)
{
    return direct(SC_LISTEN, params);
}

long sys_accept4(isr_params_t *params)
{
    return direct(SC_ACCEPT4, params);
}

long sys_getsockopt(isr_params_t *params)
{
    return direct(SC_GETSOCKOPT, params);
}

long sys_setsockopt(isr_params_t *params)
{
    return direct(SC_SETSOCKOPT, params);
}

long sys_getsockname(isr_params_t *params)
{
    return direct(SC_GETSOCKNAME, params);
}

long sys_getpeername(isr_params_t *params)
{
    return direct(SC_GETPEERNAME, params);
}

long sys_sendto(isr_params_t *params)
{
    return direct(SC_SENDTO, params);
}

long sys_sendmsg(isr_params_t *params)
{
    return direct(SC_SENDMSG, params);
}

long sys_recvfrom(isr_params_t *params)
{
    return direct(SC_RECVFROM, params);
}

long sys_recvmsg(isr_params_t *params)
{
    return direct(SC_RECVMSG, params);
}

long sys_shutdown(isr_params_t *params)
{
    return direct(SC_SHUTDOWN, params);
}

long sys_recvmmsg(isr_params_t *params)
{
    return direct(SC_RECVMMSG, params);
}

long sys_sendmmsg(isr_params_t *params)
{
    return direct(SC_SENDMMSG, params);
}
