/*
 * ethernet.c: the link layer, as ethernet.h describes it: the received
 * frame given to ARP or IP by its EtherType, the frame sent built around its
 * payload, and the interface's counters, which both keep.
 */

#include "../../klog.h"
#include "../../lib/lockdep.h"
#include "ethernet.h"
#include "../net.h"
#include "../driver/netdev.h"
#include "../arp/arp.h"
#include "../ip/ip.h"
#include "../ip6/ip6.h"
#include "../socket/packet.h"
#include "../netif.h"
#include "../../serial.h"
#include "../../lib/kstring.h"

/* TX assembly buffer at file scope: the kernel keeps no large stack frames. */
static uint8_t eth_tx_frame[ETH_HDR_SIZE + ETH_MTU];

/**
 * The EtherType is the big-endian 16-bit field at bytes 12 and 13 of the
 * header. ARP and IP are given the payload alone, the bytes after the
 * header, and the packet sockets the whole frame, since the addresses in
 * the header are part of what they report. The packet sockets see each ARP,
 * IPv4 and IPv6 frame first and whatever its destination, since `ip_rx` drops
 * what is not addressed to this machine, which is every reply a DHCP client
 * waits for. Every frame on an interface that is down is dropped, since
 * `ethernet_tx` would send no reply.
 */
void ethernet_rx(struct netdev *nd, const uint8_t *data, uint16_t len)
{
    uint16_t etype;

    bkl_assert_held();
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
    case ETH_TYPE_PAE:
        packet_rx(nd, etype, data, len);
        break;
    case ETH_TYPE_ARP:
        packet_rx(nd, etype, data, len);
        arp_rx(nd, data + ETH_HDR_SIZE, (uint16_t)(len - ETH_HDR_SIZE));
        break;
    case ETH_TYPE_IP:
        packet_rx(nd, etype, data, len);
        ip_rx(data + ETH_HDR_SIZE, (uint16_t)(len - ETH_HDR_SIZE), 0);
        break;
    case ETH_TYPE_IPV6:
        packet_rx(nd, etype, data, len);
        ip6_rx(nd, data + ETH_HDR_SIZE, (uint16_t)(len - ETH_HDR_SIZE));
        break;
    default:
        break;
    }
}

/**
 * An interface an administrator took down sends no IPv4, IPv6 or ARP, and
 * none past its MTU, but a frame of another EtherType, such as
 * netconsole's, still leaves, up to the 1500 bytes of a frame: the log of a
 * machine whose network was just configured wrong is the one most worth
 * having. A card that says its link is down is given nothing, as Linux
 * gives a card nothing while its carrier is off: the frame could not leave,
 * and a card such as the RTL8168 counts each one it is given as a transmit
 * error.
 */
int ethernet_tx(struct netdev *out, const uint8_t *dst_mac, uint16_t ethertype,
                const uint8_t *payload, uint16_t plen)
{
    uint16_t total;
    int inet = ethertype == ETH_TYPE_IP || ethertype == ETH_TYPE_ARP || ethertype == ETH_TYPE_IPV6;

    if (!out) {
        return NETDEV_TX_DROPPED;
    }

    if (plen > ETH_MTU || (inet && plen > out->mtu)) {
        klog(KLOG_WARN, "eth", "tx: payload of %d bytes is too large, dropped", (int)plen);
        out->tx_dropped++;
        return ETHERNET_TX_TOOLONG;
    }
    if (!out->up && inet) {
        out->tx_dropped++;
        return NETDEV_TX_DROPPED;
    }
    if (!netdev_running(out)) {
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
