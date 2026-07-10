/*
 * ethernet.c - Ethernet frame parsing and construction.
 *
 * ethernet_rx dispatches a received frame to ARP or IP by EtherType.
 * ethernet_tx prepends the 14-byte header and hands the frame to the driver.
 */

#include "ethernet.h"
#include "../net.h"
#include "../driver/rtl8139.h"
#include "../arp/arp.h"
#include "../ip/ip.h"
#include "../../serial.h"
#include "../../lib/kstring.h"

/* TX assembly buffer at file scope: the kernel keeps no large stack frames. */
static uint8_t eth_tx_frame[ETH_HDR_SIZE + 1500];

/**
 * Dispatch a received Ethernet frame to ARP or IP by EtherType.
 *
 * The EtherType is the big-endian 16-bit field at bytes 12..13 of the header.
 * Each handler receives only the payload (the bytes after the 14-byte header),
 * so it never needs to know about Ethernet. Frames shorter than a header are
 * dropped.
 */
void ethernet_rx(const uint8_t *data, uint16_t len)
{
    uint16_t etype;

    if (len < ETH_HDR_SIZE) {
        return;
    }

    etype = (uint16_t)(((uint16_t)data[12] << 8) | data[13]);

    switch (etype) {
    case ETH_TYPE_ARP:
        arp_rx(data + ETH_HDR_SIZE, (uint16_t)(len - ETH_HDR_SIZE));
        break;
    case ETH_TYPE_IP:
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
 * the big-endian EtherType) ahead of the payload and transmits via the
 * RTL8139. The payload must fit the 1500-byte MTU; an oversize payload is
 * dropped, not truncated.
 */
void ethernet_tx(const uint8_t *dst_mac, uint16_t ethertype,
                 const uint8_t *payload, uint16_t plen)
{
    uint16_t total;

    if (plen > 1500) {
        serial_printf("ETH: TX payload too large (%d bytes), dropped\n",
                      (int)plen);
        return;
    }

    k_memcpy(eth_tx_frame,     dst_mac, 6);
    k_memcpy(eth_tx_frame + 6, net_mac, 6);
    eth_tx_frame[12] = (uint8_t)(ethertype >> 8);
    eth_tx_frame[13] = (uint8_t)(ethertype & 0xFF);
    k_memcpy(eth_tx_frame + ETH_HDR_SIZE, payload, plen);

    total = (uint16_t)(ETH_HDR_SIZE + plen);
    rtl8139_tx(eth_tx_frame, total);
}
