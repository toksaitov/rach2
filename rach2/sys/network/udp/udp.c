/*
 * udp.c: UDP, as udp.h describes it: the list of bindings, which maps a port
 * at an address of this machine, or at any (`local_ip` 0), to a socket, and
 * the datagrams received and sent. The 8-byte header is, big-endian, the
 * source port, destination port, length and checksum.
 */

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "udp.h"
#include "../../proc/process.h"
#include "../net.h"
#include "../socket/socket.h"
#include "../ip/ip.h"
#include "../core/byteorder.h"
#include "../../serial.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"

/* A destination port and the socket that receives its datagrams: one record
   from the heap for each binding, on a list, the newest first. */
typedef struct udp_binding {
    struct udp_binding *next;
    uint16_t port;
    struct socket *sock;
    uint32_t owner; /* the effective uid that bound it */
} udp_binding_t;

static udp_binding_t *bindings;

static uint8_t udp_tx_buf[UDP_HDR_SIZE + 1500];

/* Next ephemeral port to try (kept inside the 49152-65535 range). */
static uint16_t udp_next_ephemeral = 49152;

/** Whether a binding has `port`, at any address. */
static int udp_port_in_use(uint16_t port)
{
    const udp_binding_t *b;

    for (b = bindings; b; b = b->next) {
        if (b->port == port) {
            return 1;
        }
    }
    return 0;
}

/** Two bindings to different addresses do not meet, as on Linux:
 * 127.0.0.1 and an interface's address may each have a server on port 53. */
int udp_bind(uint16_t port, struct socket *sock)
{
    udp_binding_t *b;
    uint32_t flags;
    uint32_t ip = sock ? sock->local_ip : 0;
    uint32_t uid = current_proc ? current_proc->euid : 0;
    int met = 0, shared = sock && sock->reuseaddr;

    flags = net_lock();

    for (b = bindings; b; b = b->next) {
        uint32_t held = b->sock ? b->sock->local_ip : 0;

        if (b->port == port && (held == 0 || ip == 0 || held == ip)) {
            met = 1;
            if (!b->sock || !b->sock->reuseaddr || b->owner != uid) {
                shared = 0;
            }
        }
    }
    if (met && !shared) {
        net_unlock(flags);
        klog(KLOG_DEBUG, "udp", "bind: port %d is already bound", (int)port);
        return -1;
    }

    b = kzalloc(sizeof(*b));
    if (!b) {
        net_unlock(flags);
        klog(KLOG_WARN, "udp", "bind: no memory for a binding, port %d refused", (int)port);
        return -ENOMEM;
    }
    b->port = port;
    b->sock = sock;
    b->owner = current_proc ? current_proc->euid : 0;
    b->next = bindings;
    bindings = b;
    net_unlock(flags);
    return 0;
}

/**
 * The scan goes forward from `udp_next_ephemeral`, wrapping at the top of
 * the range, and stops at the first failure to allocate, since no other port
 * would help. A send from a socket without a port (`dgram_send`), a `bind`
 * to port 0 and a `connect` call this. The port is recorded in
 * `sock->local_port`, so that the socket's later sends do not bind a fresh
 * port each time and its close unbinds it.
 */
uint16_t udp_bind_ephemeral(struct socket *sock)
{
    int tries;
    uint16_t port;

    for (tries = 0; tries < 16384; tries++) {
        port = udp_next_ephemeral++;
        if (udp_next_ephemeral < 49152) { /* wrapped past 65535 */
            udp_next_ephemeral = 49152;
        }
        if (!udp_port_in_use(port)) {
            int rc = udp_bind(port, sock);

            if (rc == -ENOMEM) {
                return 0;
            }
            if (rc == 0) {
                sock->local_port = port;
                return port;
            }
        }
    }
    return 0;
}

/**
 * Bindings are matched by socket and not by port: two sockets may share a
 * port, and releasing the first entry with that port would take the other
 * one's binding away and leave this one's behind.
 */
void udp_unbind(struct socket *sock)
{
    udp_binding_t **link;
    uint32_t flags;

    flags = net_lock();
    link = &bindings;
    while (*link) {
        udp_binding_t *b = *link;

        if (b->sock == sock) {
            *link = b->next;
            kfree(b);
        } else {
            link = &b->next;
        }
    }
    net_unlock(flags);
}

/**
 * A length field below the header's size would underflow the payload's
 * length and hand the receive memory after it to user space. The socket
 * connected to the sender takes the datagram before any other, because it
 * asked for that sender by name, and a renewal's answer must not go to a
 * socket that is merely listening. The payload goes to `socket_udp_deliver`
 * with the sender's address and port.
 */
int udp_rx(uint32_t src_ip, uint32_t dst_ip, const uint8_t *data, uint16_t len)
{
    uint16_t dst_port, udp_len, payload_len;

    if (len < UDP_HDR_SIZE) {
        net_mib.udp_in_errors++;
        return 0;
    }

    dst_port = read16be(data + 2);
    udp_len = read16be(data + 4);

    if (udp_len < UDP_HDR_SIZE || udp_len > len) {
        net_mib.udp_in_errors++;
        return 0;
    }
    payload_len = (uint16_t)(udp_len - UDP_HDR_SIZE);

    {
        struct socket *listener = NULL;
        uint16_t src_port = read16be(data + 0);
        const udp_binding_t *b;

        for (b = bindings; b; b = b->next) {
            struct socket *cand = b->sock;

            if (b->port != dst_port || !cand || (cand->local_ip != 0 && cand->local_ip != dst_ip)) {
                continue;
            }
            if (cand->connected) {
                if (cand->remote_ip == src_ip && cand->remote_port == src_port) {
                    net_mib.udp_in_datagrams++;
                    socket_udp_deliver(cand, src_ip, src_port, data + UDP_HDR_SIZE, payload_len);
                    return 0;
                }
                continue;
            }
            if (!listener || (listener->local_ip == 0 && cand->local_ip != 0)) {
                listener = cand;
            }
        }
        if (!listener) {
            net_mib.udp_no_ports++;
            return -1;
        }
        net_mib.udp_in_datagrams++;
        socket_udp_deliver(listener, src_ip, src_port, data + UDP_HDR_SIZE, payload_len);
    }
    return 0;
}

void udp_refused(uint16_t local_port, uint32_t remote_ip, uint16_t remote_port)
{
    const udp_binding_t *b;

    for (b = bindings; b; b = b->next) {
        struct socket *s = b->sock;

        if (b->port == local_port && s && s->connected && s->remote_ip == remote_ip &&
            s->remote_port == remote_port) {
            s->so_error = ECONNREFUSED;
            wq_wake(&s->waiters, WQ_DATA);
        }
    }
}

/**
 * The largest payload is 1472 bytes, the 1500-byte MTU less the IPv4 and UDP
 * headers. The shared transmit buffer is filled and sent under `net_lock`,
 * since a sender in an interrupt must not enter it at the same time (net.h).
 */
int udp_tx(const struct socket *sock, uint32_t dst_ip, uint16_t dst_port, const uint8_t *payload,
           uint16_t plen)
{
    uint16_t udp_len;
    uint32_t flags;
    int r;

    if (plen > 1500) {
        klog(KLOG_WARN, "udp", "tx: payload of %d bytes is too large, refused", (int)plen);
        return -EMSGSIZE;
    }

    udp_len = (uint16_t)(UDP_HDR_SIZE + plen);

    flags = net_lock();

    write16be(udp_tx_buf + 0, sock->local_port);
    write16be(udp_tx_buf + 2, dst_port);
    write16be(udp_tx_buf + 4, udp_len);
    write16be(udp_tx_buf + 6, 0x0000); /* checksum disabled */
    k_memcpy(udp_tx_buf + UDP_HDR_SIZE, payload, plen);

    r = ip_tx_from(sock->local_ip, dst_ip, IP_PROTO_UDP, udp_tx_buf, udp_len, sock->ip_ttl);
    if (r == 0) {
        net_mib.udp_out_datagrams++;
    }

    net_unlock(flags);
    return r;
}
