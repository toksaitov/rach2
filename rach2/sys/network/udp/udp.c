/*
 * udp.c - User Datagram Protocol.
 *
 * Header (8 bytes, big-endian): src_port, dst_port, length, checksum.
 * A heap-allocated binding list maps destination ports to sockets, each
 * bound to one address of this machine or to any (`local_ip` 0).
 * udp_rx looks up the address and port and hands the payload to
 * socket_udp_deliver.
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

/* Maps a destination UDP port to the socket that should receive its data:
   one record from the heap per bound socket, on a list. */
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

/**
 * Bind a socket to a UDP port so udp_rx can route inbound datagrams to it.
 *
 * A binding takes the port at the socket's `local_ip`, and so meets another
 * on the same port when either is to any address or both are to the same
 * one, as on Linux: 127.0.0.1 and an interface's address may each have a
 * server on port 53. A port met that way may be shared only when the new
 * socket and every binding it meets enable `SO_REUSEADDR`, and every one was
 * created under the caller's effective uid. The function updates the binding
 * list under `net_lock`.
 *
 * @return 0 on success, -1 on a conflict, -ENOMEM with no memory.
 */
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
 * Bind a socket to a free ephemeral port in the 49152-65535 range.
 *
 * Scans forward from udp_next_ephemeral, wrapping at the top of the range, and
 * binds the first free port. A send from a socket without a port
 * (`dgram_send`), a `bind` to port 0 and a `connect` call this. The port is
 * recorded in `sock->local_port`, so the socket's later sends do not bind a
 * fresh port each time and its close unbinds it.
 *
 * @return the bound port, or 0 if none could be allocated.
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
                return 0; /* no port would help */
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
 * Remove every binding `sock` has, under `net_lock`.
 *
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
 * Demultiplex one received UDP datagram to its bound socket.
 *
 * Called with interrupts off from `ip_rx`. Drops the datagram if it is
 * shorter than the header, if its length field is below the header's size or
 * above the bytes received (a short length would underflow the payload
 * length and hand adjacent receive memory to user space), or if no socket
 * takes it. The checksum is not verified. The payload is handed to
 * socket_udp_deliver along with the sender's address and port.
 *
 * A socket bound to an address takes only datagrams to that address. A port
 * may have more than one socket on it. The one connected to the sender takes
 * the datagram, because it asked for that sender by name, and a renewal's
 * answer must not go to a socket that is merely listening; otherwise the
 * first unconnected one bound to the datagram's address takes it, or the
 * first bound to any address.
 *
 * @param data the UDP header followed by its payload.
 * @param len  bytes actually received, the upper bound on the wire length.
 * @return -1 when no socket has the port, which `ip_rx` answers with a port
 *         unreachable, and 0 otherwise, a malformed datagram included.
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

/**
 * Report a port unreachable from `remote_ip` for a datagram sent from
 * `local_port` to `remote_port`: a socket connected to that peer takes
 * ECONNREFUSED as its pending error and its waiters wake. An unconnected
 * socket is not told, as Linux tells none without IP_RECVERR.
 */
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
 * Build a UDP datagram around the payload and hand it to IP for transmission.
 *
 * The maximum payload is 1472 bytes, leaving room for the IPv4 and UDP
 * headers within the 1500-byte MTU. Larger payloads return `-EMSGSIZE`: from
 * here above 1500 bytes, the transmit buffer's size, and from `ip_tx` below.
 * The checksum field is left zero, which IPv4 permits to disable checksumming.
 * The shared TX buffer is filled and queued under net_lock, since an
 * IRQ-context sender must not reenter it (net.h). The return value is the
 * contract in udp.h.
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
