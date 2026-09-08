/*
 * ethernet.c - Ethernet frame parsing and construction.
 *
 * ethernet_rx dispatches a received frame to ARP or IP by EtherType, offering
 * every IPv4 frame to the packet sockets on the way past, and drops everything
 * while the interface is down.
 * ethernet_tx prepends the 14-byte header and hands the frame to the driver.
 */

#include "../../klog.h"
#include "ethernet.h"
#include "../net.h"
#include "../driver/netdev.h"
#include "../arp/arp.h"
#include "../ip/ip.h"
#include "../socket/packet.h"
#include "../netif.h"
#include "../../serial.h"
#include "../../lib/kstring.h"

/* TX assembly buffer at file scope: the kernel keeps no large stack frames. */
static uint8_t eth_tx_frame[ETH_HDR_SIZE + 1500];

/* What the interface has carried, for /proc/net/dev. Counted here rather than
   in the drivers, because this is the one place every frame passes. */
/*
 * The machine's counters, which are the first interface's. Every count is
 * kept on the interface it belongs to; this reads them back for the callers
 * that still ask about the machine rather than about an interface.
 */
static struct eth_counts counts;

const struct eth_counts *ethernet_counts(void)
{
    struct netdev *nd = netdev_first();

    if (nd) {
        counts.rx_packets = nd->rx_packets;
        counts.rx_bytes = nd->rx_bytes;
        counts.rx_errors = nd->rx_errors;
        counts.rx_dropped = nd->rx_dropped;
        counts.tx_packets = nd->tx_packets;
        counts.tx_bytes = nd->tx_bytes;
        counts.tx_dropped = nd->tx_dropped;
    }
    return &counts;
}

/**
 * Dispatch a received Ethernet frame to ARP or IP by EtherType.
 *
 * The EtherType is the big-endian 16-bit field at bytes 12..13 of the header.
 * Each protocol handler receives only the payload (the bytes after the 14-byte
 * header), so it never needs to know about Ethernet; the packet-socket tap is
 * given the whole frame, since the addresses in the header are part of what it
 * reports. Frames shorter than a header are dropped.
 */
void ethernet_rx(struct netdev *nd, const uint8_t *data, uint16_t len)
{
    uint16_t etype;

    if (len < ETH_HDR_SIZE) {
        nd->rx_dropped++;
        return;
    }
    /* An interface an administrator took down receives nothing either. The
       device is still handing frames up, but a machine that answered them
       while it was down would be a machine whose sockets saw traffic it could
       not reply to, since transmission is gated below. */
    if (!nd->up) {
        nd->rx_dropped++;
        return;
    }

    nd->rx_packets++;
    nd->rx_bytes += len;

    etype = (uint16_t)(((uint16_t)data[12] << 8) | data[13]);

    switch (etype) {
    case ETH_TYPE_ARP:
        packet_rx(nd, etype, data, len);
        arp_rx(nd, data + ETH_HDR_SIZE, (uint16_t)(len - ETH_HDR_SIZE));
        break;
    case ETH_TYPE_IP:
        /* Packet sockets first, and unconditionally: they exist because ip_rx
           drops what is not addressed to this machine, which is every reply a
           DHCP client is waiting for. The kernel's own handler still runs, so
           a program watching ARP does not stop the machine answering it. */
        packet_rx(nd, etype, data, len);
        ip_rx(data + ETH_HDR_SIZE, (uint16_t)(len - ETH_HDR_SIZE), 0);
        break;
    default:
        break;
    }
}

/**
 * Build an Ethernet frame and hand it to the driver.
 *
 * Prepends the 14-byte header (destination MAC, our `net_mac` source MAC, and
 * the big-endian EtherType) ahead of the payload and hands it to whichever
 * driver `netdev_probe` bound. The payload must fit the 1500-byte MTU; an
 * oversize payload is dropped, not truncated.
 */
void ethernet_tx(struct netdev *out, const uint8_t *dst_mac, uint16_t ethertype,
                 const uint8_t *payload, uint16_t plen)
{
    uint16_t total;

    if (!out) {
        return; /* no interface reaches that address */
    }

    if (plen > 1500) {
        klog(KLOG_WARN, "eth", "tx: payload of %d bytes is too large, dropped",
                      (int)plen);
        out->tx_dropped++;
        return;
    }
    /* An interface an administrator took down has no IP and answers no
       ARP. Netconsole is not gated, and that is deliberate: it is how a machine
       with no console is read, and a machine whose network was just
       misconfigured is exactly when its log is worth having. */
    if (!out->up && (ethertype == ETH_TYPE_IP || ethertype == ETH_TYPE_ARP)) {
        out->tx_dropped++;
        return;
    }

    k_memcpy(eth_tx_frame,     dst_mac, 6);
    k_memcpy(eth_tx_frame + 6, out->mac, 6);
    eth_tx_frame[12] = (uint8_t)(ethertype >> 8);
    eth_tx_frame[13] = (uint8_t)(ethertype & 0xFF);
    k_memcpy(eth_tx_frame + ETH_HDR_SIZE, payload, plen);

    total = (uint16_t)(ETH_HDR_SIZE + plen);
    /* Counted after the driver answers, not before it is asked: a frame the
     * card would not take did not leave, and counting it as carried is the
     * error this verdict exists to correct. */
    if (netdev_tx(out, eth_tx_frame, total) != NETDEV_TX_OK) {
        out->tx_dropped++;
        return;
    }
    out->tx_packets++;
    out->tx_bytes += total;
}

void ethernet_note_rx_error(struct netdev *nd)
{
    nd->rx_errors++;
}

void ethernet_note_rx_drop(struct netdev *nd)
{
    nd->rx_dropped++;
}
