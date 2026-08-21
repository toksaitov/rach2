#ifndef _ETHERNET_H_
#define _ETHERNET_H_

#include "../../lib/kint.h"

/* Ethernet header: destination MAC, source MAC, EtherType. */
#define ETH_HDR_SIZE    14

/* EtherType values. */
#define ETH_TYPE_IP     0x0800
#define ETH_TYPE_ARP    0x0806

/** Dispatch a received Ethernet frame to ARP or IP by EtherType. */
void ethernet_rx(const uint8_t *data, uint16_t len);

/** Build an Ethernet frame and hand it to the driver. `ethertype` is in host
 * byte order (e.g. ETH_TYPE_IP) and is written big-endian on the wire. */
void ethernet_tx(const uint8_t *dst_mac, uint16_t ethertype,
                 const uint8_t *payload, uint16_t plen);

/* What the interface has carried since boot: every frame passes ethernet_rx or
 * ethernet_tx, so this is where it is counted, and /proc/net/dev is where it is
 * read. The counts are 32-bit and wrap, as Linux's do on a 32-bit machine. */
struct eth_counts {
    uint32_t rx_packets;
    uint32_t rx_bytes;
    uint32_t tx_packets;
    uint32_t tx_bytes;
};

/** The interface's counters, live. */
const struct eth_counts *ethernet_counts(void);

#endif /* _ETHERNET_H_ */
