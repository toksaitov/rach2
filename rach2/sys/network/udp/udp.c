/*
 * udp.c - User Datagram Protocol.
 *
 * Header (8 bytes, big-endian): src_port, dst_port, length, checksum. A static
 * table maps bound destination ports to sockets; udp_rx looks up the port and
 * hands the payload to socket_udp_deliver.
 */

#include "udp.h"
#include "../../proc/process.h"
#include "../net.h"
#include "../socket/socket.h"
#include "../ip/ip.h"
#include "../core/byteorder.h"
#include "../../serial.h"
#include "../../lib/kstring.h"

/* Maps a destination UDP port to the socket that should receive its data. */
typedef struct udp_binding {
    uint16_t       port;
    struct socket *sock;
    uint32_t       owner;  /* the effective uid that bound it */
    uint8_t        in_use;
} udp_binding_t;

static udp_binding_t port_table[UDP_PORT_TABLE_SIZE];

static uint8_t udp_tx_buf[UDP_HDR_SIZE + 1500];

/* Next ephemeral port to try (kept inside the 49152-65535 range). */
static uint16_t udp_next_ephemeral = 49152;

static int udp_port_in_use(uint16_t port)
{
    int i;

    for (i = 0; i < UDP_PORT_TABLE_SIZE; i++) {
        if (port_table[i].in_use && port_table[i].port == port) {
            return 1;
        }
    }
    return 0;
}

/**
 * Bind a socket to a UDP port so udp_rx can route inbound datagrams to it.
 *
 * Fails if the port is already bound or the port table is full. The table is
 * shared with udp_rx, which runs in IRQ context, so the update is done under
 * net_lock.
 *
 * @return 0 on success, -1 on conflict or a full table.
 */
int udp_bind(uint16_t port, struct socket *sock)
{
    int i;
    uint32_t flags;

    /* The port table is read by udp_rx in IRQ context; update it atomically. */
    flags = net_lock();

    /* A port already bound is taken again only when every socket on it asked
       for that, the new one included. A DHCP client needs exactly this: it
       keeps a socket bound to port 68 to listen on while it opens a second one
       bound to 68 to send its renewal from. It does not need, and must not
       have, a stranger's socket beside it: the connected socket takes the
       answer, so anyone who could share the port could take the client's
       replies by asking for them first. */
    if (udp_port_in_use(port)) {
        uint32_t uid = current_proc ? current_proc->euid : 0;
        int shared = sock && sock->reuseaddr;

        for (i = 0; shared && i < UDP_PORT_TABLE_SIZE; i++) {
            if (port_table[i].in_use && port_table[i].port == port &&
                (!port_table[i].sock || !port_table[i].sock->reuseaddr ||
                 port_table[i].owner != uid)) {
                shared = 0;
            }
        }
        if (!shared) {
            net_unlock(flags);
            serial_printf("UDP: port %d already bound\n", (int)port);
            return -1;
        }
    }

    for (i = 0; i < UDP_PORT_TABLE_SIZE; i++) {
        if (!port_table[i].in_use) {
            port_table[i].port   = port;
            port_table[i].sock   = sock;
            port_table[i].owner  = current_proc ? current_proc->euid : 0;
            port_table[i].in_use = 1;
            net_unlock(flags);
            return 0;
        }
    }

    net_unlock(flags);
    serial_printf("UDP: port table full, cannot bind port %d\n", (int)port);
    return -1;
}

/**
 * Bind a socket to a free ephemeral port in the 49152-65535 range.
 *
 * Scans forward from udp_next_ephemeral, wrapping at the top of the range, and
 * binds the first free port. do_sendto calls this to auto-bind an unbound
 * socket before its first send.
 *
 * @return the bound port, or 0 if none could be allocated.
 */
uint16_t udp_bind_ephemeral(struct socket *sock)
{
    int tries;
    int i;
    uint16_t port;
    uint32_t flags;
    int room = 0;

    /* A table with no free slot is answered at once. Walking sixteen thousand
       ports to find that out takes a long time and prints a line for each, and
       a caller holding the network lock across this call would hold interrupts
       off for all of it: a process that filled the table could stop the
       machine's clock by connecting one more socket. */
    flags = net_lock();
    for (i = 0; i < UDP_PORT_TABLE_SIZE; i++) {
        if (!port_table[i].in_use) {
            room = 1;
            break;
        }
    }
    net_unlock(flags);
    if (!room) {
        return 0;
    }

    for (tries = 0; tries < 16384; tries++) {
        port = udp_next_ephemeral++;
        if (udp_next_ephemeral < 49152) {  /* wrapped past 65535 */
            udp_next_ephemeral = 49152;
        }
        if (!udp_port_in_use(port)) {
            if (udp_bind(port, sock) == 0) {
                /* Record the port on the socket so the caller's later sends do
                 * not re-bind a fresh port each time and so close() unbinds it.
                 * (do_sendto auto-binds an unbound socket via this path.) */
                sock->local_port = port;
                return port;
            }
        }
    }
    return 0;
}

void udp_unbind(struct socket *sock)
{
    int i;
    uint32_t flags;

    /* By socket and not by port: two sockets may share a port, and releasing
       the first entry that names it would take the other one's binding away
       and leave this one's behind. */
    flags = net_lock();
    for (i = 0; i < UDP_PORT_TABLE_SIZE; i++) {
        if (port_table[i].in_use && port_table[i].sock == sock) {
            port_table[i].in_use = 0;
            port_table[i].sock = NULL;
        }
    }
    net_unlock(flags);
}

/**
 * Demultiplex one received UDP datagram to its bound socket.
 *
 * Called from IP receive in IRQ context. Drops the datagram if it is shorter
 * than the header or no socket is bound to the destination port. The payload is
 * handed to socket_udp_deliver along with the sender's address and port.
 *
 * @param data the UDP header followed by its payload.
 * @param len  bytes actually received, the upper bound on the wire length.
 */
void udp_rx(uint32_t src_ip, const uint8_t *data, uint16_t len)
{
    uint16_t dst_port, udp_len, payload_len;
    int i;

    if (len < UDP_HDR_SIZE) {
        return;
    }

    dst_port = read16be(data + 2);
    udp_len  = read16be(data + 4);

    /*
     * Validate the on-wire length before trusting it: it must cover at least
     * the header and must not exceed the bytes we actually received. Without
     * this check a malicious short length underflows and leaks adjacent RX
     * memory to userspace.
     */
    if (udp_len < UDP_HDR_SIZE || udp_len > len) {
        return;
    }
    payload_len = (uint16_t)(udp_len - UDP_HDR_SIZE);

    /* A port may have more than one socket on it. The one connected to the
       sender takes the datagram, because it asked for that sender by name; the
       first unconnected one takes it otherwise. Anything else would hand a
       renewal's answer to the socket that is merely listening. */
    {
        struct socket *listener = NULL;
        uint16_t src_port = read16be(data + 0);

        for (i = 0; i < UDP_PORT_TABLE_SIZE; i++) {
            struct socket *cand = port_table[i].sock;

            if (!port_table[i].in_use || port_table[i].port != dst_port || !cand) {
                continue;
            }
            if (cand->connected) {
                if (cand->remote_ip == src_ip && cand->remote_port == src_port) {
                    socket_udp_deliver(cand, src_ip, src_port, data + UDP_HDR_SIZE,
                                       payload_len);
                    return;
                }
                continue;
            }
            if (!listener) {
                listener = cand;
            }
        }
        if (listener) {
            socket_udp_deliver(listener, src_ip, src_port, data + UDP_HDR_SIZE, payload_len);
        }
    }
}

/**
 * Build a UDP datagram around the payload and hand it to IP for transmission.
 *
 * Drops payloads larger than 1500 bytes; this stack does not fragment. The
 * checksum field is left zero, which the protocol permits over IPv4 to signal
 * "no checksum". The shared TX buffer is filled and queued under net_lock.
 */
void udp_tx(uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
            const uint8_t *payload, uint16_t plen)
{
    uint16_t udp_len;
    uint32_t flags;

    if (plen > 1500) {
        serial_printf("UDP: payload too large (%d bytes), dropped\n",
                      (int)plen);
        return;
    }

    udp_len = (uint16_t)(UDP_HDR_SIZE + plen);

    /* Build and transmit atomically: the shared TX buffers must not be re-entered
     * by an IRQ-context sender (see the concurrency note in net.h). */
    flags = net_lock();

    write16be(udp_tx_buf + 0, src_port);
    write16be(udp_tx_buf + 2, dst_port);
    write16be(udp_tx_buf + 4, udp_len);
    write16be(udp_tx_buf + 6, 0x0000);   /* checksum disabled */
    k_memcpy(udp_tx_buf + UDP_HDR_SIZE, payload, plen);

    ip_tx(dst_ip, IP_PROTO_UDP, udp_tx_buf, udp_len);

    net_unlock(flags);
}
