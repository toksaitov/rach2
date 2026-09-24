/*
 * ethernet.c - Ethernet frame parsing and construction.
 *
 * ethernet_rx dispatches a received frame to ARP or IP by EtherType, offering
 * every ARP and IPv4 frame to the packet sockets on the way past, and drops
 * everything while the interface is down.
 * ethernet_tx prepends the 14-byte header and hands the frame to the driver.
 * Every frame passes one of the two, so they keep the interface's counters
 * that /proc/net/dev reads.
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

/**
 * Dispatch a received Ethernet frame to ARP or IP by EtherType.
 *
 * The EtherType is the big-endian 16-bit field at bytes 12..13 of the header.
 * Each protocol handler receives only the payload (the bytes after the 14-byte
 * header), so it never needs to know about Ethernet; the packet-socket tap is
 * given the whole frame, since the addresses in the header are part of what it
 * reports. The tap sees each ARP and IPv4 frame first and unconditionally,
 * since ip_rx drops what is not addressed to this machine, which is every
 * reply a DHCP client waits for; the kernel's own handler still runs after
 * it. Frames shorter than a header are dropped, and so is every frame while
 * an administrator has the interface down, since ethernet_tx would send no
 * reply.
 */
void ethernet_rx(struct netdev *nd, const uint8_t *data, uint16_t len)
{
    uint16_t etype;

    if (len < ETH_HDR_SIZE) {
        nd->rx_dropped++;
        return;
    }
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
 * Prepends the 14-byte header (destination MAC, the interface's own MAC as the
 * source, and the big-endian EtherType) ahead of the payload and hands it to
 * the driver of `out`. The payload must fit the 1500-byte MTU; an oversize
 * payload is dropped, not truncated. An interface an administrator took down
 * sends no IP and no ARP, but netconsole frames still leave: the log of a
 * machine whose network was just misconfigured is the one most worth having.
 * A frame is counted as sent only once the driver has taken it.
 */
int ethernet_tx(struct netdev *out, const uint8_t *dst_mac, uint16_t ethertype,
                const uint8_t *payload, uint16_t plen)
{
    uint16_t total;

    if (!out) {
        return NETDEV_TX_DROPPED; /* no interface reaches that address */
    }

    if (plen > 1500) {
        klog(KLOG_WARN, "eth", "tx: payload of %d bytes is too large, dropped", (int)plen);
        out->tx_dropped++;
        return ETHERNET_TX_TOOLONG;
    }
    if (!out->up && (ethertype == ETH_TYPE_IP || ethertype == ETH_TYPE_ARP)) {
        out->tx_dropped++;
        return NETDEV_TX_DROPPED;
    }

    k_memcpy(eth_tx_frame, dst_mac, 6);
    k_memcpy(eth_tx_frame + 6, out->mac, 6);
    eth_tx_frame[12] = (uint8_t)(ethertype >> 8);
    eth_tx_frame[13] = (uint8_t)(ethertype & 0xFF);
    k_memcpy(eth_tx_frame + ETH_HDR_SIZE, payload, plen);

    total = (uint16_t)(ETH_HDR_SIZE + plen);
    if (netdev_tx(out, eth_tx_frame, total) != NETDEV_TX_OK) {
        out->tx_dropped++;
        return NETDEV_TX_DROPPED;
    }
    out->tx_packets++;
    out->tx_bytes += total;
    return NETDEV_TX_OK;
}

void ethernet_note_rx_error(struct netdev *nd)
{
    nd->rx_errors++;
}

void ethernet_note_rx_drop(struct netdev *nd)
{
    nd->rx_dropped++;
}
