#ifndef _ETHERNET_H_
#define _ETHERNET_H_

#include "../../lib/kint.h"

/* Ethernet header: destination MAC, source MAC, EtherType. */
#define ETH_HDR_SIZE    14

/* EtherType values. */
#define ETH_TYPE_IP     0x0800
#define ETH_TYPE_ARP    0x0806

/* The interface a frame arrived on, or whose counters are asked about. */
struct netdev;

/** Dispatch a received Ethernet frame to ARP or IP by EtherType. */
void ethernet_rx(struct netdev *nd, const uint8_t *data, uint16_t len);

/** Build an Ethernet frame and hand it to the driver. `ethertype` is in host
 * byte order (e.g. ETH_TYPE_IP) and is written big-endian on the wire.
 * Returns the driver's answer, `NETDEV_TX_OK` or `NETDEV_TX_DROPPED`, so a
 * sender that keeps what the card would not take can. */
int ethernet_tx(struct netdev *out, const uint8_t *dst_mac, uint16_t ethertype,
                const uint8_t *payload, uint16_t plen);

/* What the interface has carried since boot, and what it refused. Every frame
 * that arrives or leaves passes ethernet_rx or ethernet_tx, so this is where
 * it is counted, and /proc/net/dev is where it is read. The counts are 32-bit
 * and wrap, as Linux's do on a 32-bit machine.
 *
 * The three refusal counters are reported here rather than kept by each
 * driver, because a frame a driver would not send is one the interface did not
 * send, and the file that reports it has one column for each. A driver reports
 * what it refused through the three functions below; it does not keep the
 * interface's books. */
struct eth_counts {
    uint32_t rx_packets;
    uint32_t rx_bytes;
    uint32_t rx_errors;
    uint32_t rx_dropped;
    uint32_t tx_packets;
    uint32_t tx_bytes;
    uint32_t tx_dropped;
};

/** The interface's counters, live. */
const struct eth_counts *ethernet_counts(void);

/* A frame the hardware reported an error on: it reached the card and the card
 * says it is not sound. */
void ethernet_note_rx_error(struct netdev *nd);

/* A frame this machine could not take: one the driver cannot present whole,
 * or one that arrived with nowhere to put it. */
void ethernet_note_rx_drop(struct netdev *nd);

#endif /* _ETHERNET_H_ */
